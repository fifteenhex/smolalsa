# smolalsa
Header only library for ALSA

Include it and go: everything is a static inline function, there is nothing to
link, and it builds against nolibc.

```c
struct smolalsa_pcm pcm;
int16_t frames[512];

/* NULL for the first card, or a path for a particular one */
if (smolalsa_open(&pcm, NULL, 22050, 1, 512, 4)) {
	/* no sound on this machine, which is not a reason to stop */
	return;
}

for (;;) {
	int wrote = smolalsa_write(&pcm, frames, 512);

	if (wrote < 0)
		break;		/* something went wrong */
	if (wrote == 0)
		;		/* full, come back later */
}

smolalsa_close(&pcm);
```

It talks to the kernel's PCM device rather than to libasound, so there is
nothing to install and nothing to link. Playing and recording, interleaved,
signed 16 bit, at whatever rate and however many channels the card will agree
to.

`smolalsa_open()` gives back 0, `SMOLALSA_NODEVICE` when there is no card,
`SMOLALSA_BUSY` when something else has it, or `-errno` when the card refused
the parameters. Neither of the first two is a reason for a program to stop,
which is why they are told apart.

Nothing blocks. `smolalsa_write()` hands over as many frames as will fit and
says how many that was, which may be none, so a program with a frame to draw
can get on with drawing it. Running dry is not passed on as an error either --
it means the last frame took too long -- the card is set going again and the
next write carries on.

Recording is the same shape the other way round. `smolalsa_open_capture()`
opens the other node of the same card and `smolalsa_read()` takes as many
frames as are ready, which may be none: the first read is what starts the card,
so nothing ready is the usual answer to it. Overrunning is counted and shrugged
off like running dry, since the frames are gone either way.

For a program that has nothing else to do while the card catches up,
`smolalsa_write_all()` and `smolalsa_read_all()` go round that loop themselves,
waiting on the device in between, and give up on a card that has had nothing to
say for a second rather than hanging on it for ever. `smolalsa_wait()` is that
wait on its own. A program with a frame to draw should not be using any of
them.

`smolalsa_status()` says what state the card is in and where its two pointers
have got to: `hwptr` is frames the card has been through and `applptr` is
frames handed over, both counting up to `pcm.boundary` and wrapping there, so
the two of them together say whether it is keeping up without timing anything.
`smolalsa_avail()` and `smolalsa_delay()` are the short answers to "how much
room" and "how much still to come out", and `smolalsa_xruns()` is how many
times it has run dry or over since it was opened.

The mixer is the same idea on the control node. `smolalsa_ctl_open()`, then
`smolalsa_ctl_list()` for everything the card has, or `smolalsa_ctl_find()` to
go straight to one by name -- the kernel does that lookup itself as long as the
numeric id is left at zero, so it is one call rather than a walk of the whole
list. `smolalsa_ctl_get()` and `smolalsa_ctl_set()` read and write switches,
numbers and lists as numbers, and `smolalsa_ctl_set_item()` sets a list by the
name of the item, since "Mic" is what a person has and not which number it is.

The period is how much the card takes at a time and the number of them is how
far ahead it will hold. 512 frames at 22050 with four of them is about 90ms of
slack: enough that a slow frame does not leave a hole, little enough that a
sound starts about when it was asked for. The period is asked for as a minimum
and read back afterwards, since a card that will only do 1024 frames at a time
is no reason to fail the open -- so everything in `struct smolalsa_pcm` is what
the card agreed to and not what was asked for.

There is no udev on the machine this is aimed at, so the paths are the names
the kernel makes for itself: `/dev/snd/pcmC0D0p` for playing (116,16),
`/dev/snd/pcmC0D0c` for recording (116,24) and `/dev/snd/controlC0` for the
mixer (116,0).

## The programs

Thin things: they read the arguments, call the header and print.

`smolalsa_test [device]` checks the parts that need no card -- the parameter
structures against the ioctl numbers, the WAV header, the sine table and the
clock -- and then beeps for two seconds if there is a card to beep on. It exits
0 whether or not there is one, and non-zero only if a check was wrong, so it is
the thing to run in CI.

`smolalsa_play [-d dev] [-r rate] [-c channels] [-p period-frames] [-n periods]
(-t freq-hz [-s seconds] [-a amplitude-0-100] | file.wav | -R file.raw)` plays
a tone, a 16 bit WAV or raw signed 16 bit samples. A WAV brings its own rate and
channel count unless `-r` or `-c` say otherwise. It prints what the card agreed
to, where it has got to every second, and finishes with `frames`, `elapsed ms`,
`expected ms` and `xruns`: elapsed against expected is how the card's pacing is
checked, since the two should be within a period or so of each other.

`smolalsa_rec [-d dev] [-r rate] [-c channels] [-p period-frames] [-n periods]
[-s seconds] [-R] out.wav` records to a WAV, or to raw samples with `-R`. Every
second it says the loudest sample of that second and a checksum of everything
so far, which is what tells a quiet room from a dead input, and it ends with
the same timing line.

`smolalsa_ctl [-d dev] [list | get NAME | set NAME VALUE]` lists what the card
lets you change, with its type, how many values it has, where it is now and
what else it could be; and sets one by name from a number, `on`/`off`, or the
name of a list item.

`smolalsa_mod [-d dev] [-r rate] [-s seconds] [-l] file.mod ...` plays
ProTracker modules: four, six or eight Amiga channels mixed into stereo 16 bit
with nothing but integer arithmetic, since nolibc has no floating point to
offer. The usual effects are there -- arpeggio, portamento, vibrato, tremolo,
sample offset, volume slides, jumps, breaks, the E commands that matter and
speed and tempo. `-l` plays the list round and round, which is what a box left
on overnight wants, and `-s` stops after so many seconds. It says how many
frames went out in how many milliseconds at the end, so a card that plays fast
or slow shows up as a number rather than a feeling.

## Building it

Both builds are `smolcommon.mk`'s. `make` on its own builds all five against
the host's libc, into `<prog>.libc.elf`. That is the half of the header behind
`#ifndef NOLIBC`, and it is what the self-test runs on a desktop.

Adding `SMOL_ARCH` builds the static nolibc ones for a target as well. That
half wants a cross prefix, nolibc and nolibc-extensions, and `NOPIE=1` too if
your nolibc cannot relocate itself yet:

```
make SMOL_ARCH=x86_64 CROSS_COMPILE=x86_64-linux-gnu- NOPIE=1 \
     NOLIBCDIR=/path/to/linux/tools/include/nolibc \
     NOLIBCEXTDIR=/path/to/nolibc-extensions
```

`SMOL_ARCH` is one of `x86_64`, `cortexa7`, `cortexa9`, `68000`, `68030`,
`68040` or `68060`, and it names what comes out: `<prog>.<arch>.elf`, stripped,
with the symbols left in `<prog>.<arch>.elf.dbg` beside it, so one tree can
hold more than one target at a time. Add `UAPIDIR="/path/to/uapi/include"` when
the target's kernel headers are not this machine's. `V=1` prints the command
lines.

sound/asound.h is the kernel's own header and it reaches for the libc's time.h
on the way past, which nolibc disagrees with, so those includes are held off.
Only the status structures have a time in them, and with nolibc's struct
timespec those come out as the kernel's own 64 bit layout, which it accepts.
`smolalsa_test` checks each structure against the size its ioctl number
carries, since that is what would break first if any of this were wrong.
