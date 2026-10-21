/* mus.h - the music, as the rest of the program sees it.
 *
 * One module playing, pumped from the main loop. Everything awkward about
 * that lives here: how much to mix per pass, what to do when the card is
 * not there, and whether to loop.
 *
 * Why the main loop and not an interrupt: this client has one thread and an
 * int 9h handler it cares about. Mixing four channels in an ISR on a 486
 * would be the thing that eats keystrokes, and a missed keystroke is worse
 * than a stutter. The mixer is cheap enough to call forty times a second
 * between network polls.
 */
#ifndef DOSYPP_MUS_H
#define DOSYPP_MUS_H

#include <stddef.h>

/* Frames mixed per call to the mixer. At 11kHz this is 46ms of audio, and
   on a 486DX2-66 it costs about 2.6ms (make dos-bench-run). */
#define MUS_SLICE 512

/* How many of those slices one mus_tick() will do.
 *
 * One was the obvious answer and it was wrong. A main-loop pass that hashes
 * HASHQ_BUDGET (64KB) takes 126ms on a 486 at 521 KB/s, so a fixed slice per
 * pass supplies 4,060 frames a second against 11,025 needed: the ring drains
 * in a couple of seconds and the music stutters for as long as the share is
 * being hashed. Measured, not reasoned about -- test/dos/musbench.c phase 3.
 *
 * So a tick mixes as much as the device has room for, up to this. Eight
 * slices is 370ms of audio for about 21ms of work, which covers a pass three
 * times longer than the hashing one and still cannot be felt at the
 * keyboard. Unbounded would mean filling the whole 1.5s ring in one pass,
 * which is the hitch this design exists to avoid. */
#define MUS_MAX_SLICES 8

/* Start playing `path` at `rate` Hz, writing to `wav_path` on the host
   (ignored on DOS, where the Sound Blaster is the output). Returns 0, or
   -1 with the reason in mus_error() -- a missing card or a missing file is
   a reason to carry on without music, not to stop. */
int mus_start(const char *path, unsigned long rate, const char *wav_path);

void mus_stop(void);

/* Mix and hand over whatever the device has room for. Call it often; it
   returns at once when there is nothing to do. */
void mus_tick(void);

int  mus_playing(void);

/* Keep going round when the module ends. A BBS tune looped; so does this,
   by default. */
void mus_set_loop(int on);

/* The module's own title, or "" -- which is the name whoever tracked it
   typed into the first twenty bytes of the file. */
const char *mus_title(void);

/* What it is playing through: "Sound Blaster at 220h, IRQ 5, DMA 1" or a
   WAV file. For the info door. */
const char *mus_device(void);

/* Effects the module uses that the mixer ignores, or "" -- so the info
   door can be honest about a tune it is not playing faithfully. */
const char *mus_caveats(void);

/* How many times round it has been, for the info door. */
unsigned long mus_loops(void);

/* Frames the mixer has handed to the device.
 *
 * Not the same as mus_frames(), and the difference is the whole point: an
 * auto-init DMA ring is read by the card whether or not anybody refills it,
 * so a starved ring replays stale audio and the played count keeps
 * advancing at the full rate. Underrun shows up here, as fed falling behind
 * played, and nowhere else. */
unsigned long mus_fed(void);

/* Frames the device says it has played. On DOS that number comes from the
   DMA controller's own counter, so it advancing is evidence the card is
   really reading the ring -- which is the only thing a test rig with no
   speaker can measure. */
unsigned long mus_frames(void);

const char *mus_error(void);

#endif /* DOSYPP_MUS_H */
