// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * smolalsa_mod: a little ProTracker MOD player.
 *
 *   smolalsa_mod [-d /dev/snd/pcmC0D0p] [-r rate] [-s seconds] [-l] file.mod ...
 *
 * Four (or six, or eight) Amiga channels mixed into stereo signed 16 bit with
 * integer arithmetic only, so it builds against nolibc. The usual effects
 * are there: arpeggio, portamento, tone portamento, vibrato, tremolo, sample
 * offset, volume slides, jumps, breaks, the E-commands that matter and the
 * speed/tempo one. -l plays the list forever, which is what a box left on
 * overnight wants.
 */

#include "smolalsa.h"

#ifndef NOLIBC
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#endif

#define MOD_MAXSIZE	(2 * 1024 * 1024)
#define MOD_MAXCHANNELS	8
#define MOD_SAMPLES	31
#define MOD_ROWS	64
#define PAL_CLOCK	3546895u	/* Paula's clock divided by two, Hz */
#define RATE		44100
#define PERIOD		1024
#define PERIODS		4

struct mod_sample {
	const int8_t *data;
	unsigned int length;	/* bytes */
	unsigned int loop_start;
	unsigned int loop_len;	/* 0 or 1: no loop */
	int finetune;		/* -8..7 */
	unsigned int volume;	/* 0..64 */
};

struct mod_channel {
	/* what is playing */
	const struct mod_sample *sample;
	uint32_t pos;		/* 16.16 into the sample */
	uint32_t step;		/* 16.16 per output frame */
	int playing;

	/* note state */
	int period;		/* the period the channel is at */
	int target_period;	/* tone portamento destination */
	int volume;
	int finetune;

	/* effect memory */
	unsigned int porta_speed;
	unsigned int vib_speed, vib_depth, vib_pos;
	unsigned int trem_speed, trem_depth, trem_pos;
	unsigned int offset;
	unsigned int loop_row, loop_count;
	unsigned int vol_slide;
	unsigned int retrig;

	/* the row being played */
	unsigned int note_period;
	unsigned int effect, param;
	int arp_base;
};

struct mod {
	unsigned int channels;
	unsigned int song_len;
	const uint8_t *order;
	const uint8_t *patterns;
	unsigned int num_patterns;
	struct mod_sample samples[MOD_SAMPLES + 1];

	struct mod_channel chan[MOD_MAXCHANNELS];
	unsigned int rate;
	unsigned int speed, bpm;
	unsigned int tick, row, order_pos;
	unsigned int samples_per_tick, samples_left;
	int break_row, jump_order, pattern_delay;
	unsigned int rows_played;
	int done;
};

static const int16_t sine_table[32] = {
	0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
	255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24,
};

/* the 36 periods of finetune 0, C-1 to B-3 */
static const uint16_t base_periods[36] = {
	856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
	428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
	214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,
};

/* 2^(-finetune/96) in 16.16, finetune -8..7 */
static const uint32_t finetune_mul[16] = {
	69432, 68955, 68475, 68000, 67525, 67056, 66591, 66125,
	65536, 65065, 64596, 64131, 63670, 63212, 62757, 62305,
};

static unsigned int be16(const uint8_t *p)
{
	return (p[0] << 8) | p[1];
}

static int mod_finetune_period(int period, int finetune)
{
	uint32_t p;

	if (!period)
		return 0;
	p = ((uint32_t)period * finetune_mul[finetune + 8] + 32768) >> 16;
	return (int)p;
}

static int mod_parse(struct mod *m, const uint8_t *data, unsigned int size)
{
	const uint8_t *tag;
	unsigned int i, max = 0, pat_bytes, off;

	memset(m, 0, sizeof(*m));
	if (size < 1084)
		return -1;
	tag = data + 1080;
	if (!memcmp(tag, "M.K.", 4) || !memcmp(tag, "M!K!", 4) || !memcmp(tag, "FLT4", 4) ||
	    !memcmp(tag, "4CHN", 4))
		m->channels = 4;
	else if (!memcmp(tag, "6CHN", 4))
		m->channels = 6;
	else if (!memcmp(tag, "8CHN", 4) || !memcmp(tag, "FLT8", 4) || !memcmp(tag, "OCTA", 4))
		m->channels = 8;
	else
		return -2;

	m->song_len = data[950];
	if (!m->song_len || m->song_len > 128)
		return -3;
	m->order = data + 952;
	for (i = 0; i < 128; i++)
		if (m->order[i] > max)
			max = m->order[i];
	m->num_patterns = max + 1;
	m->patterns = data + 1084;
	pat_bytes = m->num_patterns * MOD_ROWS * m->channels * 4;
	off = 1084 + pat_bytes;
	if (off > size)
		return -4;

	for (i = 1; i <= MOD_SAMPLES; i++) {
		const uint8_t *h = data + 20 + (i - 1) * 30;
		struct mod_sample *s = &m->samples[i];

		s->length = be16(h + 22) * 2;
		s->finetune = h[24] & 0xf;
		if (s->finetune > 7)
			s->finetune -= 16;
		s->volume = h[25] > 64 ? 64 : h[25];
		s->loop_start = be16(h + 26) * 2;
		s->loop_len = be16(h + 28) * 2;
		if (s->loop_len <= 2)
			s->loop_len = 0;
		if (s->loop_start + s->loop_len > s->length) {
			if (s->loop_start >= s->length)
				s->loop_len = 0;
			else
				s->loop_len = s->length - s->loop_start;
		}
		s->data = (const int8_t *)(data + off);
		if (off + s->length > size) {
			/* a truncated file: play what there is */
			s->length = off < size ? size - off : 0;
			s->loop_len = 0;
		}
		off += s->length;
	}

	m->speed = 6;
	m->bpm = 125;
	return 0;
}

static void mod_set_rate(struct mod *m, unsigned int rate)
{
	m->rate = rate;
	m->samples_per_tick = rate * 5 / (m->bpm * 2);
}

static void mod_chan_step(struct mod_channel *c, unsigned int rate, int period)
{
	if (period < 27)
		period = 27;
	c->step = (uint32_t)(((uint64_t)PAL_CLOCK << 16) / ((uint64_t)period * rate));
}

static void mod_chan_trigger(struct mod_channel *c, unsigned int offset)
{
	if (!c->sample || !c->sample->length) {
		c->playing = 0;
		return;
	}
	if (offset >= c->sample->length)
		offset = c->sample->loop_len ? c->sample->loop_start : c->sample->length - 1;
	c->pos = offset << 16;
	c->playing = 1;
	c->vib_pos = 0;
	c->trem_pos = 0;
}

static void mod_note_on(struct mod *m, struct mod_channel *c, const uint8_t *note)
{
	unsigned int sample_no = (note[0] & 0xf0) | (note[2] >> 4);
	unsigned int period = ((note[0] & 0xf) << 8) | note[1];
	unsigned int effect = note[2] & 0xf, param = note[3];
	unsigned int delay = (effect == 0xe && (param >> 4) == 0xd) ? param & 0xf : 0;

	c->effect = effect;
	c->param = param;
	c->note_period = period;

	if (sample_no && sample_no <= MOD_SAMPLES) {
		c->sample = &m->samples[sample_no];
		c->volume = c->sample->volume;
		c->finetune = c->sample->finetune;
	}
	if (effect == 0xe && (param >> 4) == 0x5)
		c->finetune = (param & 0xf) > 7 ? (int)(param & 0xf) - 16 : (int)(param & 0xf);

	if (period) {
		int ft = mod_finetune_period(period, c->finetune);

		if (effect == 3 || effect == 5) {
			/* slide towards the note rather than jumping to it */
			c->target_period = ft;
			if (effect == 3 && param)
				c->porta_speed = param;
		} else if (!delay) {
			c->period = ft;
			c->target_period = ft;
			mod_chan_step(c, m->rate, c->period);
			if (effect == 9) {
				if (param)
					c->offset = param << 8;
				mod_chan_trigger(c, c->offset);
			} else {
				mod_chan_trigger(c, 0);
			}
		}
	}

	switch (effect) {
	case 0x4:
		if (param & 0xf0)
			c->vib_speed = param >> 4;
		if (param & 0x0f)
			c->vib_depth = param & 0xf;
		break;
	case 0x7:
		if (param & 0xf0)
			c->trem_speed = param >> 4;
		if (param & 0x0f)
			c->trem_depth = param & 0xf;
		break;
	case 0x5:
	case 0x6:
	case 0xa:
		if (param)
			c->vol_slide = param;
		break;
	case 0xb:
		m->jump_order = param;
		break;
	case 0xc:
		c->volume = param > 64 ? 64 : param;
		break;
	case 0xd:
		m->break_row = (param >> 4) * 10 + (param & 0xf);
		if (m->break_row > 63)
			m->break_row = 0;
		break;
	case 0xe:
		switch (param >> 4) {
		case 0x1:
			c->period -= param & 0xf;
			if (c->period < 113)
				c->period = 113;
			mod_chan_step(c, m->rate, c->period);
			break;
		case 0x2:
			c->period += param & 0xf;
			if (c->period > 856)
				c->period = 856;
			mod_chan_step(c, m->rate, c->period);
			break;
		case 0x6:
			if (!(param & 0xf)) {
				c->loop_row = m->row;
			} else if (!c->loop_count) {
				c->loop_count = param & 0xf;
				m->break_row = c->loop_row;
				m->jump_order = -2;	/* same order, no advance */
			} else if (--c->loop_count) {
				m->break_row = c->loop_row;
				m->jump_order = -2;
			}
			break;
		case 0x9:
			c->retrig = param & 0xf;
			break;
		case 0xa:
			c->volume += param & 0xf;
			if (c->volume > 64)
				c->volume = 64;
			break;
		case 0xb:
			c->volume -= param & 0xf;
			if (c->volume < 0)
				c->volume = 0;
			break;
		case 0xc:
			if (!(param & 0xf))
				c->volume = 0;
			break;
		case 0xe:
			m->pattern_delay = param & 0xf;
			break;
		}
		break;
	case 0xf:
		if (param == 0)
			break;
		if (param < 32)
			m->speed = param;
		else {
			m->bpm = param;
			mod_set_rate(m, m->rate);
		}
		break;
	}
	c->arp_base = c->period;
}

static void mod_volume_slide(struct mod_channel *c)
{
	if (c->vol_slide & 0xf0)
		c->volume += c->vol_slide >> 4;
	else
		c->volume -= c->vol_slide & 0xf;
	if (c->volume > 64)
		c->volume = 64;
	if (c->volume < 0)
		c->volume = 0;
}

static void mod_tone_porta(struct mod *m, struct mod_channel *c)
{
	if (!c->target_period || !c->period)
		return;
	if (c->period < c->target_period) {
		c->period += c->porta_speed;
		if (c->period > c->target_period)
			c->period = c->target_period;
	} else if (c->period > c->target_period) {
		c->period -= c->porta_speed;
		if (c->period < c->target_period)
			c->period = c->target_period;
	}
	mod_chan_step(c, m->rate, c->period);
}

static int mod_vibrato_delta(unsigned int pos, unsigned int depth)
{
	int v = sine_table[pos & 31];

	if (pos & 32)
		v = -v;
	return (v * (int)depth) >> 7;
}

/* effects that run on the ticks after the first one of a row */
static void mod_note_tick(struct mod *m, struct mod_channel *c)
{
	unsigned int param = c->param;

	switch (c->effect) {
	case 0x0:
		if (param) {
			unsigned int which = m->tick % 3;
			int p = c->arp_base;

			if (which) {
				unsigned int semis = which == 1 ? param >> 4 : param & 0xf;
				int i;

				/* find the base note and step up from it */
				for (i = 0; i < 36; i++)
					if (mod_finetune_period(base_periods[i], c->finetune) <= p)
						break;
				i += semis;
				if (i > 35)
					i = 35;
				p = mod_finetune_period(base_periods[i], c->finetune);
			}
			mod_chan_step(c, m->rate, p);
		}
		break;
	case 0x1:
		c->period -= param;
		if (c->period < 113)
			c->period = 113;
		mod_chan_step(c, m->rate, c->period);
		break;
	case 0x2:
		c->period += param;
		if (c->period > 856)
			c->period = 856;
		mod_chan_step(c, m->rate, c->period);
		break;
	case 0x3:
		mod_tone_porta(m, c);
		break;
	case 0x4:
		mod_chan_step(c, m->rate, c->period + mod_vibrato_delta(c->vib_pos, c->vib_depth));
		c->vib_pos += c->vib_speed;
		break;
	case 0x5:
		mod_tone_porta(m, c);
		mod_volume_slide(c);
		break;
	case 0x6:
		mod_chan_step(c, m->rate, c->period + mod_vibrato_delta(c->vib_pos, c->vib_depth));
		c->vib_pos += c->vib_speed;
		mod_volume_slide(c);
		break;
	case 0x7:
		c->trem_pos += c->trem_speed;
		break;
	case 0xa:
		mod_volume_slide(c);
		break;
	case 0xe:
		switch (param >> 4) {
		case 0x9:
			if (c->retrig && (m->tick % c->retrig) == 0)
				mod_chan_trigger(c, 0);
			break;
		case 0xc:
			if (m->tick == (param & 0xf))
				c->volume = 0;
			break;
		case 0xd:
			if (m->tick == (param & 0xf) && c->note_period) {
				c->period = mod_finetune_period(c->note_period, c->finetune);
				c->target_period = c->period;
				mod_chan_step(c, m->rate, c->period);
				mod_chan_trigger(c, 0);
			}
			break;
		}
		break;
	}
}

static void mod_advance_row(struct mod *m)
{
	unsigned int i;
	const uint8_t *row;

	if (m->pattern_delay) {
		m->pattern_delay--;
		return;
	}

	if (m->jump_order >= 0 || m->break_row >= 0) {
		if (m->jump_order >= 0)
			m->order_pos = m->jump_order;
		else if (m->jump_order != -2)
			m->order_pos++;
		m->row = m->break_row >= 0 ? m->break_row : 0;
		m->jump_order = -1;
		m->break_row = -1;
	}

	if (m->order_pos >= m->song_len) {
		m->order_pos = 0;
		m->row = 0;
		m->done = 1;
	}

	row = m->patterns + ((m->order[m->order_pos] * MOD_ROWS + m->row) * m->channels * 4);
	for (i = 0; i < m->channels; i++)
		mod_note_on(m, &m->chan[i], row + i * 4);
	m->rows_played++;

	if (m->jump_order < 0 && m->break_row < 0) {
		if (++m->row >= MOD_ROWS) {
			m->row = 0;
			m->order_pos++;
		}
	}
}

static void mod_tick(struct mod *m)
{
	unsigned int i;

	if (m->tick == 0) {
		mod_advance_row(m);
	} else {
		for (i = 0; i < m->channels; i++)
			mod_note_tick(m, &m->chan[i]);
	}
	if (++m->tick >= m->speed)
		m->tick = 0;
}

static void mod_start(struct mod *m, unsigned int rate)
{
	mod_set_rate(m, rate);
	memset(m->chan, 0, sizeof(m->chan));
	m->tick = 0;
	m->row = 0;
	m->order_pos = 0;
	m->samples_left = 0;
	m->break_row = -1;
	m->jump_order = -1;
	m->done = 0;
	m->rows_played = 0;
}

/* Mix @frames stereo frames into @out */
static void mod_mix(struct mod *m, int16_t *out, unsigned int frames)
{
	unsigned int f, i;
	unsigned int shift = m->channels > 4 ? 1 : 0;

	for (f = 0; f < frames; f++) {
		int left = 0, right = 0;

		if (!m->samples_left) {
			mod_tick(m);
			m->samples_left = m->samples_per_tick;
		}
		m->samples_left--;

		for (i = 0; i < m->channels; i++) {
			struct mod_channel *c = &m->chan[i];
			const struct mod_sample *s = c->sample;
			int v, vol;
			unsigned int p;

			if (!c->playing || !s)
				continue;
			/* run off the end: back round the loop, or stop */
			p = c->pos >> 16;
			if (p >= s->length) {
				if (!s->loop_len) {
					c->playing = 0;
					continue;
				}
				p = s->loop_start + ((p - s->loop_start) % s->loop_len);
				c->pos = (p << 16) | (c->pos & 0xffff);
			}
			vol = c->volume;
			if (c->effect == 0x7 && c->trem_depth) {
				vol += mod_vibrato_delta(c->trem_pos, c->trem_depth) * 2;
				if (vol < 0)
					vol = 0;
				if (vol > 64)
					vol = 64;
			}
			v = s->data[p] * vol;	/* -8192..8128 */
			/* Amiga panning: channels 0 and 3 left, 1 and 2 right, softened */
			if ((i & 3) == 0 || (i & 3) == 3) {
				left += v * 3;
				right += v;
			} else {
				left += v;
				right += v * 3;
			}
			c->pos += c->step;
		}
		/* four channels at full volume: 4 * 8192 * 3 = 98304, so scale */
		left = (left >> (2 + shift));
		right = (right >> (2 + shift));
		if (left > 32767)
			left = 32767;
		if (left < -32768)
			left = -32768;
		if (right > 32767)
			right = 32767;
		if (right < -32768)
			right = -32768;
		out[f * 2] = (int16_t)left;
		out[f * 2 + 1] = (int16_t)right;
	}
}

static uint8_t filebuf[MOD_MAXSIZE];
static int16_t mixbuf[PERIOD * 2];

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s [-d dev] [-r rate] [-s seconds] [-l] file.mod ...\n",
		prog);
}

static int play_file(const char *path, const char *dev, unsigned int rate, int loop,
		     unsigned int seconds)
{
	static struct mod m;
	struct smolalsa_pcm pcm;
	unsigned int size = 0, played = 0;
	unsigned long t0;
	int fd, ret, n;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		printf("smolalsa_mod: cannot open %s\n", path);
		return 1;
	}
	while (size < MOD_MAXSIZE) {
		n = read(fd, filebuf + size, MOD_MAXSIZE - size);
		if (n <= 0)
			break;
		size += n;
	}
	close(fd);

	ret = mod_parse(&m, filebuf, size);
	if (ret) {
		printf("smolalsa_mod: %s is not a MOD I understand (%d)\n", path, ret);
		return 1;
	}

	ret = smolalsa_open(&pcm, dev, rate, 2, PERIOD, PERIODS);
	if (ret) {
		printf("smolalsa_mod: no sound device (%d)\n", ret);
		return 1;
	}
	if (pcm.rate != rate)
		printf("smolalsa_mod: card gave %u Hz\n", pcm.rate);

	printf("smolalsa_mod: %s: \"%.20s\", %u channels, %u orders, %u patterns, at %u Hz\n",
	       path, (const char *)filebuf, m.channels, m.song_len, m.num_patterns, pcm.rate);

	mod_start(&m, pcm.rate);
	t0 = smolalsa_now_ms();
	while (!m.done || loop) {
		unsigned int have = PERIOD, off = 0;

		if (m.done) {
			printf("smolalsa_mod: looping\n");
			m.done = 0;
		}
		if (seconds && smolalsa_now_ms() - t0 >= seconds * 1000UL)
			break;
		mod_mix(&m, mixbuf, have);
		while (off < have) {
			ret = smolalsa_write(&pcm, mixbuf + off * 2, have - off);
			if (ret < 0) {
				printf("smolalsa_mod: write failed (%d)\n", ret);
				smolalsa_close(&pcm);
				return 1;
			}
			if (ret == 0) {
				struct timespec nap = { 0, 5 * 1000 * 1000 };

				nanosleep(&nap, NULL);
				continue;
			}
			off += ret;
		}
		played += have;
	}

	printf("smolalsa_mod: %u frames in %lu ms (expected %lu ms), %u rows\n",
	       played, smolalsa_now_ms() - t0, smolalsa_frames_ms(pcm.rate, played),
	       m.rows_played);
	smolalsa_close(&pcm);
	return 0;
}

int main(int argc, char **argv, char **envp)
{
	const char *dev = NULL;
	unsigned int rate = RATE, seconds = 0;
	int loop = 0, opt, i, ret;

	(void)envp;

	while ((opt = getopt(argc, argv, "d:r:s:lh")) != -1) {
		switch (opt) {
		case 'd':
			dev = optarg;
			break;
		case 'r':
			rate = (unsigned int)atoi(optarg);
			break;
		case 's':
			seconds = (unsigned int)atoi(optarg);
			break;
		case 'l':
			loop = 1;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (optind >= argc || !rate) {
		usage(argv[0]);
		return 1;
	}

	do {
		for (i = optind; i < argc; i++) {
			/* with several files -l cycles through them rather than repeating one */
			ret = play_file(argv[i], dev, rate, loop && argc - optind == 1, seconds);
			if (ret)
				return ret;
		}
	} while (loop);

	return 0;
}
