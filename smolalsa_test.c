// SPDX-License-Identifier: GPL-3.0-or-later

#include "smolalsa.h"

#ifndef NOLIBC
#include <stdio.h>
#include <string.h>
#include <time.h>
#endif

#define RATE		22050
#define PERIOD		512
#define PERIODS		4
#define SECONDS		2

static unsigned int failures;

/* A failed check is a line number and a condition, and the run carries on */
#define CHECK(cond)								\
	do {									\
		if (!(cond)) {							\
			printf("wrong at line %d: %s\n", __LINE__, #cond);	\
			failures++;						\
		}								\
	} while (0)

/*
 * An ioctl number carries the size of what it takes, so a structure that comes
 * out a different size here than the kernel makes it is not a mismatched call,
 * it is a call that does not exist. This is what says the header's held-off
 * libc includes are safe, and snd_pcm_status, with the timestamps in it, is
 * the one that would go first.
 */
static void check_ioctls(void)
{
	CHECK(_IOC_SIZE(SNDRV_PCM_IOCTL_HW_PARAMS) == sizeof(struct snd_pcm_hw_params));
	CHECK(_IOC_SIZE(SNDRV_PCM_IOCTL_SW_PARAMS) == sizeof(struct snd_pcm_sw_params));
	CHECK(_IOC_SIZE(SNDRV_PCM_IOCTL_STATUS) == sizeof(struct snd_pcm_status));
	CHECK(_IOC_SIZE(SNDRV_PCM_IOCTL_WRITEI_FRAMES) == sizeof(struct snd_xferi));
	CHECK(_IOC_SIZE(SNDRV_PCM_IOCTL_READI_FRAMES) == sizeof(struct snd_xferi));
	CHECK(_IOC_SIZE(SNDRV_CTL_IOCTL_ELEM_LIST) == sizeof(struct snd_ctl_elem_list));
	CHECK(_IOC_SIZE(SNDRV_CTL_IOCTL_ELEM_INFO) == sizeof(struct snd_ctl_elem_info));
	CHECK(_IOC_SIZE(SNDRV_CTL_IOCTL_ELEM_READ) == sizeof(struct snd_ctl_elem_value));
	CHECK(_IOC_SIZE(SNDRV_CTL_IOCTL_ELEM_WRITE) == sizeof(struct snd_ctl_elem_value));

	printf("structures: hw params %u, sw params %u, status %u, element info %u\n",
	       (unsigned int)sizeof(struct snd_pcm_hw_params),
	       (unsigned int)sizeof(struct snd_pcm_sw_params),
	       (unsigned int)sizeof(struct snd_pcm_status),
	       (unsigned int)sizeof(struct snd_ctl_elem_info));
}

/* Saying yes to everything, then narrowing it down to one thing at a time */
static void check_params(void)
{
	struct snd_pcm_hw_params hw;
	unsigned int i, w, ones = 0;

	smolalsa_anything(&hw);

	for (i = SNDRV_PCM_HW_PARAM_FIRST_MASK; i <= SNDRV_PCM_HW_PARAM_LAST_MASK; i++) {
		struct snd_mask *mask = smolalsa_mask(&hw, (int)i);

		for (w = 0; w < sizeof(mask->bits) / sizeof(mask->bits[0]); w++)
			if (mask->bits[w] == ~0u)
				ones++;
	}
	CHECK(ones == 3 * (SNDRV_MASK_MAX / 32));
	CHECK(hw.rmask == ~0u);
	CHECK(hw.cmask == 0);

	for (i = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL;
	     i++) {
		struct snd_interval *interval = smolalsa_interval(&hw, (int)i);

		CHECK(interval->min == 0);
		CHECK(interval->max == ~0u);
		CHECK(interval->openmin == 0 && interval->openmax == 0);
		CHECK(interval->integer == 0 && interval->empty == 0);
	}

	/* a pick lands in the right mask, on the right bit, and on nothing else */
	smolalsa_pick(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	CHECK(smolalsa_picked(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE));
	CHECK(!smolalsa_picked(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S32_LE));
	CHECK(hw.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[0] ==
	      1u << SNDRV_PCM_FORMAT_S16_LE);
	/* and leaves the other two masks alone */
	CHECK(smolalsa_picked(&hw, SNDRV_PCM_HW_PARAM_ACCESS,
			      SNDRV_PCM_ACCESS_RW_INTERLEAVED));
	CHECK(smolalsa_picked(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD));

	/* a format above 64 is a different word of the mask, not a lost bit */
	smolalsa_pick(&hw, SNDRV_PCM_HW_PARAM_FORMAT, 200);
	CHECK(smolalsa_picked(&hw, SNDRV_PCM_HW_PARAM_FORMAT, 200));
	CHECK(hw.masks[SNDRV_PCM_HW_PARAM_FORMAT -
		       SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[200 / 32] == 1u << (200 % 32));

	/* exactly is one value and says so; at least is a floor and does not */
	smolalsa_exactly(&hw, SNDRV_PCM_HW_PARAM_RATE, 44100);
	CHECK(smolalsa_chosen(&hw, SNDRV_PCM_HW_PARAM_RATE) == 44100);
	CHECK(smolalsa_interval(&hw, SNDRV_PCM_HW_PARAM_RATE)->integer);

	smolalsa_atleast(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 256);
	CHECK(smolalsa_interval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min == 256);
	CHECK(smolalsa_interval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->max == ~0u);
	CHECK(smolalsa_chosen(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE) == 0);

	/* what a card that narrowed it without setting the flag hands back */
	smolalsa_interval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->max = 1024;
	smolalsa_interval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min = 1024;
	CHECK(smolalsa_chosen(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE) == 1024);
}

static void check_wav(void)
{
	unsigned char buf[128];
	struct smolalsa_wav wav;

	/* what is written reads back */
	CHECK(smolalsa_wav_header(buf, 44100, 2, 16, 8000) == SMOLALSA_WAV_HDRSZ);
	CHECK(smolalsa_wav_parse(buf, SMOLALSA_WAV_HDRSZ, &wav) == 0);
	CHECK(wav.rate == 44100);
	CHECK(wav.channels == 2);
	CHECK(wav.bits == 16);
	CHECK(wav.format == 1);
	CHECK(wav.dataoffset == SMOLALSA_WAV_HDRSZ);
	CHECK(wav.databytes == 8000);
	CHECK(smolalsa_le32(buf + 4) == 36 + 8000);
	CHECK(smolalsa_le32(buf + 28) == 44100 * 4);	/* bytes a second */
	CHECK(smolalsa_le16(buf + 32) == 4);		/* bytes a frame */

	/*
	 * A file with something between fmt and data, of an odd length so there
	 * is a pad byte after it as well, which is the case that catches anyone
	 * who assumed the samples start at 44.
	 */
	memset(buf, 0, sizeof(buf));
	memcpy(buf, "RIFF", 4);
	smolalsa_putle32(buf + 4, 100);
	memcpy(buf + 8, "WAVE", 4);
	memcpy(buf + 12, "fmt ", 4);
	smolalsa_putle32(buf + 16, 16);
	smolalsa_putle16(buf + 20, 1);
	smolalsa_putle16(buf + 22, 1);
	smolalsa_putle32(buf + 24, 8000);
	smolalsa_putle32(buf + 28, 16000);
	smolalsa_putle16(buf + 32, 2);
	smolalsa_putle16(buf + 34, 16);
	memcpy(buf + 36, "LIST", 4);
	smolalsa_putle32(buf + 40, 5);
	memcpy(buf + 44, "INFOx", 5);
	memcpy(buf + 50, "data", 4);
	smolalsa_putle32(buf + 54, 64);
	CHECK(smolalsa_wav_parse(buf, sizeof(buf), &wav) == 0);
	CHECK(wav.rate == 8000);
	CHECK(wav.channels == 1);
	CHECK(wav.dataoffset == 58);
	CHECK(wav.databytes == 64);

	/* a size of all ones means the writer did not know either */
	smolalsa_putle32(buf + 54, ~0u);
	CHECK(smolalsa_wav_parse(buf, sizeof(buf), &wav) == 0);
	CHECK(wav.databytes == 0);

	/* eight bit is not something the card is set up for */
	smolalsa_putle16(buf + 34, 8);
	CHECK(smolalsa_wav_parse(buf, sizeof(buf), &wav) < 0);
	smolalsa_putle16(buf + 34, 16);

	/* and nothing that is not a WAV gets halfway through */
	CHECK(smolalsa_wav_parse(buf, 4, &wav) < 0);
	memcpy(buf, "RIFX", 4);
	CHECK(smolalsa_wav_parse(buf, sizeof(buf), &wav) < 0);
	memcpy(buf, "RIFF", 4);
	memcpy(buf + 50, "datb", 4);
	CHECK(smolalsa_wav_parse(buf, sizeof(buf), &wav) < 0);
}

static void check_sine(void)
{
	struct smolalsa_tone tone;
	int16_t frames[1024];
	unsigned int i;
	long sum = 0;
	int peak = 0;

	/* the quarter wave climbs from nothing to full scale and never dips */
	CHECK(smolalsa_sine_quarter[0] == 0);
	CHECK(smolalsa_sine_quarter[SMOLALSA_SINE_QUARTER] == 32767);
	for (i = 0; i < SMOLALSA_SINE_QUARTER; i++)
		CHECK(smolalsa_sine_quarter[i] <= smolalsa_sine_quarter[i + 1]);

	/* the four corners of the cycle */
	CHECK(smolalsa_sine(0) == 0);
	CHECK(smolalsa_sine(16384) == 32767);
	CHECK(smolalsa_sine(32768) == 0);
	CHECK(smolalsa_sine(49152) == -32767);

	/* half a cycle on is the same upside down, whatever the phase */
	for (i = 1; i < 32768; i += 137)
		CHECK(smolalsa_sine(i) == -smolalsa_sine(i + 32768));

	/*
	 * And each quarter mirrors the one before it, on table points only: 64
	 * of phase is one step of the 1024 and anything between two steps takes
	 * the lower one, so any other phase mirrors onto its neighbour.
	 */
	for (i = 0; i < SMOLALSA_SINE_POINTS / 2; i++)
		CHECK(smolalsa_sine(i * 64) ==
		      smolalsa_sine((SMOLALSA_SINE_POINTS / 2 - i) * 64));

	/* a sine and a cosine still square up to one, in whole numbers */
	for (i = 0; i < 65536; i += 521) {
		int s = smolalsa_sine(i), c = smolalsa_sine(i + 16384);
		long r = ((long)s * s + (long)c * c) >> 15;

		CHECK(r > 32700 && r < 32800);
	}

	/* a kilohertz at 48k is 48 frames a cycle */
	smolalsa_tone_init(&tone, 48000, 1000, 100);
	CHECK(tone.step == (1000 * 65536) / 48000);
	CHECK(tone.level == 32767);
	smolalsa_tone_fill(&tone, frames, 48, 1);
	CHECK(frames[0] == 0);
	for (i = 0; i < 48; i++) {
		if (frames[i] > peak)
			peak = frames[i];
		if (-frames[i] > peak)
			peak = -frames[i];
		sum += frames[i];
	}
	CHECK(peak > 32000);			/* it gets all the way up */
	CHECK(sum > -1000 && sum < 1000);	/* and is not sitting off centre */

	/* half as loud is half as far, and both channels get the same */
	smolalsa_tone_init(&tone, 48000, 1000, 50);
	CHECK(tone.level == 16383);
	smolalsa_tone_fill(&tone, frames, 24, 2);
	peak = 0;
	for (i = 0; i < 48; i += 2) {
		CHECK(frames[i] == frames[i + 1]);
		if (frames[i] > peak)
			peak = frames[i];
	}
	CHECK(peak > 16000 && peak <= 16383);

	/* and the phase stays inside one cycle however long it runs */
	smolalsa_tone_init(&tone, 8000, 3000, 100);
	for (i = 0; i < 10000; i++) {
		smolalsa_tone_fill(&tone, frames, 1, 1);
		CHECK(tone.phase <= 0xffff);
	}
}

static void check_time(void)
{
	unsigned long then, now;

	CHECK(smolalsa_frames_ms(48000, 48000) == 1000);
	CHECK(smolalsa_frames_ms(48000, 24000) == 500);
	CHECK(smolalsa_frames_ms(44100, 44100 * 3) == 3000);
	CHECK(smolalsa_frames_ms(48000, 0) == 0);
	CHECK(smolalsa_frames_ms(0, 48000) == 0);
	/* ten minutes, which is where multiplying first would have wrapped */
	CHECK(smolalsa_frames_ms(48000, 48000ul * 600) == 600000);

	then = smolalsa_now_ms();
	now = smolalsa_now_ms();
	CHECK(then != 0);
	CHECK(now >= then);
}

/* Nothing there is its own answer, and it leaves nothing open */
static void check_nodevice(void)
{
	struct smolalsa_pcm pcm;

	CHECK(smolalsa_open(&pcm, "/dev/snd/smolalsa-no-such-card", RATE, 1, PERIOD,
			    PERIODS) == SMOLALSA_NODEVICE);
	CHECK(pcm.fd == -1);
	CHECK(smolalsa_open_capture(&pcm, "/dev/snd/smolalsa-no-such-card", RATE, 1,
				    PERIOD, PERIODS) == SMOLALSA_NODEVICE);
	CHECK(pcm.fd == -1);
	/* and writing to what never opened is nothing written, not a crash */
	CHECK(smolalsa_write(&pcm, "xx", 1) == 0);
	CHECK(smolalsa_read(&pcm, &pcm, 1) == 0);
	CHECK(smolalsa_wait(&pcm, 0) == 0);
}

/* A square wave that fades, which is a beep and needs no arithmetic */
static void fill(int16_t *frames, unsigned int n, unsigned int at, unsigned int total)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		unsigned int t = at + i;
		unsigned int period = RATE / 440;	/* an A, near enough */
		int level = (int)(32767 - (32767 * (long)t) / total);

		frames[i] = (int16_t)((t % period) < period / 2 ? level : -level);
	}
}

int main(int argc, char **argv, char **envp)
{
	struct smolalsa_pcm pcm;
	struct smolalsa_state state = { 0 };
	static int16_t frames[PERIOD];
	unsigned int total = RATE * SECONDS, at = 0;
	int ret;

	(void)envp;

	check_ioctls();
	check_params();
	check_wav();
	check_sine();
	check_time();
	check_nodevice();

	if (failures) {
		printf("%u checks failed\n", failures);
		return 1;
	}

	printf("the structures, the WAV header, the sine and the clock all check out\n");

	ret = smolalsa_open(&pcm, argc > 1 ? argv[1] : NULL, RATE, 1, PERIOD, PERIODS);
	if (ret) {
		printf("no sound here (%d), which is not fatal\n", ret);
		return 0;
	}

	printf("beeping for %u seconds at %uHz\n", (unsigned int)SECONDS, (unsigned int)RATE);

	while (at < total) {
		unsigned int want = total - at < PERIOD ? total - at : PERIOD;

		fill(frames, want, at, total);

		ret = smolalsa_write(&pcm, frames, want);
		if (ret < 0) {
			printf("stopped: %d\n", ret);
			break;
		}

		/* Nothing went in, so the card is full: wait rather than spin */
		if (ret == 0) {
			struct timespec nap = { 0, 10 * 1000 * 1000 };

			nanosleep(&nap, NULL);
			continue;
		}

		at += (unsigned int)ret;
	}

	smolalsa_status(&pcm, &state);

	printf("beeped %u frames, ran dry %u times, and the card ended up %s\n", at,
	       smolalsa_xruns(&pcm), smolalsa_statename(state.state));

	smolalsa_close(&pcm);

	return 0;
}
