// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Record from a card into a 16 bit WAV, or into raw samples.
 *
 *	smolalsa_rec [-d /dev/snd/pcmC0D0c] [-r rate] [-c channels]
 *		[-p period-frames] [-n periods] [-s seconds] [-R] out.wav
 *
 * Every second it says the loudest sample of that second and a checksum of
 * everything so far, which tells a quiet room from a dead input without anyone
 * having to listen to the file. It ends with the same timing line as
 * smolalsa_play.
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
#define SECONDS		5
#define CHUNKBYTES	32768

static int16_t chunk[CHUNKBYTES / 2];
static unsigned char hdr[SMOLALSA_WAV_HDRSZ];

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-d dev] [-r rate] [-c channels] [-p period-frames]\n"
		"       [-n periods] [-s seconds] [-R] out.wav\n", prog);
}

/* Keep writing until it has all gone, or it really fails */
static int writeall(int fd, const void *buf, unsigned long len)
{
	const unsigned char *at = buf;
	unsigned long done = 0;

	while (done < len) {
		long n = (long)write(fd, at + done, len - done);

		if (n > 0)
			done += (unsigned long)n;
		else if (n < 0 && errno == EINTR)
			continue;
		else
			return -1;
	}

	return 0;
}

int main(int argc, char **argv, char **envp)
{
	const char *dev = NULL, *path;
	struct smolalsa_pcm pcm;
	unsigned int rate = RATE, channels = CHANNELS, period = PERIOD;
	unsigned int periods = PERIODS, seconds = SECONDS;
	unsigned int frames = 0, total, chunkframes;
	unsigned int sum = 0, peak = 0, secondpeak = 0;
	unsigned long started, next, now, elapsed, expected, bytes = 0;
	int raw = 0, fd, opt, ret;

	(void)envp;

	while ((opt = getopt(argc, argv, "d:r:c:p:n:s:Rh")) != -1) {
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
		case 's':
			seconds = (unsigned int)atoi(optarg);
			break;
		case 'R':
			raw = 1;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (argc - optind != 1 || !seconds || !rate || !channels || !period || !periods) {
		usage(argv[0]);
		return 1;
	}
	path = argv[optind];

	ret = smolalsa_open_capture(&pcm, dev, rate, channels, period, periods);
	if (ret) {
		printf("no card to record from (%d)\n", ret);
		return 1;
	}

	printf("%s: %u Hz, %u channels, signed 16 bit, %u frames a period, %u of them\n",
	       dev ? dev : SMOLALSA_CAPTUREPATH, pcm.rate, pcm.channels, pcm.periodframes,
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

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		printf("cannot open %s\n", path);
		smolalsa_close(&pcm);
		return 1;
	}

	/* The size is not known until it stops, so the header goes down twice */
	if (!raw) {
		smolalsa_wav_header(hdr, pcm.rate, pcm.channels, 16, 0);
		if (writeall(fd, hdr, SMOLALSA_WAV_HDRSZ)) {
			printf("cannot write %s\n", path);
			smolalsa_close(&pcm);
			close(fd);
			return 1;
		}
	}

	total = seconds * pcm.rate;
	started = smolalsa_now_ms();
	next = started + 1000;

	while (frames < total) {
		unsigned int want = chunkframes, i, n;

		if (total - frames < want)
			want = total - frames;

		ret = smolalsa_read_all(&pcm, chunk, want);
		if (ret < 0) {
			printf("stopped: %d\n", ret);
			break;
		}

		n = (unsigned int)ret * pcm.channels;
		for (i = 0; i < n; i++) {
			int sample = chunk[i];
			unsigned int size = (unsigned int)(sample < 0 ? -sample : sample);

			if (size > secondpeak)
				secondpeak = size;
			if (size > peak)
				peak = size;

			/*
			 * Rotate and exclusive-or rather than add, so a
			 * channel that is stuck or swapped shows up.
			 */
			sum = (sum << 1 | sum >> 31) ^ (unsigned int)(unsigned short)sample;
		}

		if (writeall(fd, chunk, (unsigned long)ret * pcm.framebytes)) {
			printf("cannot write %s\n", path);
			break;
		}

		frames += (unsigned int)ret;
		bytes += (unsigned long)ret * pcm.framebytes;

		now = smolalsa_now_ms();
		if (now >= next) {
			printf("%lu s: frames %u, peak %u, sum %08x, xruns %u\n",
			       (now - started) / 1000, frames, secondpeak, sum,
			       smolalsa_xruns(&pcm));
			secondpeak = 0;
			next += 1000;
		}
	}

	elapsed = smolalsa_now_ms() - started;
	smolalsa_drop(&pcm);
	smolalsa_close(&pcm);

	if (!raw) {
		smolalsa_wav_header(hdr, pcm.rate, pcm.channels, 16, (unsigned int)bytes);
		if (lseek(fd, 0, SEEK_SET) != 0 || writeall(fd, hdr, SMOLALSA_WAV_HDRSZ))
			printf("cannot write the sizes back into %s\n", path);
	}
	close(fd);

	expected = smolalsa_frames_ms(pcm.rate, frames);
	printf("wrote %s, %lu bytes, peak %u, sum %08x\n", path, bytes, peak, sum);
	printf("frames %u, elapsed %lu ms, expected %lu ms, xruns %u\n", frames, elapsed,
	       expected, smolalsa_xruns(&pcm));

	return 0;
}
