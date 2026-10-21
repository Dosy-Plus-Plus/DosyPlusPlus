/* sndout.h - somewhere for the mixer's bytes to go.
 *
 * Two backends, chosen at compile time:
 *
 *   DOS: a Sound Blaster, programmed for auto-init DMA over a ring buffer
 *        in conventional memory. No interrupt handler -- the DMA
 *        controller's own counter says where the card is reading, and the
 *        main loop refills what has been played. An ISR would be the
 *        textbook answer; on a 486 running a single-threaded client it
 *        would also be the thing that breaks the keyboard handler at three
 *        in the morning. Polling a counter cannot.
 *
 *   Host: a WAV file. Not a joke: it is what makes the mixer testable
 *        without a sound card, and "render the tune to a file" is a useful
 *        thing for a DOS program's build to be able to do anyway.
 *
 * Signed 8-bit mono throughout, because that is what a Sound Blaster 1.0
 * takes and what a MOD's samples already are. Anything better would mean
 * resampling for the sake of a format the target machine cannot play.
 */
#ifndef DOSYPP_SNDOUT_H
#define DOSYPP_SNDOUT_H

#include <stddef.h>

/* How much audio the ring holds. 16KB at 11kHz is about a second and a
   half, which is long enough that a slow main loop does not stutter and
   short enough that stopping the music stops it audibly. Must stay a power
   of two and must not cross a 64KB physical boundary -- the DMA controller
   cannot. */
#define SNDOUT_RING 16384

/* Open the device at `rate` Hz. Returns 0, or -1 with the reason in
   sndout_error(). On DOS this is where BLASTER is read and the card reset;
   on the host, `path` names the WAV file to write. */
int sndout_open(unsigned long rate, const char *path);

void sndout_close(void);

/* How many frames can be written right now without overwriting audio the
   card has not played yet. */
long sndout_space(void);

/* Hand over up to `frames` bytes. Returns how many were taken, which is
   less than offered when the ring is full. */
long sndout_write(const signed char *buf, long frames);

/* Frames the device has played since it was opened. The share door uses it
   to show that the music is actually moving, which on a machine with no
   speaker plugged in is the only way to tell. */
unsigned long sndout_played(void);

/* What the device is, for the info door: "Sound Blaster at 220h, IRQ 5,
   DMA 1" or "WAV file". Never NULL. */
const char *sndout_describe(void);

const char *sndout_error(void);

#endif /* DOSYPP_SNDOUT_H */
