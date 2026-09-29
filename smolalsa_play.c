// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Play a tone, a 16 bit WAV or raw samples on a card.
 *
 *	smolalsa_play [-d /dev/snd/pcmC0D0p] [-r rate] [-c channels]
 *		[-p period-frames] [-n periods] (-t freq-hz [-s seconds]
 *		[-a amplitude-0-100] | file.wav | -R file.raw)
 *
 * What the card agreed to is printed on the way in, where it has got to every
 * second, and how long it took at the end. Elapsed against expected is the
 * thing to look at: a card pacing us properly comes out within a period or two
 * of the length of the samples.
 */
#include "smolalsa.h"

#ifndef NOLIBC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#endif

#define RATE		48000
#define CHANNELS	2
#define PERIOD		1024
#define PERIODS		4
#define HDRBUF		4096		/* room for anyone's chunks before data */
#define CHUNKBYTES	32768

static int16_t chunk[CHUNKBYTES / 2];
static unsigned char hdrbuf[HDRBUF];

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-d dev] [-r rate] [-c channels] [-p period-frames]\n"
		"       [-n periods] (-t freq-hz [-s seconds] [-a amp-0-100] |\n"
		"       file.wav | -R file.raw)\n", prog);
}

/* Keep reading until the buffer is full, the file ends, or it really fails */
static long readall(int fd, void *buf, unsigned long len)
{
	unsigned char *at = buf;
	unsigned long done = 0;

	while (done < len) {
		long n = (long)read(fd, at + done, len - done);

		if (n > 0)
			done += (unsigned long)n;
		else if (n == 0)
			break;
		else if (errno == EINTR)
			continue;
		else
			return -1;
	}

	return (long)done;
}

int main(int argc, char **argv, char **envp)
{
	const char *dev = NULL, *path = NULL;
	struct smolalsa_pcm pcm;
	struct smolalsa_tone tone;
	struct smolalsa_wav wav;
	unsigned int rate = 0, channels = 0, period = PERIOD, periods = PERIODS;
	unsigned int freq = 0, amp = 80, seconds = 0;
	unsigned int frames = 0, total = 0, chunkframes, left = 0;
	unsigned long started, next, now, elapsed, expected, deadline;
	int raw = 0, fd = -1, opt, ret;

	(void)envp;

	while ((opt = getopt(argc, argv, "d:r:c:p:n:t:s:a:R:h")) != -1) {
		switch (opt) {
		case 'd':
			dev = optarg;
			break;
		case 'r':
			rate = (unsigned int)atoi(optarg);
			break;
		case 'c':
			channels = (unsigned int)atoi(optarg);
			break;
		case 'p':
			period = (unsigned int)atoi(optarg);
			break;
		case 'n':
			periods = (unsigned int)atoi(optarg);
			break;
		case 't':
			freq = (unsigned int)atoi(optarg);
			break;
		case 's':
			seconds = (unsigned int)atoi(optarg);
			break;
		case 'a':
			amp = (unsigned int)atoi(optarg);
			break;
		case 'R':
			path = optarg;
			raw = 1;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (optind < argc) {
		if (path || freq || argc - optind != 1) {
			usage(argv[0]);
			return 1;
		}
		path = argv[optind];
	}

	/* a tone or a file, and not both */
	if ((!path && !freq) || (path && freq) || !period || !periods) {
		usage(argv[0]);
		return 1;
	}

	if (path) {
		fd = open(path, O_RDONLY);
		if (fd < 0) {
			printf("cannot open %s\n", path);
			return 1;
		}
	}

	if (path && !raw) {
		long n = readall(fd, hdrbuf, sizeof(hdrbuf));

		if (n < 12 || smolalsa_wav_parse(hdrbuf, (unsigned int)n, &wav) < 0) {
			printf("%s is not a 16 bit PCM WAV\n", path);
			return 1;
		}

		/* the file knows its own rate and layout, unless told otherwise */
		if (!rate)
			rate = wav.rate;
		if (!channels)
			channels = wav.channels;

		left = wav.databytes;
		if (lseek(fd, (off_t)wav.dataoffset, SEEK_SET) < 0) {
			printf("cannot seek %s\n", path);
			return 1;
		}

		printf("%s: WAV, %u Hz, %u channels, %u bit, %u bytes of samples\n", path,
		       wav.rate, wav.channels, wav.bits, wav.databytes);
	}

	if (!rate)
		rate = RATE;
	if (!channels)
		channels = CHANNELS;

	ret = smolalsa_open(&pcm, dev, rate, channels, period, periods);
	if (ret) {
		printf("no card to play on (%d)\n", ret);
		return 1;
	}

	printf("%s: %u Hz, %u channels, signed 16 bit, %u frames a period, %u of them\n",
	       dev ? dev : SMOLALSA_DEVICEPATH, pcm.rate, pcm.channels, pcm.periodframes,
	       pcm.periods);

	chunkframes = (unsigned int)(sizeof(chunk) / pcm.framebytes);
	if (chunkframes > pcm.periodframes)
		chunkframes = pcm.periodframes;
	if (!chunkframes) {
		printf("a period of %u frames does not fit in the chunk buffer\n",
		       pcm.periodframes);
		smolalsa_close(&pcm);
		return 1;
	}

	if (freq) {
		smolalsa_tone_init(&tone, pcm.rate, freq, amp);
		if (!seconds)
			seconds = 3;
		printf("a %u Hz tone at %u%% for %u seconds\n", freq, amp, seconds);
	}
	if (seconds)
		total = seconds * pcm.rate;

	started = smolalsa_now_ms();
	next = started + 1000;

	while (!total || frames < total) {
		unsigned int want = chunkframes;

		if (total && total - frames < want)
			want = total - frames;

		if (freq) {
			smolalsa_tone_fill(&tone, chunk, want, pcm.channels);
		} else {
			unsigned long bytes = (unsigned long)want * pcm.framebytes;
			long n;

			if (left && bytes > left)
				bytes = left;

			n = readall(fd, chunk, bytes);
			if (n < 0) {
				printf("cannot read %s\n", path);
				break;
			}
			if (left)
				left -= (unsigned int)n;

			/* a frame that is only half there is no use to the card */
			want = (unsigned int)n / pcm.framebytes;
			if (!want)
				break;
		}

		ret = smolalsa_write_all(&pcm, chunk, want);
		if (ret < 0) {
			printf("stopped: %d\n", ret);
			break;
		}
		frames += (unsigned int)ret;

		now = smolalsa_now_ms();
		if (now >= next) {
			struct smolalsa_state state;

			if (smolalsa_status(&pcm, &state) < 0)
				memset(&state, 0, sizeof(state));

			printf("%lu s: frames %u, hw_ptr %lu, xruns %u\n",
			       (now - started) / 1000, frames, state.hwptr,
			       smolalsa_xruns(&pcm));
			next += 1000;
		}
	}

	/*
	 * A non-blocking drain comes back at once, so wait for the card to
	 * actually stop -- but only for as long as the buffer could hold.
	 */
	smolalsa_drain(&pcm);
	deadline = smolalsa_now_ms() +
		   smolalsa_frames_ms(pcm.rate, pcm.periodframes * pcm.periods) + 100;
	while (smolalsa_delay(&pcm) > 0 && smolalsa_now_ms() < deadline) {
		struct timespec nap = { 0, 5 * 1000 * 1000 };

		nanosleep(&nap, NULL);
	}

	elapsed = smolalsa_now_ms() - started;
	expected = smolalsa_frames_ms(pcm.rate, frames);
	printf("frames %u, elapsed %lu ms, expected %lu ms, xruns %u\n", frames, elapsed,
	       expected, smolalsa_xruns(&pcm));

	smolalsa_close(&pcm);
	if (fd >= 0)
		close(fd);

	return 0;
}
