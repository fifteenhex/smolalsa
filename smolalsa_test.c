// SPDX-License-Identifier: GPL-3.0-or-later

#include "smolalsa.h"

#ifndef NOLIBC
#include <stdio.h>
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

/* Nothing there is its own answer, and it leaves nothing open */
static void check_nodevice(void)
{
	struct smolalsa_pcm pcm;

	CHECK(smolalsa_open(&pcm, "/dev/snd/smolalsa-no-such-card", RATE, 1, PERIOD,
			    PERIODS) == SMOLALSA_NODEVICE);
	CHECK(pcm.fd == -1);
	/* and writing to what never opened is nothing written, not a crash */
	CHECK(smolalsa_write(&pcm, "xx", 1) == 0);
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
	static int16_t frames[PERIOD];
	unsigned int total = RATE * SECONDS, at = 0;
	int ret;

	(void)envp;

	check_ioctls();
	check_params();
	check_nodevice();

	if (failures) {
		printf("%u checks failed\n", failures);
		return 1;
	}

	printf("the structures and the parameter helpers check out\n");

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

	printf("beeped %u frames, ran dry %u times\n", at, smolalsa_xruns(&pcm));

	smolalsa_close(&pcm);

	return 0;
}
