// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _SMOLALSA_H
#define _SMOLALSA_H

/*
 * sound/asound.h pulls in a lot of standard headers so we need some janky
 * hacks to make it work with nolibc.
 */
#ifndef NOLIBC
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#endif

#ifdef NOLIBC
#define _TIME_H
#define _STDLIB_H
#define _ENDIAN_H
#define _SYS_IOCTL_H
#endif

#include <sound/asound.h>

#ifndef SMOLALSA_DEVICEPATH
#define SMOLALSA_DEVICEPATH	"/dev/snd/pcmC0D0p"
#endif

/*
 * The other two nodes of the same card. No udev here, so these are the names
 * the kernel makes itself: 116,16 playback, 116,24 capture, 116,0 mixer.
 */
#ifndef SMOLALSA_CAPTUREPATH
#define SMOLALSA_CAPTUREPATH	"/dev/snd/pcmC0D0c"
#endif

#ifndef SMOLALSA_CONTROLPATH
#define SMOLALSA_CONTROLPATH	"/dev/snd/controlC0"
#endif

/* How long the blocking helpers will sit on a card before giving up on it */
#ifndef SMOLALSA_WAITMS
#define SMOLALSA_WAITMS		1000
#endif

/* A card that is there but busy is worth knowing from one that is not */
#define SMOLALSA_NODEVICE	(-1)
#define SMOLALSA_BUSY		(-2)

struct smolalsa_pcm {
	int fd;
	unsigned int rate;
	unsigned int channels;
	unsigned int framebytes;
	unsigned int periodframes;
	unsigned int periods;
	unsigned int capture;		/* recording rather than playing */
	unsigned int xruns;		/* times it ran dry, or over */
	unsigned long boundary;		/* where the pointers wrap */
};

static inline struct snd_mask *smolalsa_mask(struct snd_pcm_hw_params *params, int which)
{
	return &params->masks[which - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

static inline struct snd_interval *smolalsa_interval(struct snd_pcm_hw_params *params,
						     int which)
{
	return &params->intervals[which - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

/*
 * Start by saying yes to everything. The kernel narrows what it is given down
 * to what the hardware can do, so anything not asked for is left to it.
 */
static inline void smolalsa_anything(struct snd_pcm_hw_params *params)
{
	unsigned int i;

	memset(params, 0, sizeof(*params));

	for (i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; i++)
		memset(params->masks[i].bits, 0xff, sizeof(params->masks[i].bits));

	for (i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
	     i++) {
		params->intervals[i].min = 0;
		params->intervals[i].max = ~0u;
	}

	params->rmask = ~0u;
	params->cmask = 0;
	params->info = ~0u;
}

static inline void smolalsa_pick(struct snd_pcm_hw_params *params, int which, unsigned int bit)
{
	struct snd_mask *mask = smolalsa_mask(params, which);

	memset(mask->bits, 0, sizeof(mask->bits));
	mask->bits[bit >> 5] = 1u << (bit & 31);
}

static inline int smolalsa_picked(struct snd_pcm_hw_params *params, int which,
				  unsigned int bit)
{
	struct snd_mask *mask = smolalsa_mask(params, which);

	return (mask->bits[bit >> 5] & (1u << (bit & 31))) != 0;
}

static inline void smolalsa_exactly(struct snd_pcm_hw_params *params, int which,
				    unsigned int value)
{
	struct snd_interval *interval = smolalsa_interval(params, which);

	interval->min = value;
	interval->max = value;
	interval->integer = 1;
	interval->openmin = 0;
	interval->openmax = 0;
	interval->empty = 0;
}

/*
 * A floor rather than a value, which is how the period size wants asking for:
 * a card that will only hand over 1024 frames at a time should get on with
 * that rather than turn a request for 512 into a failed open.
 */
static inline void smolalsa_atleast(struct snd_pcm_hw_params *params, int which,
				    unsigned int value)
{
	smolalsa_interval(params, which)->min = value;
}

/*
 * What the kernel settled on. Not every driver sets the integer flag on the
 * way back, so a minimum that has met its maximum counts as settled too.
 */
static inline unsigned int smolalsa_chosen(struct snd_pcm_hw_params *params, int which)
{
	struct snd_interval *interval = smolalsa_interval(params, which);

	if (interval->integer || interval->min == interval->max)
		return interval->max;

	return 0;
}

static inline void smolalsa_close(struct smolalsa_pcm *pcm)
{
	if (pcm->fd >= 0)
		close(pcm->fd);

	pcm->fd = -1;
}

#define __smolalsa_cleanup_pcm __attribute__((cleanup(smolalsa_close)))

/* Throw away what is queued and get ready to start again */
static inline int smolalsa_prepare(struct smolalsa_pcm *pcm)
{
	if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_PREPARE, 0) < 0)
		return -errno;

	return 0;
}

/* Stop where it is and drop whatever had not been played or picked up yet */
static inline int smolalsa_drop(struct smolalsa_pcm *pcm)
{
	if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_DROP, 0) < 0)
		return -errno;

	return 0;
}

/* Let what is queued play out and then stop, which here returns straight away */
static inline int smolalsa_drain(struct smolalsa_pcm *pcm)
{
	if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_DRAIN, 0) < 0)
		return -errno;

	return 0;
}

/*
 * Open a card, signed 16 bit interleaved, and tell it what to expect. Both
 * directions come through here.
 *
 * The period is how much the card takes at a time and the number of them is
 * how far ahead it will hold: 512 frames at 22050 with four of them is about
 * 90ms of slack. It is asked for as a minimum and read back afterwards, so
 * everything in struct smolalsa_pcm is what the kernel chose rather than what
 * was asked for. The fd is non-blocking, since the caller has a frame to draw.
 */
static inline int smolalsa_open_stream(struct smolalsa_pcm *pcm, const char *path,
				       unsigned int capture, unsigned int rate,
				       unsigned int channels, unsigned int periodframes,
				       unsigned int periods)
{
	struct snd_pcm_hw_params hw;
	struct snd_pcm_sw_params sw;
	int fd, ret;

	memset(pcm, 0, sizeof(*pcm));
	pcm->fd = -1;

	if (!path)
		path = capture ? SMOLALSA_CAPTUREPATH : SMOLALSA_DEVICEPATH;

	fd = open(path, (capture ? O_RDONLY : O_WRONLY) | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return errno == EBUSY ? SMOLALSA_BUSY : SMOLALSA_NODEVICE;

	smolalsa_anything(&hw);
	smolalsa_pick(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	smolalsa_pick(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	smolalsa_pick(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	smolalsa_exactly(&hw, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
	smolalsa_exactly(&hw, SNDRV_PCM_HW_PARAM_FRAME_BITS, 16 * channels);
	smolalsa_exactly(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, channels);
	smolalsa_exactly(&hw, SNDRV_PCM_HW_PARAM_RATE, rate);
	smolalsa_atleast(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, periodframes);
	smolalsa_exactly(&hw, SNDRV_PCM_HW_PARAM_PERIODS, periods);

	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) {
		ret = -errno;
		close(fd);
		return ret;
	}

	pcm->capture = capture ? 1 : 0;
	pcm->rate = smolalsa_chosen(&hw, SNDRV_PCM_HW_PARAM_RATE);
	pcm->channels = smolalsa_chosen(&hw, SNDRV_PCM_HW_PARAM_CHANNELS);
	pcm->periodframes = smolalsa_chosen(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
	pcm->periods = smolalsa_chosen(&hw, SNDRV_PCM_HW_PARAM_PERIODS);
	if (!pcm->rate)
		pcm->rate = rate;
	if (!pcm->channels)
		pcm->channels = channels;
	if (!pcm->periodframes)
		pcm->periodframes = periodframes;
	if (!pcm->periods)
		pcm->periods = periods;
	pcm->framebytes = 2 * pcm->channels;

	memset(&sw, 0, sizeof(sw));
	sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
	sw.period_step = 1;
	sw.avail_min = pcm->periodframes;
	/*
	 * Playing starts once there is a period in. Recording has nothing
	 * to wait for, so one frame, or the card never starts at all.
	 */
	sw.start_threshold = capture ? 1 : pcm->periodframes;
	sw.stop_threshold = pcm->periodframes * pcm->periods;
	sw.silence_threshold = 0;
	sw.silence_size = 0;

	/*
	 * Where the ring counter wraps: a power of two multiple of the
	 * buffer, as large as will fit. The kernel hands its own back in
	 * the same call, and that is what the pointers count up to.
	 */
	sw.boundary = pcm->periodframes * pcm->periods;
	while (sw.boundary * 2 < 0x40000000)
		sw.boundary *= 2;

	if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
		ret = -errno;
		close(fd);
		return ret;
	}
	pcm->boundary = (unsigned long)sw.boundary;

	pcm->fd = fd;

	ret = smolalsa_prepare(pcm);
	if (ret < 0) {
		smolalsa_close(pcm);
		return ret;
	}

	return 0;
}

/* Open a card for playing */
static inline int smolalsa_open(struct smolalsa_pcm *pcm, const char *path,
				unsigned int rate, unsigned int channels,
				unsigned int periodframes, unsigned int periods)
{
	return smolalsa_open_stream(pcm, path, 0, rate, channels, periodframes, periods);
}

/* Open a card for recording, which is the same card and a different node */
static inline int smolalsa_open_capture(struct smolalsa_pcm *pcm, const char *path,
					unsigned int rate, unsigned int channels,
					unsigned int periodframes, unsigned int periods)
{
	return smolalsa_open_stream(pcm, path, 1, rate, channels, periodframes, periods);
}

/*
 * Hand over as many frames as will fit and say how many that was, which may be
 * none. Running dry means the last frame took too long rather than anything
 * being wrong, so the card is set going again and nothing went in this time.
 */
static inline int smolalsa_write(struct smolalsa_pcm *pcm, const void *frames,
				 unsigned int nframes)
{
	struct snd_xferi xfer;
	int ret;

	if (pcm->fd < 0)
		return 0;

	xfer.buf = (void *)frames;
	xfer.frames = nframes;
	xfer.result = 0;

	ret = ioctl(pcm->fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer);
	if (ret < 0) {
		if (errno == EAGAIN)
			return 0;

		if (errno == EPIPE || errno == ESTRPIPE) {
			pcm->xruns++;
			smolalsa_prepare(pcm);
			return 0;
		}

		return -errno;
	}

	return (int)xfer.result;
}

/*
 * The same the other way round. The first read is what starts a recording, so
 * a card that has only just been opened has nothing ready yet. Overrunning is
 * counted and shrugged off like running dry, since the frames are gone either
 * way.
 */
static inline int smolalsa_read(struct smolalsa_pcm *pcm, void *frames,
				unsigned int nframes)
{
	struct snd_xferi xfer;
	int ret;

	if (pcm->fd < 0)
		return 0;

	xfer.buf = frames;
	xfer.frames = nframes;
	xfer.result = 0;

	ret = ioctl(pcm->fd, SNDRV_PCM_IOCTL_READI_FRAMES, &xfer);
	if (ret < 0) {
		if (errno == EAGAIN)
			return 0;

		if (errno == EPIPE || errno == ESTRPIPE) {
			pcm->xruns++;
			smolalsa_prepare(pcm);
			return 0;
		}

		return -errno;
	}

	return (int)xfer.result;
}

/*
 * Wait for room for a period, or a period to hand over: 1 when there is
 * something to do, 0 on the timeout, -errno if the wait itself failed. For
 * programs with nothing else to do meanwhile -- one with a frame to draw
 * should write what fits, draw, and come back.
 */
static inline int smolalsa_wait(struct smolalsa_pcm *pcm, int timeoutms)
{
	struct pollfd pfd;
	int ret;

	if (pcm->fd < 0)
		return 0;

	pfd.fd = pcm->fd;
	pfd.events = pcm->capture ? POLLIN : POLLOUT;
	pfd.revents = 0;

	do {
		ret = poll(&pfd, 1, timeoutms);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0)
		return -errno;

	return ret ? 1 : 0;
}

/*
 * Hand over the lot, waiting on the card in between. A card that has had no
 * room for a whole second is stuck, and saying so beats waiting for ever.
 */
static inline int smolalsa_write_all(struct smolalsa_pcm *pcm, const void *frames,
				     unsigned int nframes)
{
	const unsigned char *at = frames;
	unsigned int done = 0;

	while (done < nframes) {
		int ret = smolalsa_write(pcm, at + (unsigned long)done * pcm->framebytes,
					 nframes - done);

		if (ret < 0)
			return ret;

		if (!ret) {
			ret = smolalsa_wait(pcm, SMOLALSA_WAITMS);
			if (ret < 0)
				return ret;
			if (!ret)
				return -ETIMEDOUT;
			continue;
		}

		done += (unsigned int)ret;
	}

	return (int)done;
}

/* And take the lot, the same way */
static inline int smolalsa_read_all(struct smolalsa_pcm *pcm, void *frames,
				    unsigned int nframes)
{
	unsigned char *at = frames;
	unsigned int done = 0;

	while (done < nframes) {
		int ret = smolalsa_read(pcm, at + (unsigned long)done * pcm->framebytes,
					nframes - done);

		if (ret < 0)
			return ret;

		if (!ret) {
			ret = smolalsa_wait(pcm, SMOLALSA_WAITMS);
			if (ret < 0)
				return ret;
			if (!ret)
				return -ETIMEDOUT;
			continue;
		}

		done += (unsigned int)ret;
	}

	return (int)done;
}

struct smolalsa_state {
	int state;			/* SNDRV_PCM_STATE_* */
	unsigned long hwptr;		/* frames the card has been through */
	unsigned long applptr;		/* frames handed over */
	unsigned long avail;		/* room to write, or frames to read */
	long delay;			/* frames still to come out */
};

static inline const char *smolalsa_statename(int state)
{
	switch (state) {
	case SNDRV_PCM_STATE_OPEN:		return "open";
	case SNDRV_PCM_STATE_SETUP:		return "setup";
	case SNDRV_PCM_STATE_PREPARED:		return "prepared";
	case SNDRV_PCM_STATE_RUNNING:		return "running";
	case SNDRV_PCM_STATE_XRUN:		return "xrun";
	case SNDRV_PCM_STATE_DRAINING:		return "draining";
	case SNDRV_PCM_STATE_PAUSED:		return "paused";
	case SNDRV_PCM_STATE_SUSPENDED:		return "suspended";
	case SNDRV_PCM_STATE_DISCONNECTED:	return "disconnected";
	default:				return "?";
	}
}

/*
 * Where the card has got to. Both pointers count frames since the stream was
 * prepared and wrap at pcm->boundary, so hw_ptr against what was handed over
 * says whether the card is keeping up without timing anything.
 */
static inline int smolalsa_status(struct smolalsa_pcm *pcm, struct smolalsa_state *state)
{
	struct snd_pcm_status status;
	int ret;

	memset(&status, 0, sizeof(status));

	ret = ioctl(pcm->fd, SNDRV_PCM_IOCTL_STATUS, &status);
	if (ret < 0)
		return -errno;

	state->state = status.state;
	state->hwptr = (unsigned long)status.hw_ptr;
	state->applptr = (unsigned long)status.appl_ptr;
	state->avail = (unsigned long)status.avail;
	state->delay = (long)status.delay;

	return 0;
}

/* Frames that can go in now, or that are waiting to be taken out */
static inline long smolalsa_avail(struct smolalsa_pcm *pcm)
{
	struct smolalsa_state state;
	int ret;

	ret = smolalsa_status(pcm, &state);
	if (ret < 0)
		return ret;

	return (long)state.avail;
}

/* Frames between what has been handed over and what has come out of the card */
static inline long smolalsa_delay(struct smolalsa_pcm *pcm)
{
	snd_pcm_sframes_t frames = 0;

	if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_DELAY, &frames) < 0)
		return -errno;

	return (long)frames;
}

/* Times the card has run dry, or over, since it was opened */
static inline unsigned int smolalsa_xruns(struct smolalsa_pcm *pcm)
{
	return pcm->xruns;
}

/*
 * The mixer. Everything a card lets anyone change is an element: a switch, a
 * number, or one of a list of names. Elements are found by name, and the
 * kernel does that lookup itself as long as the numeric id is left at zero.
 */
struct smolalsa_ctl {
	int fd;
	unsigned int count;		/* elements the card had when opened */
};

struct smolalsa_ctl_elem {
	struct snd_ctl_elem_id id;	/* numeric id, interface, name, index */
	int type;			/* SNDRV_CTL_ELEM_TYPE_* */
	unsigned int access;
	unsigned int count;		/* values, one per channel */
	long min, max, step;		/* numbers */
	unsigned int items;		/* names, if it is a list of them */
};

static inline const char *smolalsa_ctl_typename(int type)
{
	switch (type) {
	case SNDRV_CTL_ELEM_TYPE_BOOLEAN:	return "BOOLEAN";
	case SNDRV_CTL_ELEM_TYPE_INTEGER:	return "INTEGER";
	case SNDRV_CTL_ELEM_TYPE_ENUMERATED:	return "ENUMERATED";
	case SNDRV_CTL_ELEM_TYPE_BYTES:		return "BYTES";
	case SNDRV_CTL_ELEM_TYPE_IEC958:	return "IEC958";
	case SNDRV_CTL_ELEM_TYPE_INTEGER64:	return "INTEGER64";
	default:				return "NONE";
	}
}

static inline void smolalsa_ctl_close(struct smolalsa_ctl *ctl)
{
	if (ctl->fd >= 0)
		close(ctl->fd);

	ctl->fd = -1;
}

/* How many elements the card has: ask for none of them and read the count */
static inline int smolalsa_ctl_count(struct smolalsa_ctl *ctl, unsigned int *count)
{
	struct snd_ctl_elem_list list;

	memset(&list, 0, sizeof(list));

	if (ioctl(ctl->fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0)
		return -errno;

	*count = list.count;

	return 0;
}

static inline int smolalsa_ctl_open(struct smolalsa_ctl *ctl, const char *path)
{
	int fd, ret;

	memset(ctl, 0, sizeof(*ctl));
	ctl->fd = -1;

	if (!path)
		path = SMOLALSA_CONTROLPATH;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return errno == EBUSY ? SMOLALSA_BUSY : SMOLALSA_NODEVICE;

	ctl->fd = fd;

	ret = smolalsa_ctl_count(ctl, &ctl->count);
	if (ret < 0) {
		smolalsa_ctl_close(ctl);
		return ret;
	}

	return 0;
}

/*
 * Fill in up to max element ids, and say how many that was and how many there
 * are. Nothing here allocates, so the caller brings the room.
 */
static inline int smolalsa_ctl_list(struct smolalsa_ctl *ctl, struct snd_ctl_elem_id *ids,
				    unsigned int max, unsigned int *used,
				    unsigned int *total)
{
	struct snd_ctl_elem_list list;

	memset(&list, 0, sizeof(list));
	memset(ids, 0, max * sizeof(*ids));
	list.offset = 0;
	list.space = max;
	list.pids = ids;

	if (ioctl(ctl->fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0)
		return -errno;

	if (used)
		*used = list.used;
	if (total)
		*total = list.count;

	return 0;
}

/* What an element is: its type, how many values it has, and what they may be */
static inline int smolalsa_ctl_info(struct smolalsa_ctl *ctl,
				    const struct snd_ctl_elem_id *id,
				    struct smolalsa_ctl_elem *elem)
{
	struct snd_ctl_elem_info info;

	memset(&info, 0, sizeof(info));
	info.id = *id;

	if (ioctl(ctl->fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0)
		return -errno;

	memset(elem, 0, sizeof(*elem));
	elem->id = info.id;
	elem->type = info.type;
	elem->access = info.access;
	elem->count = info.count;

	if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER ||
	    info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN) {
		elem->min = info.value.integer.min;
		elem->max = info.value.integer.max;
		elem->step = info.value.integer.step;
	}

	if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED)
		elem->items = info.value.enumerated.items;

	return 0;
}

/*
 * Find an element by name on a given interface. The kernel matches on the
 * name whenever the numeric id is zero, so this is one ioctl rather than a
 * walk of the list, and the numeric id comes back filled in.
 */
static inline int smolalsa_ctl_find_iface(struct smolalsa_ctl *ctl, int iface,
					  const char *name, unsigned int index,
					  struct smolalsa_ctl_elem *elem)
{
	struct snd_ctl_elem_id id;
	unsigned int len = (unsigned int)strlen(name);

	if (len >= sizeof(id.name))
		return -EINVAL;

	memset(&id, 0, sizeof(id));
	id.numid = 0;
	id.iface = iface;
	id.index = index;
	memcpy(id.name, name, len);

	return smolalsa_ctl_info(ctl, &id, elem);
}

/* The mixer interface at index 0, which is what a name on its own means */
static inline int smolalsa_ctl_find(struct smolalsa_ctl *ctl, const char *name,
				    struct smolalsa_ctl_elem *elem)
{
	return smolalsa_ctl_find_iface(ctl, SNDRV_CTL_ELEM_IFACE_MIXER, name, 0, elem);
}

static inline int smolalsa_ctl_read(struct smolalsa_ctl *ctl,
				    const struct snd_ctl_elem_id *id,
				    struct snd_ctl_elem_value *value)
{
	memset(value, 0, sizeof(*value));
	value->id = *id;

	if (ioctl(ctl->fd, SNDRV_CTL_IOCTL_ELEM_READ, value) < 0)
		return -errno;

	return 0;
}

static inline int smolalsa_ctl_write(struct smolalsa_ctl *ctl,
				     const struct snd_ctl_elem_id *id,
				     struct snd_ctl_elem_value *value)
{
	value->id = *id;

	if (ioctl(ctl->fd, SNDRV_CTL_IOCTL_ELEM_WRITE, value) < 0)
		return -errno;

	return 0;
}

/*
 * The name of one item of a list element. There is no way to ask for all of
 * them at once, so a list of four names is four calls.
 */
static inline int smolalsa_ctl_item(struct smolalsa_ctl *ctl,
				    const struct snd_ctl_elem_id *id,
				    unsigned int item, char *name, unsigned int len)
{
	struct snd_ctl_elem_info info;
	unsigned int n;

	if (!len)
		return -EINVAL;

	memset(&info, 0, sizeof(info));
	info.id = *id;
	info.value.enumerated.item = item;

	if (ioctl(ctl->fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0)
		return -errno;

	if (info.type != SNDRV_CTL_ELEM_TYPE_ENUMERATED)
		return -EINVAL;

	if (item >= info.value.enumerated.items)
		return -ENOENT;

	n = (unsigned int)strlen(info.value.enumerated.name);
	if (n > len - 1)
		n = len - 1;

	memcpy(name, info.value.enumerated.name, n);
	name[n] = 0;

	return 0;
}

/*
 * Read an element by name into vals, one per channel. A switch reads back 0 or
 * 1 and a list reads back which item it is on, so everything that is not a
 * pile of bytes comes out as a number.
 */
static inline int smolalsa_ctl_get(struct smolalsa_ctl *ctl, const char *name,
				   long *vals, unsigned int max, unsigned int *count)
{
	struct snd_ctl_elem_value value;
	struct smolalsa_ctl_elem elem;
	unsigned int i, n;
	int ret;

	if (!max)
		return -EINVAL;

	ret = smolalsa_ctl_find(ctl, name, &elem);
	if (ret < 0)
		return ret;

	ret = smolalsa_ctl_read(ctl, &elem.id, &value);
	if (ret < 0)
		return ret;

	n = elem.count > max ? max : elem.count;

	for (i = 0; i < n; i++) {
		if (elem.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED)
			vals[i] = (long)value.value.enumerated.item[i];
		else if (elem.type == SNDRV_CTL_ELEM_TYPE_INTEGER64)
			vals[i] = (long)value.value.integer64.value[i];
		else
			vals[i] = value.value.integer.value[i];
	}

	if (count)
		*count = n;

	return 0;
}

/*
 * Set every channel of an element to the same thing, which is what a volume on
 * the command line means. A number out of range is clamped rather than
 * refused: asking for more than the card has should end up at its loudest.
 */
static inline int smolalsa_ctl_set(struct smolalsa_ctl *ctl, const char *name, long val)
{
	struct snd_ctl_elem_value value;
	struct smolalsa_ctl_elem elem;
	unsigned int i;
	int ret;

	ret = smolalsa_ctl_find(ctl, name, &elem);
	if (ret < 0)
		return ret;

	switch (elem.type) {
	case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
		val = val ? 1 : 0;
		break;
	case SNDRV_CTL_ELEM_TYPE_INTEGER:
	case SNDRV_CTL_ELEM_TYPE_INTEGER64:
		if (val < elem.min)
			val = elem.min;
		if (val > elem.max)
			val = elem.max;
		break;
	case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
		if (val < 0 || (unsigned int)val >= elem.items)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	/* The write carries the whole array, so read first and change what exists */
	ret = smolalsa_ctl_read(ctl, &elem.id, &value);
	if (ret < 0)
		return ret;

	for (i = 0; i < elem.count; i++) {
		if (elem.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED)
			value.value.enumerated.item[i] = (unsigned int)val;
		else if (elem.type == SNDRV_CTL_ELEM_TYPE_INTEGER64)
			value.value.integer64.value[i] = val;
		else
			value.value.integer.value[i] = val;
	}

	return smolalsa_ctl_write(ctl, &elem.id, &value);
}

/* Set a list element by the name of the item, since "Mic" is what a person has */
static inline int smolalsa_ctl_set_item(struct smolalsa_ctl *ctl, const char *name,
					const char *item)
{
	struct smolalsa_ctl_elem elem;
	char have[64];
	unsigned int i;
	int ret;

	ret = smolalsa_ctl_find(ctl, name, &elem);
	if (ret < 0)
		return ret;

	if (elem.type != SNDRV_CTL_ELEM_TYPE_ENUMERATED)
		return -EINVAL;

	for (i = 0; i < elem.items; i++) {
		ret = smolalsa_ctl_item(ctl, &elem.id, i, have, sizeof(have));
		if (ret < 0)
			return ret;

		if (!strcmp(have, item))
			return smolalsa_ctl_set(ctl, name, (long)i);
	}

	return -ENOENT;
}

/* A WAV file. Only 16 bit PCM, since that is what the card is set to. */
#define SMOLALSA_WAV_HDRSZ	44		/* RIFF, fmt and data, no more */

struct smolalsa_wav {
	unsigned int format;		/* 1 is plain PCM, 0xfffe is extensible */
	unsigned int channels;
	unsigned int rate;
	unsigned int bits;
	unsigned int dataoffset;	/* bytes from the start of the file */
	unsigned int databytes;		/* 0 when the file does not say */
};

static inline unsigned int smolalsa_le16(const unsigned char *at)
{
	return (unsigned int)at[0] | ((unsigned int)at[1] << 8);
}

static inline unsigned int smolalsa_le32(const unsigned char *at)
{
	return (unsigned int)at[0] | ((unsigned int)at[1] << 8) |
	       ((unsigned int)at[2] << 16) | ((unsigned int)at[3] << 24);
}

static inline void smolalsa_putle16(unsigned char *at, unsigned int value)
{
	at[0] = (unsigned char)(value & 0xff);
	at[1] = (unsigned char)((value >> 8) & 0xff);
}

static inline void smolalsa_putle32(unsigned char *at, unsigned int value)
{
	at[0] = (unsigned char)(value & 0xff);
	at[1] = (unsigned char)((value >> 8) & 0xff);
	at[2] = (unsigned char)((value >> 16) & 0xff);
	at[3] = (unsigned char)((value >> 24) & 0xff);
}

/*
 * Walk the chunks in the front of a WAV and pick out the format and where the
 * samples start. What is between fmt and data is usually a name or a
 * timestamp, so it has to be stepped over rather than assumed away.
 *
 * A file being written as it is played puts nothing, or all ones, in the data
 * size; both come back as zero, meaning read until the file ends.
 */
static inline int smolalsa_wav_parse(const void *buf, unsigned int len,
				     struct smolalsa_wav *wav)
{
	const unsigned char *at = buf;
	unsigned int off = 12, gotfmt = 0;

	if (len < 12)
		return -EINVAL;

	if (memcmp(at, "RIFF", 4) || memcmp(at + 8, "WAVE", 4))
		return -EINVAL;

	memset(wav, 0, sizeof(*wav));

	while (off + 8 <= len) {
		const unsigned char *id = at + off;
		unsigned int size = smolalsa_le32(at + off + 4);
		unsigned int body = off + 8;

		if (!memcmp(id, "fmt ", 4)) {
			if (size < 16 || body + 16 > len)
				return -EINVAL;

			wav->format = smolalsa_le16(at + body);
			wav->channels = smolalsa_le16(at + body + 2);
			wav->rate = smolalsa_le32(at + body + 4);
			wav->bits = smolalsa_le16(at + body + 14);
			gotfmt = 1;
		} else if (!memcmp(id, "data", 4)) {
			if (!gotfmt)
				return -EINVAL;

			wav->dataoffset = body;
			wav->databytes = size == ~0u ? 0 : size;

			if (wav->format != 1 && wav->format != 0xfffe)
				return -EINVAL;

			if (wav->bits != 16 || !wav->channels || !wav->rate)
				return -EINVAL;

			return 0;
		}

		/* chunks are padded to an even length */
		off = body + size + (size & 1);
		if (off <= body)
			return -EINVAL;
	}

	return -EINVAL;
}

/*
 * Write the 44 byte header of a plain 16 bit WAV. Recording does not know the
 * size until it stops, so put zero in and write it again over the top at the
 * end.
 */
static inline unsigned int smolalsa_wav_header(unsigned char *hdr, unsigned int rate,
					       unsigned int channels, unsigned int bits,
					       unsigned int databytes)
{
	unsigned int block = channels * (bits / 8);

	memset(hdr, 0, SMOLALSA_WAV_HDRSZ);
	memcpy(hdr, "RIFF", 4);
	smolalsa_putle32(hdr + 4, 36 + databytes);
	memcpy(hdr + 8, "WAVE", 4);
	memcpy(hdr + 12, "fmt ", 4);
	smolalsa_putle32(hdr + 16, 16);
	smolalsa_putle16(hdr + 20, 1);			/* plain PCM */
	smolalsa_putle16(hdr + 22, channels);
	smolalsa_putle32(hdr + 24, rate);
	smolalsa_putle32(hdr + 28, rate * block);	/* bytes a second */
	smolalsa_putle16(hdr + 32, block);
	smolalsa_putle16(hdr + 34, bits);
	memcpy(hdr + 36, "data", 4);
	smolalsa_putle32(hdr + 40, databytes);

	return SMOLALSA_WAV_HDRSZ;
}

/*
 * A sine, for a test tone. nolibc has no floating point, so it is a table: a
 * quarter of a wave in 256 steps plus the end of it, 32767 * sin(k * pi / 512)
 * rounded. The other three quarters are this one mirrored.
 */
#define SMOLALSA_SINE_QUARTER	256
#define SMOLALSA_SINE_POINTS	(4 * SMOLALSA_SINE_QUARTER)

static const int16_t smolalsa_sine_quarter[SMOLALSA_SINE_QUARTER + 1] = {
	     0,    201,    402,    603,    804,   1005,   1206,   1407,
	  1608,   1809,   2009,   2210,   2410,   2611,   2811,   3012,
	  3212,   3412,   3612,   3811,   4011,   4210,   4410,   4609,
	  4808,   5007,   5205,   5404,   5602,   5800,   5998,   6195,
	  6393,   6590,   6786,   6983,   7179,   7375,   7571,   7767,
	  7962,   8157,   8351,   8545,   8739,   8933,   9126,   9319,
	  9512,   9704,   9896,  10087,  10278,  10469,  10659,  10849,
	 11039,  11228,  11417,  11605,  11793,  11980,  12167,  12353,
	 12539,  12725,  12910,  13094,  13279,  13462,  13645,  13828,
	 14010,  14191,  14372,  14553,  14732,  14912,  15090,  15269,
	 15446,  15623,  15800,  15976,  16151,  16325,  16499,  16673,
	 16846,  17018,  17189,  17360,  17530,  17700,  17869,  18037,
	 18204,  18371,  18537,  18703,  18868,  19032,  19195,  19357,
	 19519,  19680,  19841,  20000,  20159,  20317,  20475,  20631,
	 20787,  20942,  21096,  21250,  21403,  21554,  21705,  21856,
	 22005,  22154,  22301,  22448,  22594,  22739,  22884,  23027,
	 23170,  23311,  23452,  23592,  23731,  23870,  24007,  24143,
	 24279,  24413,  24547,  24680,  24811,  24942,  25072,  25201,
	 25329,  25456,  25582,  25708,  25832,  25955,  26077,  26198,
	 26319,  26438,  26556,  26674,  26790,  26905,  27019,  27133,
	 27245,  27356,  27466,  27575,  27683,  27790,  27896,  28001,
	 28105,  28208,  28310,  28411,  28510,  28609,  28706,  28803,
	 28898,  28992,  29085,  29177,  29268,  29358,  29447,  29534,
	 29621,  29706,  29791,  29874,  29956,  30037,  30117,  30195,
	 30273,  30349,  30424,  30498,  30571,  30643,  30714,  30783,
	 30852,  30919,  30985,  31050,  31113,  31176,  31237,  31297,
	 31356,  31414,  31470,  31526,  31580,  31633,  31685,  31736,
	 31785,  31833,  31880,  31926,  31971,  32014,  32057,  32098,
	 32137,  32176,  32213,  32250,  32285,  32318,  32351,  32382,
	 32412,  32441,  32469,  32495,  32521,  32545,  32567,  32589,
	 32609,  32628,  32646,  32663,  32678,  32692,  32705,  32717,
	 32728,  32737,  32745,  32752,  32757,  32761,  32765,  32766,
	 32767,
};

/*
 * A whole cycle is 65536 of phase, so a step of freq * 65536 / rate walks one
 * at the right speed and the table is reached by dropping the low six bits.
 */
static inline int smolalsa_sine(unsigned int phase)
{
	unsigned int i = (phase >> 6) & (SMOLALSA_SINE_POINTS - 1);

	if (i < SMOLALSA_SINE_QUARTER)
		return smolalsa_sine_quarter[i];

	if (i < 2 * SMOLALSA_SINE_QUARTER)
		return smolalsa_sine_quarter[2 * SMOLALSA_SINE_QUARTER - i];

	if (i < 3 * SMOLALSA_SINE_QUARTER)
		return -smolalsa_sine_quarter[i - 2 * SMOLALSA_SINE_QUARTER];

	return -smolalsa_sine_quarter[4 * SMOLALSA_SINE_QUARTER - i];
}

struct smolalsa_tone {
	unsigned int phase;		/* where in the cycle, out of 65536 */
	unsigned int step;		/* how far along a frame goes */
	int level;			/* 0 to 32767 */
};

/*
 * Set a tone going. The step is a whole number of 1/65536ths of a cycle, so the
 * note can be out by up to rate/65536 Hz -- under a hertz at 48k, and every
 * sum here stays a 32 bit one.
 */
static inline void smolalsa_tone_init(struct smolalsa_tone *tone, unsigned int rate,
				      unsigned int freq, unsigned int percent)
{
	memset(tone, 0, sizeof(*tone));

	if (percent > 100)
		percent = 100;

	tone->level = (int)((32767u * percent) / 100u);
	tone->step = rate ? (freq * 65536u) / rate : 0;
}

/* Fill frames of interleaved samples, the same tone in every channel */
static inline void smolalsa_tone_fill(struct smolalsa_tone *tone, int16_t *frames,
				      unsigned int nframes, unsigned int channels)
{
	unsigned int f, c;

	for (f = 0; f < nframes; f++) {
		int sample = (smolalsa_sine(tone->phase) * tone->level) >> 15;

		for (c = 0; c < channels; c++)
			frames[f * channels + c] = (int16_t)sample;

		tone->phase = (tone->phase + tone->step) & 0xffff;
	}
}

/* Time in whole milliseconds off the monotonic clock */
static inline unsigned long smolalsa_now_ms(void)
{
	struct timespec now = { 0, 0 };

	clock_gettime(CLOCK_MONOTONIC, &now);

	return (unsigned long)now.tv_sec * 1000ul + (unsigned long)(now.tv_nsec / 1000000);
}

/*
 * How long a number of frames lasts. Multiplying by a thousand first would wrap
 * a 32 bit long after about 89 seconds at 48k, so the division is split.
 */
static inline unsigned long smolalsa_frames_ms(unsigned int rate, unsigned long frames)
{
	if (!rate)
		return 0;

	return (frames / rate) * 1000ul + (frames % rate) * 1000ul / rate;
}

#endif /* _SMOLALSA_H */
