/* mod.h - ProTracker modules: load one, mix it to PCM.
 *
 * Why this exists: a BBS played music. Not as decoration -- the MOD was
 * half the identity of a board, and a warez client from 1994 without one is
 * a file manager with a border.
 *
 * What this is:
 *
 *   - A loader for 31-sample, 4-channel ProTracker files (M.K., 4CHN, and
 *     the 15-sample originals), which is what a BBS would have played.
 *   - A mixer that produces signed 8-bit mono PCM at a rate the caller
 *     picks, in slices, so the main loop stays answerable. There is no
 *     thread to put it on and no interrupt to do it from: a 486 mixing four
 *     channels is affordable, a 486 doing it inside an ISR is not.
 *
 * Integer arithmetic throughout. The whole program is built -msoft-float,
 * so a stray double would fail at link time, and on a 486SX it would trap.
 * The mixing position is a 16.16 fixed-point counter, finetune is a
 * multiply and a shift, and volume is the same.
 *
 * Effects: 0-7 (arpeggio, portamento up/down, tone portamento, vibrato,
 * the two combined slides, tremolo), 9 (sample offset), A (volume slide),
 * B (position jump), C (set volume), D (pattern break) and F (speed and
 * tempo). Left out: 8 (panning, which is meaningless in mono) and the
 * E-series. mod_unsupported() reports what a loaded module uses that this
 * does not, because a partial player that says so is worth more than one
 * that silently plays the wrong notes.
 *
 * The period arithmetic for pitch effects is two small x1024 tables --
 * semitones for arpeggio, a sine for vibrato and tremolo -- so a 486 pays
 * a multiply and a shift per channel per tick and nothing pays for a
 * division by a variable.
 */
#ifndef DOSYPP_MOD_H
#define DOSYPP_MOD_H

#include <stddef.h>

#define MOD_CHANNELS    4
#define MOD_SAMPLES     31
#define MOD_NAME_MAX    21      /* 20 chars plus NUL */
#define MOD_ORDERS      128
#define MOD_ROWS        64

/* A sample's header, as the file carries it. Lengths are in bytes, already
   doubled from the file's word counts. */
typedef struct {
    char           name[MOD_NAME_MAX + 2];
    unsigned long  length;
    unsigned long  loop_start;
    unsigned long  loop_length;
    int            finetune;        /* -8..7 */
    int            volume;          /* 0..64 */
    signed char   *data;            /* length bytes, or NULL */
} mod_sample;

/* One channel's state while playing. */
typedef struct {
    int           sample;           /* 1..31, or 0 for none */
    unsigned long period;           /* the note's period; 0 is silence */
    int           volume;           /* 0..64 */
    unsigned long pos;              /* 16.16 into the sample */
    unsigned long step;             /* 16.16 increment per output frame */
    int           looping;

    /* Pitch effects work on a base period and a per-tick offset: the slide
       effects move `period` itself, while vibrato and arpeggio leave it
       alone and bend what is played. Keeping them apart is what lets a
       vibrato end on the note it started on. */
    unsigned long period_target;    /* tone portamento destination */
    int           porta_speed;      /* remembered: 3xx and 5xx with x=0 */

    int           vib_pos;          /* 0..63 round the sine table */
    int           vib_speed;
    int           vib_depth;

    int           trem_vol;         /* tremolo's volume for this tick */
    int           trem_pos;
    int           trem_speed;
    int           trem_depth;

    int           arp_x;            /* arpeggio's two semitone offsets */
    int           arp_y;

    int           effect;           /* the row's effect, for the tick work */
    int           param;
} mod_channel;

typedef struct {
    char          name[MOD_NAME_MAX + 2];
    mod_sample    sample[MOD_SAMPLES + 1];   /* 1-based, [0] unused */
    int           nsample;

    unsigned char order[MOD_ORDERS];
    int           norder;
    int           npattern;
    unsigned char *patterns;        /* npattern * 64 * channels * 4 bytes */

    int           channels;

    /* Playback position. */
    int           order_pos;
    int           row;
    int           tick;             /* which tick of the current row */
    int           speed;            /* ticks per row, default 6 */

    /* Set by the row, acted on when the row ends: a jump and a break can
       both appear on one row, and both have to wait for it to finish
       playing. -1 means neither. */
    int           pending_jump;
    int           pending_break;
    int           bpm;              /* default 125 */
    int           finished;         /* ran off the end and did not loop */

    mod_channel   ch[MOD_CHANNELS];

    /* Output. */
    unsigned long rate;
    long          frames_per_tick;  /* at the current bpm */
    long          tick_left;

    unsigned long effects_seen;     /* bitmask of effect numbers used */
    char          err[128];
} mod_player;

/* Load a module from a file. Returns 0, or -1 with the reason in
   mod_error(). The player owns the sample data until mod_free. */
int mod_load(mod_player *m, const char *path, unsigned long rate);

/* Load from memory, which is how the built-in tune is played: it is
   compiled in rather than shipped as a file nobody can edit. */
int mod_load_mem(mod_player *m, const unsigned char *data, size_t len,
                 unsigned long rate);

void mod_free(mod_player *m);

const char *mod_error(const mod_player *m);

/* Mix `frames` signed 8-bit mono samples into buf. Returns the number
   written, which is less than asked for only at the end of a module that
   is not looping. */
long mod_mix(mod_player *m, signed char *buf, long frames);

/* Start again from the top. */
void mod_rewind(mod_player *m);

/* Which effects the module uses that this player ignores, as a readable
   list, or "" if it uses none. Exposed so the art pack can be chosen to
   play correctly rather than approximately. */
void mod_unsupported(const mod_player *m, char *out, size_t outsz);

#endif /* DOSYPP_MOD_H */
