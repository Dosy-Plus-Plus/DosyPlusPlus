/* mod.c - the ProTracker loader and mixer. See mod.h. */
#include "mod.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Finetune, as a scale on the period rather than a second period table.
 *
 * ProTracker shipped sixteen tables of thirty-six periods; they are all
 * generated from the same ratios, because a finetune step is an eighth of a
 * semitone -- 2^(n/96). Scaling the period by that ratio gives the same
 * answer as looking it up, in 16 entries instead of 576. Times 1024, so the
 * arithmetic is a multiply and a shift and no division by a variable. */
static const int FINETUNE_SCALE[16] = {
    1024, 1016, 1008,  1000,  992,  984,  976,  968,
    1088, 1080, 1072,  1064, 1056, 1048, 1040, 1032
};

/* A semitone down, as a scale on the period: 1024 / 2^(n/12).
 *
 * Arpeggio is "play this note, then n semitones up, then m semitones up,
 * one per tick". ProTracker did it by stepping along its period table;
 * this player computes periods instead of looking them up, so it needs the
 * ratio. Sixteen entries covers every arpeggio parameter, which is a
 * single hex digit each. */
static const int SEMITONE_SCALE[16] = {
    1024, 966, 912, 861, 813, 767, 724, 683,
     645, 609, 575, 542, 512, 483, 456, 430
};

/* ProTracker's vibrato sine, a quarter wave reflected: 32 entries, 0..255.
   Used for tremolo as well, which is the same shape applied to volume. */
static const unsigned char SINE_TABLE[32] = {
      0,  24,  49,  74,  97, 120, 141, 161,
    180, 197, 212, 224, 235, 244, 250, 253,
    255, 253, 250, 244, 235, 224, 212, 197,
    180, 161, 141, 120,  97,  74,  49,  24
};

/* The period range a real Amiga could play, which is also the range
   ProTracker clamped slides to. A slide that ran past it would either go
   silent or alias, and modules rely on the clamp. */
#define PERIOD_MIN 113
#define PERIOD_MAX 856

/* PAL Amiga clock / 2, which is the number a period divides into to give a
   sampling rate. Integer: 3546895 is the standard value every tracker
   uses. */
#define AMIGA_CLOCK 3546895UL

static void seterr(mod_player *m, const char *why)
{
    snprintf(m->err, sizeof m->err, "%s", why);
}

const char *mod_error(const mod_player *m)
{
    return m->err[0] ? m->err : "no error";
}

static unsigned read_be16(const unsigned char *p)
{
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

/* ------------------------------------------------------------- loading */

/* How many patterns a module has: the highest order entry plus one. The
   file does not say, which is the single most awkward thing about the
   format. */
static int count_patterns(const unsigned char *order, int n)
{
    int hi = 0;
    int i;

    for (i = 0; i < n; i++)
        if (order[i] > hi)
            hi = order[i];
    return hi + 1;
}

static void recalc_tick(mod_player *m)
{
    /* Ticks per second are bpm * 2 / 5, as ProTracker had it: at 125 bpm
       that is the familiar 50Hz. The division is done once per tempo
       change rather than per tick. */
    unsigned long ticks_per_sec = ((unsigned long)m->bpm * 2UL) / 5UL;

    if (ticks_per_sec == 0)
        ticks_per_sec = 50;

    m->frames_per_tick = (long)(m->rate / ticks_per_sec);
    if (m->frames_per_tick < 1)
        m->frames_per_tick = 1;
}

int mod_load_mem(mod_player *m, const unsigned char *data, size_t len,
                 unsigned long rate)
{
    size_t at;
    int i;
    int nsample = 31;
    int chans = 4;
    size_t pattern_bytes;

    memset(m, 0, sizeof *m);
    m->rate = rate ? rate : 11025;

    if (len < 1084) {
        seterr(m, "too short to be a module");
        return -1;
    }

    /* The tag at offset 1080 says how many samples and channels. Without
       it the file is a 15-sample original, which has no tag at all -- so
       the absence of a recognised tag is itself the signal. */
    {
        const unsigned char *tag = data + 1080;

        if (memcmp(tag, "M.K.", 4) == 0 || memcmp(tag, "M!K!", 4) == 0 ||
            memcmp(tag, "FLT4", 4) == 0 || memcmp(tag, "4CHN", 4) == 0) {
            nsample = 31;
            chans = 4;
        } else if (memcmp(tag, "6CHN", 4) == 0 ||
                   memcmp(tag, "8CHN", 4) == 0) {
            seterr(m, "more than four channels, which this player does "
                      "not mix");
            return -1;
        } else {
            /* No tag: 15 samples, and the header is 600 bytes shorter. */
            nsample = 15;
            chans = 4;
        }
    }

    m->nsample = nsample;
    m->channels = chans;

    memcpy(m->name, data, MOD_NAME_MAX - 1);
    m->name[MOD_NAME_MAX - 1] = '\0';

    at = 20;
    for (i = 1; i <= nsample; i++) {
        mod_sample *s = &m->sample[i];

        if (at + 30 > len) {
            seterr(m, "the sample headers are truncated");
            return -1;
        }

        memcpy(s->name, data + at, 22);
        s->name[22] = '\0';

        /* Lengths are in words, and the file is big-endian whatever the
           machine is. */
        s->length      = (unsigned long)read_be16(data + at + 22) * 2UL;
        s->finetune    = data[at + 24] & 0x0F;
        s->volume      = data[at + 25];
        s->loop_start  = (unsigned long)read_be16(data + at + 26) * 2UL;
        s->loop_length = (unsigned long)read_be16(data + at + 28) * 2UL;

        if (s->volume > 64)
            s->volume = 64;

        at += 30;
    }

    if (at + 2 > len) {
        seterr(m, "no song length");
        return -1;
    }

    m->norder = data[at];
    if (m->norder > MOD_ORDERS)
        m->norder = MOD_ORDERS;
    at += 2;                        /* length, then the restart byte */

    if (at + MOD_ORDERS > len) {
        seterr(m, "the order list is truncated");
        return -1;
    }
    memcpy(m->order, data + at, MOD_ORDERS);
    at += MOD_ORDERS;

    if (nsample == 31)
        at += 4;                    /* the tag */

    m->npattern = count_patterns(m->order, m->norder);

    pattern_bytes = (size_t)m->npattern * MOD_ROWS * (size_t)chans * 4;
    if (at + pattern_bytes > len) {
        seterr(m, "the pattern data is truncated");
        return -1;
    }

    m->patterns = (unsigned char *)malloc(pattern_bytes);
    if (!m->patterns) {
        seterr(m, "out of memory for the patterns");
        return -1;
    }
    memcpy(m->patterns, data + at, pattern_bytes);
    at += pattern_bytes;

    /* Sample data follows, in order. A module whose last sample is
       truncated is common enough -- trackers wrote them that way -- so a
       short read shortens the sample rather than failing the load. */
    for (i = 1; i <= nsample; i++) {
        mod_sample *s = &m->sample[i];
        size_t have;

        if (s->length == 0)
            continue;

        have = (at < len) ? (len - at) : 0;
        if (have == 0) {
            s->length = 0;
            continue;
        }
        if (have < s->length)
            s->length = (unsigned long)have;

        s->data = (signed char *)malloc((size_t)s->length);
        if (!s->data) {
            seterr(m, "out of memory for the samples");
            mod_free(m);
            return -1;
        }
        memcpy(s->data, data + at, (size_t)s->length);
        at += s->length;

        /* A loop shorter than three bytes is the format's way of saying
           "no loop", and a loop running past the end is a file to be
           tolerated rather than trusted. */
        if (s->loop_length < 4)
            s->loop_length = 0;
        if (s->loop_start > s->length)
            s->loop_start = 0, s->loop_length = 0;
        if (s->loop_start + s->loop_length > s->length)
            s->loop_length = s->length - s->loop_start;
    }

    /* Note which effects appear, so the caller can be told what will be
       ignored rather than discovering it by ear. */
    {
        size_t k;

        for (k = 0; k + 3 < pattern_bytes; k += 4) {
            unsigned eff = m->patterns[k + 2] & 0x0F;
            unsigned param = m->patterns[k + 3];

            /* Effect 0 with no parameter means "no effect", and every
               module is full of it. Counting it would report arpeggio on
               every file ever written. */
            if (eff == 0 && param == 0)
                continue;

            m->effects_seen |= 1UL << eff;
        }
    }

    m->speed = 6;
    m->bpm = 125;
    recalc_tick(m);
    mod_rewind(m);
    return 0;
}

int mod_load(mod_player *m, const char *path, unsigned long rate)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long len;
    int rc;

    memset(m, 0, sizeof *m);

    if (!f) {
        seterr(m, "cannot open that module");
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        seterr(m, "cannot measure that module");
        return -1;
    }
    len = ftell(f);

    /* A megabyte is a big module and a sane bound on a 16MB machine. */
    if (len <= 0 || len > 1024L * 1024L) {
        fclose(f);
        seterr(m, "that file is not a plausible module");
        return -1;
    }

    buf = (unsigned char *)malloc((size_t)len);
    if (!buf) {
        fclose(f);
        seterr(m, "out of memory reading the module");
        return -1;
    }

    rewind(f);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        seterr(m, "could not read the whole module");
        return -1;
    }
    fclose(f);

    rc = mod_load_mem(m, buf, (size_t)len, rate);
    free(buf);
    return rc;
}

void mod_free(mod_player *m)
{
    int i;

    for (i = 0; i <= MOD_SAMPLES; i++) {
        free(m->sample[i].data);
        m->sample[i].data = NULL;
    }
    free(m->patterns);
    m->patterns = NULL;
}

void mod_rewind(mod_player *m)
{
    int i;

    m->order_pos = 0;
    m->row = 0;
    m->tick = 0;
    m->finished = 0;
    m->tick_left = 0;
    m->pending_jump = -1;
    m->pending_break = -1;

    for (i = 0; i < MOD_CHANNELS; i++)
        memset(&m->ch[i], 0, sizeof m->ch[i]);
}

/* ------------------------------------------------------------- playing */

static unsigned long step_for(mod_player *m, unsigned long period,
                              int finetune)
{
    unsigned long rate;
    unsigned long scaled;

    if (period == 0)
        return 0;

    /* Finetune scales the period. The table is x1024, so this is a
       multiply and a shift -- no division by a non-constant and no
       floating point. */
    scaled = (period * (unsigned long)FINETUNE_SCALE[finetune & 0x0F]) >> 10;
    if (scaled == 0)
        scaled = period;

    rate = AMIGA_CLOCK / scaled;

    /* 16.16 increment: how far through the sample one output frame moves.
       Done as a 64-bit-free multiply by splitting the shift. */
    if (m->rate == 0)
        return 0;
    return (unsigned long)(((rate << 8) / m->rate) << 8);
}

/* Set a channel's playback rate from an effective period -- the note's
   period bent by whatever a pitch effect is doing this tick. */
static void set_step(mod_player *m, mod_channel *ch, long period)
{
    if (period < PERIOD_MIN)
        period = PERIOD_MIN;
    if (period > PERIOD_MAX)
        period = PERIOD_MAX;

    if (ch->sample > 0)
        ch->step = step_for(m, (unsigned long)period,
                            m->sample[ch->sample].finetune);
}

/* How far the sine table bends things, given a depth. The shift is 7 for
   vibrato (ProTracker's /128) and 6 for tremolo (/64), which is why this
   takes one. */
static int sine_delta(int pos, int depth, int shift)
{
    int v = ((int)SINE_TABLE[pos & 31] * depth) >> shift;

    return (pos & 32) ? -v : v;
}

/* The row that is playing. */
static const unsigned char *row_at(mod_player *m)
{
    if (m->order_pos >= m->norder)
        return NULL;

    return m->patterns +
           ((size_t)m->order[m->order_pos] * MOD_ROWS + (size_t)m->row) *
           (size_t)m->channels * 4;
}

/* Act on one row: set up the channels, and note any effect that changes
   where we go next. The position is NOT advanced here -- the row has to
   play first. */
static void do_row(mod_player *m)
{
    const unsigned char *row = row_at(m);
    int c;

    if (!row) {
        m->finished = 1;
        return;
    }

    m->pending_jump = -1;
    m->pending_break = -1;

    for (c = 0; c < m->channels && c < MOD_CHANNELS; c++) {
        const unsigned char *n = row + c * 4;
        unsigned period = ((unsigned)(n[0] & 0x0F) << 8) | n[1];
        int sample = (n[0] & 0xF0) | (n[2] >> 4);
        int effect = n[2] & 0x0F;
        int param = n[3];
        mod_channel *ch = &m->ch[c];

        if (sample > 0 && sample <= m->nsample) {
            ch->sample = sample;
            ch->volume = m->sample[sample].volume;
        }

        /* A bend that has stopped has to be undone, or the note the next
           row inherits is wherever the sine or the arpeggio happened to
           leave it. The slides are different: where they left the period
           IS the new note. */
        if ((ch->effect == 0x0 || ch->effect == 0x4 || ch->effect == 0x6) &&
            !(effect == 0x0 || effect == 0x4 || effect == 0x6) &&
            ch->period > 0)
            set_step(m, ch, (long)ch->period);

        ch->effect = effect;
        ch->param = param;
        ch->trem_vol = ch->volume;

        /* Tone portamento is the one effect for which a note is a
           destination rather than a note: 3xx and 5xx slide towards it
           from wherever the channel already is, and must not restart the
           sample. */
        if (period > 0 && (effect == 0x3 || effect == 0x5)) {
            ch->period_target = period;
        } else if (period > 0) {
            ch->period = period;
            ch->period_target = period;
            ch->pos = 0;
            ch->looping = 0;
            /* A new note resets the sine positions, unless the effect's
               own flag says to keep them -- which this player does not
               implement (E4x/E7x), so it always resets. */
            ch->vib_pos = 0;
            ch->trem_pos = 0;
            set_step(m, ch, (long)period);
        }

        switch (effect) {
        case 0x0:       /* arpeggio: the note, then +x, then +y semitones */
            ch->arp_x = param >> 4;
            ch->arp_y = param & 0x0F;
            break;
        case 0x3:       /* tone portamento; a zero speed reuses the last */
            if (param)
                ch->porta_speed = param;
            break;
        case 0x4:       /* vibrato; zero speed or depth reuses the last */
            if (param >> 4)
                ch->vib_speed = param >> 4;
            if (param & 0x0F)
                ch->vib_depth = param & 0x0F;
            break;
        case 0x5:       /* tone portamento + volume slide */
            break;      /* the slide's parameter is the volume one */
        case 0x6:       /* vibrato + volume slide, likewise */
            break;
        case 0x7:       /* tremolo */
            if (param >> 4)
                ch->trem_speed = param >> 4;
            if (param & 0x0F)
                ch->trem_depth = param & 0x0F;
            break;
        case 0x9:       /* sample offset, in 256-byte units */
            if (ch->sample > 0) {
                unsigned long off = (unsigned long)param * 256UL;

                if (off < m->sample[ch->sample].length)
                    ch->pos = off << 16;
            }
            break;
        case 0xC:       /* set volume */
            ch->volume = (param > 64) ? 64 : param;
            break;
        case 0xB:       /* position jump */
            m->pending_jump = param;
            break;
        case 0xD:       /* pattern break, in BCD because of course */
            m->pending_break = (param >> 4) * 10 + (param & 0x0F);
            break;
        case 0xF:       /* set speed or tempo */
            if (param == 0)
                break;
            if (param < 32) {
                m->speed = param;
            } else {
                m->bpm = param;
                recalc_tick(m);
            }
            break;
        default:
            break;
        }
    }

}

/* Move to the next row, honouring whatever the row that just finished
   asked for. A jump and a break can both appear, and the jump wins the
   pattern while the break chooses the row -- which is what trackers did
   and what modules written on them expect. */
static void advance_row(mod_player *m)
{
    if (m->pending_jump >= 0) {
        m->order_pos = m->pending_jump;
        m->row = (m->pending_break >= 0 && m->pending_break < MOD_ROWS)
                 ? m->pending_break : 0;
    } else if (m->pending_break >= 0) {
        m->order_pos++;
        m->row = (m->pending_break < MOD_ROWS) ? m->pending_break : 0;
    } else {
        m->row++;
        if (m->row >= MOD_ROWS) {
            m->row = 0;
            m->order_pos++;
        }
    }

    m->pending_jump = -1;
    m->pending_break = -1;

    if (m->order_pos >= m->norder)
        m->finished = 1;
}

/* One channel's volume slide, which three effects share: A, 5 and 6. */
static void volume_slide(mod_channel *ch, int param)
{
    if (param >> 4)
        ch->volume += param >> 4;
    else
        ch->volume -= param & 0x0F;

    if (ch->volume < 0)
        ch->volume = 0;
    if (ch->volume > 64)
        ch->volume = 64;
}

/* Per-tick work: everything that moves between rows rather than on them.
 *
 * Two kinds live here and they are kept apart deliberately. The slides
 * (1, 2, 3, 5, A) change the channel's own period or volume, so where they
 * leave it is where the next row starts. Vibrato, tremolo and arpeggio
 * (0, 4, 6, 7) do not: they bend what is played this tick and leave the
 * note where it was, which is why a vibrato ends on the note it began on.
 *
 * Called for ticks 1..speed-1. Arpeggio is the exception: it has to act on
 * tick 0 as well, because its first tick is the unbent note and its cycle
 * is three ticks long. */
static void do_tick(mod_player *m)
{
    int c;

    for (c = 0; c < m->channels && c < MOD_CHANNELS; c++) {
        mod_channel *ch = &m->ch[c];
        int param = ch->param;
        long period;

        if (ch->sample <= 0)
            continue;

        switch (ch->effect) {
        case 0x0:                           /* arpeggio */
            if (ch->arp_x == 0 && ch->arp_y == 0)
                break;
            {
                int which = m->tick % 3;
                int semis = (which == 0) ? 0
                          : (which == 1) ? ch->arp_x : ch->arp_y;

                period = (long)((ch->period *
                                 (unsigned long)SEMITONE_SCALE[semis & 0x0F])
                                >> 10);
                set_step(m, ch, period);
            }
            break;

        case 0x1:                           /* portamento up: period down */
            if (ch->period > (unsigned long)param)
                ch->period -= (unsigned long)param;
            else
                ch->period = PERIOD_MIN;
            if (ch->period < PERIOD_MIN)
                ch->period = PERIOD_MIN;
            set_step(m, ch, (long)ch->period);
            break;

        case 0x2:                           /* portamento down */
            ch->period += (unsigned long)param;
            if (ch->period > PERIOD_MAX)
                ch->period = PERIOD_MAX;
            set_step(m, ch, (long)ch->period);
            break;

        case 0x3:                           /* tone portamento */
        case 0x5:                           /* and with a volume slide */
            if (ch->effect == 0x5)
                volume_slide(ch, param);
            if (ch->period_target && ch->porta_speed) {
                if (ch->period > ch->period_target) {
                    ch->period -= (unsigned long)ch->porta_speed;
                    if (ch->period < ch->period_target)
                        ch->period = ch->period_target;
                } else if (ch->period < ch->period_target) {
                    ch->period += (unsigned long)ch->porta_speed;
                    if (ch->period > ch->period_target)
                        ch->period = ch->period_target;
                }
                set_step(m, ch, (long)ch->period);
            }
            break;

        case 0x4:                           /* vibrato */
        case 0x6:                           /* and with a volume slide */
            if (ch->effect == 0x6)
                volume_slide(ch, param);
            ch->vib_pos = (ch->vib_pos + ch->vib_speed) & 63;
            period = (long)ch->period +
                     sine_delta(ch->vib_pos, ch->vib_depth, 7);
            set_step(m, ch, period);
            break;

        case 0x7:                           /* tremolo: vibrato on volume */
            {
                int vol;

                ch->trem_pos = (ch->trem_pos + ch->trem_speed) & 63;
                vol = ch->volume +
                      sine_delta(ch->trem_pos, ch->trem_depth, 6);
                if (vol < 0)
                    vol = 0;
                if (vol > 64)
                    vol = 64;
                /* Applied to the mix without disturbing the channel's own
                   volume, which the next row inherits. */
                ch->trem_vol = vol;
            }
            break;

        case 0xA:                           /* volume slide */
            volume_slide(ch, param);
            break;

        default:
            break;
        }
    }
}

long mod_mix(mod_player *m, signed char *buf, long frames)
{
    long done = 0;

    if (!m->patterns)
        return 0;

    while (done < frames) {
        long chunk;
        long i;

        if (m->tick_left <= 0) {
            if (m->finished)
                break;

            /* The row is acted on at tick 0; the ticks after it carry the
               effects that work per tick. The position moves only when
               the last tick of the row has been played. */
            if (m->tick == 0)
                do_row(m);
            else
                do_tick(m);

            m->tick++;
            if (m->tick >= m->speed) {
                m->tick = 0;
                advance_row(m);
            }

            m->tick_left = m->frames_per_tick;
        }

        chunk = frames - done;
        if (chunk > m->tick_left)
            chunk = m->tick_left;

        for (i = 0; i < chunk; i++) {
            int acc = 0;
            int c;

            for (c = 0; c < m->channels && c < MOD_CHANNELS; c++) {
                mod_channel *ch = &m->ch[c];
                mod_sample *s;
                unsigned long idx;

                if (ch->sample <= 0 || ch->step == 0)
                    continue;

                s = &m->sample[ch->sample];
                if (!s->data || s->length == 0)
                    continue;

                idx = ch->pos >> 16;

                if (idx >= s->length) {
                    if (s->loop_length > 0) {
                        /* Wrap into the loop rather than restarting: a
                           sample's attack is not part of its loop. */
                        unsigned long into = idx - s->loop_start;

                        if (s->loop_length)
                            into %= s->loop_length;
                        idx = s->loop_start + into;
                        ch->pos = idx << 16;
                        ch->looping = 1;
                    } else {
                        ch->step = 0;       /* done: silence this channel */
                        continue;
                    }
                }

                /* Tremolo bends the volume for this tick only, so the
                   channel's own volume is what the next row inherits. */
                acc += ((int)s->data[idx] *
                        (ch->effect == 0x7 ? ch->trem_vol : ch->volume)) >> 6;
                ch->pos += ch->step;
            }

            /* Four channels at full volume can reach four times a
               sample's range, so the mix is divided rather than clipped:
               clipping four channels of a loud module sounds like a fault
               and dividing sounds like a mix. */
            acc /= 4;
            if (acc > 127)
                acc = 127;
            if (acc < -128)
                acc = -128;

            buf[done + i] = (signed char)acc;
        }

        done += chunk;
        m->tick_left -= chunk;
    }

    return done;
}

void mod_unsupported(const mod_player *m, char *out, size_t outsz)
{
    /* Effects this mixer acts on. Everything else in a module will be
       heard as a plain note, which is usually close and occasionally
       wrong -- and either way the caller gets told. */
    static const struct { int eff; const char *name; } IGNORED[] = {
        { 0x8, "set panning" },         /* meaningless in mono */
        { 0xE, "extended effects" }     /* fine slides, loops, retrigger */
    };
    size_t used = 0;
    int i;

    if (outsz == 0)
        return;
    out[0] = '\0';

    for (i = 0; i < (int)(sizeof IGNORED / sizeof IGNORED[0]); i++) {
        int bit = IGNORED[i].eff;

        if (!(m->effects_seen & (1UL << bit)))
            continue;

        if (used > 0 && used + 2 < outsz) {
            out[used++] = ',';
            out[used++] = ' ';
        }
        snprintf(out + used, outsz - used, "%s", IGNORED[i].name);
        used = strlen(out);
        if (used + 4 >= outsz)
            break;
    }
}
