// SPDX-License-Identifier: GPL-3.0-or-later

#include "smolalsa.h"

#ifndef NOLIBC
#include <stdio.h>
#endif

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

int main(int argc, char **argv, char **envp)
{
	(void)argc;
	(void)argv;
	(void)envp;

	check_ioctls();

	if (failures) {
		printf("%u checks failed\n", failures);
		return 1;
	}

	printf("the structures check out\n");

	return 0;
}
