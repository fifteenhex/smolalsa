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

#endif /* _SMOLALSA_H */
