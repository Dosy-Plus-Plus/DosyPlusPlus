/* common.h - shared types and small helpers. No DOS dependencies. */
#ifndef DOSYPP_COMMON_H
/* The largest file this client will touch, and why.
 *
 * Every seek in the program goes through fseek/ftell, whose offsets are
 * `long` -- 32 bits on DJGPP. A file at or beyond 2GB therefore cannot be
 * addressed: (long)offset truncates, and the failure mode is not an error
 * but a seek to the wrong place, which serves or writes the wrong bytes
 * and verifies against the wrong hash. Silent corruption is the one
 * outcome worth ruling out, so the limit is explicit, enforced where files
 * enter the program, and reported.
 *
 * 2GB-1 rather than 2GB: the last addressable offset is LONG_MAX, and a
 * file of exactly 2GB has a byte at it.
 *
 * Lifting this means 64-bit file offsets throughout, which DJGPP does not
 * have. It is a limit of the target, not of the design. */
#define DOSYPP_MAX_FILE 2147483647UL

/* One place for the version: it used to be typed into the banner, the info
   door and nothing else, which is how a client ends up reporting two
   different versions of itself. */
#define DOSYPP_VERSION "0.1"

#define DOSYPP_COMMON_H

#include <stddef.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Longest ADC message we will accept, including the terminating newline.
   Real INF and file-list messages stay well under this; anything longer is
   treated as a protocol violation rather than grown into. */
#define ADC_LINE_MAX 4096

/* DOSy++ is built without floating point (see IMPLEMENTATION_PLAN.md S6):
   some 486 cores are SX-class. Percentages and rates use these helpers. */
#define PCT(part, whole) ((whole) ? (int)(((part) * 100) / (whole)) : 0)

#endif /* DOSYPP_COMMON_H */
