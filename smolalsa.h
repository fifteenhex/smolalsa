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
 * Open a card for playing, signed 16 bit interleaved, and tell it what to
 * expect.
 *
 * The period is how much the card takes at a time and the number of them is
 * how far ahead it will hold: 512 frames at 22050 with four of them is about
 * 90ms of slack. It is asked for as a minimum and read back afterwards, so
 * everything in struct smolalsa_pcm is what the kernel chose rather than what
 * was asked for. The fd is non-blocking, since the caller has a frame to draw.
 */
static inline int smolalsa_open(struct smolalsa_pcm *pcm, const char *path,
				unsigned int rate, unsigned int channels,
				unsigned int periodframes, unsigned int periods)
{
	struct snd_pcm_hw_params hw;
	struct snd_pcm_sw_params sw;
	int fd, ret;

	memset(pcm, 0, sizeof(*pcm));
	pcm->fd = -1;

	if (!path)
		path = SMOLALSA_DEVICEPATH;

	fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
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
	/* Playing starts once there is a period in */
	sw.start_threshold = pcm->periodframes;
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
	pfd.events = POLLOUT;
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

#endif /* _SMOLALSA_H */
