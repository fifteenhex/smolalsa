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

#endif /* _SMOLALSA_H */
