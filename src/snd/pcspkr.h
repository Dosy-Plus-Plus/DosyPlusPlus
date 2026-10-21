/* pcspkr.h - the PC speaker, used sparingly.
 *
 * A BBS beeped: at connect, when a node messaged you, when you fumbled a
 * command. It is part of the feel, and it is also genuinely useful -- a
 * private message arriving while you are reading the file areas should not
 * be silent.
 *
 * Everything here is non-blocking in the sense that matters: a sound is a
 * short queued sequence of tones that snd_tick() advances from the main
 * loop. Nothing sleeps, because the hub connection has to keep being
 * serviced while a jingle plays. That rules out the obvious
 * "outportb; delay(200); outportb" approach.
 *
 * On the host this is a no-op rather than a terminal bell: a bell per
 * private message in someone's terminal is obnoxious, and the host build
 * exists for development.
 */
#ifndef DOSYPP_PCSPKR_H
#define DOSYPP_PCSPKR_H

/* Enable or disable all sound. Off is a legitimate preference and the
   config file carries it. */
void snd_init(int enabled);
void snd_shutdown(void);
int  snd_enabled(void);

/* Advance the queue. `now_ms` is a monotonic millisecond count; the caller
   owns the clock so this needs no timer hook of its own. */
void snd_tick(unsigned long now_ms);

/* A millisecond clock, from the PIT on DOS and gettimeofday on the host.
   Here because the sound queue is the only thing that needs this
   resolution. */
unsigned long snd_millis(void);

/* The named sounds. Each replaces whatever was playing: a queue that can
   back up would leave the speaker droning after a burst of messages. */
void snd_connect(void);      /* logged on: a rising two-tone */
void snd_disconnect(void);   /* carrier lost: a falling slide */
void snd_message(void);      /* someone spoke to you */
void snd_error(void);        /* low buzz */
void snd_keypress_bad(void); /* unknown command: a short click */

#endif /* DOSYPP_PCSPKR_H */
