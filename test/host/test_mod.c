/* test_mod.c - the module player.
 *
 * "It makes a noise" is not a test. These measure what came out:
 *
 *   - the pitch, by counting zero crossings in the rendered PCM and
 *     comparing with the frequency the Amiga period asks for;
 *   - the timing, by finding where the note stops and checking it against
 *     the row and tick arithmetic;
 *   - the length of the whole render against speed, bpm and pattern count.
 *
 * That is possible because the fixture is generated rather than borrowed:
 * tools/makemod.py writes a module whose samples and notes are known
 * exactly. A real tune would only let us check that the output does not
 * change, which catches regressions but proves nothing about correctness.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/snd/mod.h"

static int g_fail;
static int g_run;

#define CHECK(cond, ...) do {                                              \
        g_run++;                                                           \
        if (!(cond)) {                                                     \
            printf("FAIL line %d: ", __LINE__);                            \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

#define RATE 22050L

static char g_test_mod[384];
static char g_tune_mod[384];
static char g_fx_mod[384];

/* Count sign changes: for a square wave that is two per period, so the
   frequency is crossings / 2 / seconds. Zeros are skipped rather than
   counted as a side, because a silent run between notes would otherwise
   read as a very high pitch. */
static long count_crossings(const signed char *buf, long n)
{
    long crossings = 0;
    int last = 0;
    long i;

    for (i = 0; i < n; i++) {
        int sign = (buf[i] > 0) ? 1 : (buf[i] < 0) ? -1 : 0;

        if (sign == 0)
            continue;
        if (last != 0 && sign != last)
            crossings++;
        last = sign;
    }
    return crossings;
}

static long first_nonzero(const signed char *buf, long n)
{
    long i;

    for (i = 0; i < n; i++)
        if (buf[i] != 0)
            return i;
    return -1;
}

static long last_nonzero(const signed char *buf, long n)
{
    long i;

    for (i = n - 1; i >= 0; i--)
        if (buf[i] != 0)
            return i;
    return -1;
}

/* The tone the fixture should produce.
 *
 * The sample is a 64-byte square wave with one full cycle in those 64
 * bytes. At Amiga period 428 the sample is clocked at 3546895/428 = 8287
 * bytes per second, so the wave repeats 8287/64 = 129.5 times a second. */
static void test_pitch(void)
{
    mod_player m;
    signed char *buf;
    const long frames = RATE;           /* one second */
    long got;
    long crossings;
    long hz;

    CHECK(mod_load(&m, g_test_mod, RATE) == 0,
          "could not load the test module: %s", mod_error(&m));
    if (m.err[0])
        return;

    CHECK(m.channels == 4, "channels is %d", m.channels);
    CHECK(m.nsample == 31, "nsample is %d", m.nsample);
    CHECK(m.norder == 1, "norder is %d", m.norder);
    CHECK(m.npattern == 1, "npattern is %d", m.npattern);
    CHECK(m.speed == 6, "speed is %d", m.speed);
    CHECK(m.bpm == 125, "bpm is %d", m.bpm);

    buf = (signed char *)malloc((size_t)frames);
    CHECK(buf != NULL, "out of memory");
    if (!buf) { mod_free(&m); return; }

    memset(buf, 0, (size_t)frames);
    got = mod_mix(&m, buf, frames);
    CHECK(got == frames, "mixed %ld of %ld frames", got, frames);

    /* Something came out, and it started at once rather than after a
       row's worth of silence. */
    CHECK(first_nonzero(buf, got) >= 0, "the render is silent");
    CHECK(first_nonzero(buf, got) < RATE / 20,
          "the first sound is %ld frames in, which is late",
          first_nonzero(buf, got));

    /* The note runs for 16 rows at 6 ticks and 50 ticks a second: 1.92
       seconds, so a one-second render is all note. */
    crossings = count_crossings(buf, got);
    hz = crossings / 2;

    CHECK(hz > 120 && hz < 140,
          "the tone measured %ld Hz, expected about 129", hz);

    free(buf);
    mod_free(&m);
}

/* The note stops when the pattern says so, not when the sample runs out:
   row 16 sets the volume to zero, which at speed 6 and 50 ticks a second
   is 16 * 6 / 50 = 1.92 seconds in. */
static void test_timing(void)
{
    mod_player m;
    signed char *buf;
    const long frames = RATE * 3;
    long got;
    long last;
    long expect;

    CHECK(mod_load(&m, g_test_mod, RATE) == 0, "load failed: %s",
          mod_error(&m));
    if (m.err[0])
        return;

    buf = (signed char *)malloc((size_t)frames);
    if (!buf) { mod_free(&m); return; }
    memset(buf, 0, (size_t)frames);

    got = mod_mix(&m, buf, frames);
    last = last_nonzero(buf, got);
    expect = (RATE * 16 * 6) / 50;

    CHECK(last > 0, "nothing was rendered");
    /* Within a tick either way: the row boundary is a tick boundary and
       the mixer fills whole ticks. */
    CHECK(last > expect - (RATE / 50) * 2 && last < expect + (RATE / 50) * 2,
          "the note ended at frame %ld, expected about %ld", last, expect);

    /* After that, silence rather than a stuck sample. */
    {
        long i;
        int quiet = 1;

        for (i = expect + (RATE / 25); i < got; i++)
            if (buf[i] != 0) { quiet = 0; break; }
        CHECK(quiet, "there is sound after the volume was set to zero");
    }

    free(buf);
    mod_free(&m);
}

/* A module ends. One pattern of 64 rows at speed 6, 50 ticks a second, is
   7.68 seconds; after that mod_mix must report fewer frames than asked
   for rather than looping silently for ever. */
static void test_ends(void)
{
    mod_player m;
    signed char buf[4096];
    long total = 0;
    int guard;

    CHECK(mod_load(&m, g_test_mod, RATE) == 0, "load failed");
    if (m.err[0])
        return;

    for (guard = 0; guard < 1000; guard++) {
        long n = mod_mix(&m, buf, (long)sizeof buf);

        total += n;
        if (n < (long)sizeof buf)
            break;
    }

    CHECK(guard < 1000, "the module never ended");
    CHECK(m.finished, "the player does not think it finished");

    {
        long expect = (RATE * 64 * 6) / 50;

        CHECK(total > expect - RATE / 10 && total < expect + RATE / 10,
              "rendered %ld frames, expected about %ld", total, expect);
    }

    /* And rewinding starts it again. */
    mod_rewind(&m);
    CHECK(!m.finished, "rewind did not clear the finished flag");
    CHECK(mod_mix(&m, buf, 64) == 64, "nothing came out after a rewind");

    mod_free(&m);
}

/* The tune that ships has to load, play, and use only effects this mixer
   implements -- otherwise what people hear is not what was written. */
static void test_tune(void)
{
    mod_player m;
    signed char buf[8192];
    char missing[160];
    long total = 0;
    int guard;
    long loudest = 0;

    CHECK(mod_load(&m, g_tune_mod, RATE) == 0,
          "could not load the shipped tune: %s", mod_error(&m));
    if (m.err[0])
        return;

    CHECK(m.norder == 4, "the tune has %d orders, expected 4", m.norder);
    CHECK(m.npattern == 2, "the tune has %d patterns, expected 2",
          m.npattern);

    mod_unsupported(&m, missing, sizeof missing);
    CHECK(missing[0] == '\0',
          "the shipped tune uses effects this player ignores: %s", missing);

    for (guard = 0; guard < 2000; guard++) {
        long n = mod_mix(&m, buf, (long)sizeof buf);
        long i;

        for (i = 0; i < n; i++) {
            long v = buf[i] < 0 ? -(long)buf[i] : (long)buf[i];

            if (v > loudest)
                loudest = v;
        }

        total += n;
        if (n < (long)sizeof buf)
            break;
    }

    /* Four patterns of 64 rows: about half a minute. */
    CHECK(total > RATE * 20, "the tune is only %ld frames", total);
    CHECK(loudest > 20, "the tune is nearly silent (peak %ld)", loudest);
    CHECK(loudest <= 127, "the mix clipped at %ld", loudest);

    mod_free(&m);
}

/* Modules come from strangers too -- the art pack is a file on disk and
   somebody will point this at their own. None of these may crash. */
static void test_hostile(void)
{
    mod_player m;
    unsigned char buf[2048];
    char path[384];
    FILE *f;

    /* Too short. */
    memset(buf, 0, sizeof buf);
    CHECK(mod_load_mem(&m, buf, 100, RATE) != 0,
          "a 100-byte file loaded as a module");

    /* Long enough, but all zeros: no tag, so it reads as a 15-sample
       original with no patterns worth playing. It must either refuse or
       load to silence, and either way not crash. */
    if (mod_load_mem(&m, buf, sizeof buf, RATE) == 0) {
        signed char out[256];

        (void)mod_mix(&m, out, (long)sizeof out);
        mod_free(&m);
    }

    /* A tag claiming more channels than this mixer has. */
    memset(buf, 0, sizeof buf);
    memcpy(buf + 1080, "8CHN", 4);
    CHECK(mod_load_mem(&m, buf, sizeof buf, RATE) != 0,
          "an 8-channel module was accepted");
    CHECK(strstr(mod_error(&m), "channel") != NULL,
          "the refusal does not mention channels: %s", mod_error(&m));

    /* A sample claiming to be enormous, with nothing behind it. The
       loader must shorten it rather than reading past the buffer. */
    memset(buf, 0, sizeof buf);
    memcpy(buf + 1080, "M.K.", 4);
    buf[20 + 22] = 0xFF;            /* sample 1 length, high byte */
    buf[20 + 23] = 0xFF;
    buf[950] = 1;                   /* one order */
    if (mod_load_mem(&m, buf, sizeof buf, RATE) == 0) {
        signed char out[256];

        CHECK(m.sample[1].length <= sizeof buf,
              "a sample claimed %lu bytes from a %lu byte file",
              m.sample[1].length, (unsigned long)sizeof buf);
        (void)mod_mix(&m, out, (long)sizeof out);
        mod_free(&m);
    }

    /* A loop running past the end of its sample. */
    memset(buf, 0, sizeof buf);
    memcpy(buf + 1080, "M.K.", 4);
    buf[20 + 22] = 0x00; buf[20 + 23] = 0x10;   /* 32 bytes */
    buf[20 + 26] = 0x00; buf[20 + 27] = 0x08;   /* loop start 16 */
    buf[20 + 28] = 0xFF; buf[20 + 29] = 0xFF;   /* absurd loop length */
    buf[950] = 1;
    if (mod_load_mem(&m, buf, sizeof buf, RATE) == 0) {
        signed char out[1024];

        CHECK(m.sample[1].loop_start + m.sample[1].loop_length <=
              m.sample[1].length,
              "a loop runs past the end of its sample");
        (void)mod_mix(&m, out, (long)sizeof out);
        mod_free(&m);
    }

    /* A file that is not there. */
    CHECK(mod_load(&m, "/nonexistent/none.mod", RATE) != 0,
          "a missing file loaded");

    /* A file of text. */
    snprintf(path, sizeof path, "%s", "test_mod_junk.tmp");
    f = fopen(path, "wb");
    if (f) {
        int i;

        for (i = 0; i < 2000; i++)
            fputc('A' + (i % 26), f);
        fclose(f);

        /* Either refused or loaded as nonsense that plays without
           crashing. Both are acceptable; a crash is not. */
        if (mod_load(&m, path, RATE) == 0) {
            signed char out[512];

            (void)mod_mix(&m, out, (long)sizeof out);
            mod_free(&m);
        }
        remove(path);
    }
}

/* ------------------------------------------------------- the effects
 *
 * Each row of FXTEST.MOD holds one note and does one thing to it, so the
 * pitch the mixer actually produced over that row can be compared with the
 * period arithmetic the effect is supposed to perform. The measurement is
 * zero crossings, same as test_pitch: for this sample the frequency in Hz
 * is the Amiga clock divided by the period, divided by the 64-byte wave.
 */

/* Frames in one row at speed 6, 125 bpm: six ticks of a fiftieth of a
   second. */
#define ROW_FRAMES ((RATE * 6L) / 50L)

/* The pitch a period produces with this fixture's sample. 3546895/period
   bytes a second through a 64-byte wave. */
static long hz_of_period(long period)
{
    return (3546895L / period) / 64L;
}

/* Hz measured over [from, from+len) of a render. */
static long hz_in(const signed char *buf, long from, long len)
{
    long crossings = count_crossings(buf + from, len);

    /* crossings / 2 cycles over len frames, scaled to a second. */
    return (crossings * RATE) / (2L * len);
}

/* The pitch range over a window, measured from the distance between
   successive zero crossings rather than by counting them.
 *
 * Counting crossings over a short window is coarse: one tick at 22kHz is
 * 810 frames, which holds four cycles of a 129 Hz tone, so the answer comes
 * in steps of about twelve Hz and a vibrato of eight disappears into the
 * rounding. The gap between two crossings is half a wave, and that is
 * exact. */
static void hz_range(const signed char *buf, long from, long len,
                     long *lo, long *hi)
{
    long prev = -1;
    int last = 0;
    long i;

    *lo = 1000000;
    *hi = 0;

    for (i = from; i < from + len; i++) {
        int sign = (buf[i] > 0) ? 1 : (buf[i] < 0) ? -1 : 0;

        if (sign == 0)
            continue;
        if (last != 0 && sign != last) {
            if (prev >= 0) {
                long gap = i - prev;
                long hz = gap > 0 ? RATE / (2 * gap) : 0;

                if (hz < *lo) *lo = hz;
                if (hz > *hi) *hi = hz;
            }
            prev = i;
        }
        last = sign;
    }

    if (*hi == 0) {                 /* silence: say so rather than lie */
        *lo = 0;
        *hi = 0;
    }
}

/* The pitch at the start and the end of a window, from the first and last
   half-wave in it. */
static void hz_ends(const signed char *buf, long from, long len,
                    long *first, long *last_out)
{
    long prev = -1, firsthz = 0, lasthz = 0;
    int last = 0;
    long i;

    for (i = from; i < from + len; i++) {
        int sign = (buf[i] > 0) ? 1 : (buf[i] < 0) ? -1 : 0;

        if (sign == 0)
            continue;
        if (last != 0 && sign != last) {
            if (prev >= 0) {
                long gap = i - prev;
                long hz = gap > 0 ? RATE / (2 * gap) : 0;

                if (!firsthz)
                    firsthz = hz;
                lasthz = hz;
            }
            prev = i;
        }
        last = sign;
    }

    *first = firsthz;
    *last_out = lasthz;
}

static void test_effects(void)
{
    mod_player m;
    signed char *buf;
    const long frames = ROW_FRAMES * 10L;       /* the ten fixture rows */
    long got;

    CHECK(mod_load(&m, g_fx_mod, RATE) == 0,
          "could not load %s: %s", g_fx_mod, mod_error(&m));
    if (m.err[0])
        return;

    /* Nothing in this fixture should be reported as unsupported any more:
       that list is what the info door shows, and it was the list of things
       this player could not do. */
    {
        char why[160];

        mod_unsupported(&m, why, sizeof why);
        CHECK(why[0] == '\0',
              "the effect fixture reports unsupported effects: %s", why);
    }

    buf = (signed char *)malloc((size_t)frames);
    CHECK(buf != NULL, "out of memory");
    if (!buf) { mod_free(&m); return; }
    memset(buf, 0, (size_t)frames);

    got = mod_mix(&m, buf, frames);
    CHECK(got == frames, "mixed %ld of %ld frames", got, frames);
    if (got < frames) { free(buf); mod_free(&m); return; }

    /* Row 0: the baseline. Period 428 -> about 129 Hz. */
    {
        long base = hz_in(buf, 0, ROW_FRAMES);

        CHECK(base > 120 && base < 140,
              "row 0 measured %ld Hz, expected about %ld",
              base, hz_of_period(428));

        /* Row 1, portamento up (1 10): the period drops by 16 on each of
           the five ticks after the first, 428 -> 348, so the pitch rises.
           Measured over the row's second half, by which time most of the
           slide has happened. */
        {
            long first = hz_in(buf, ROW_FRAMES, ROW_FRAMES / 2);
            long last  = hz_in(buf, ROW_FRAMES + ROW_FRAMES / 2,
                               ROW_FRAMES / 2);

            CHECK(last > first + 5,
                  "portamento up did not rise: %ld Hz then %ld Hz",
                  first, last);
            CHECK(last > 140 && last < 175,
                  "portamento up ended at %ld Hz, expected about %ld",
                  last, hz_of_period(428 - 5 * 16));
        }

        /* Row 2, portamento down (2 10): a new note resets the period to
           428, and it slides the other way to 508. */
        {
            long first, last;

            hz_ends(buf, 2 * ROW_FRAMES, ROW_FRAMES, &first, &last);

            CHECK(first > last + 3,
                  "portamento down did not fall: %ld Hz then %ld Hz",
                  first, last);
            CHECK(last > 95 && last < 125,
                  "portamento down ended at %ld Hz, expected about %ld",
                  last, hz_of_period(428 + 5 * 16));
        }

        /* Row 3, arpeggio (0 47): the note, then four semitones up, then
           seven, one per tick. Each tick is a sixth of the row, so the
           first three ticks should measure three different pitches, rising.
           A player that ignored the effect would measure the same pitch
           three times -- which is exactly what this used to do. */
        {
            long tick = ROW_FRAMES / 6;
            long at = 3 * ROW_FRAMES;
            long t0 = hz_in(buf, at, tick);
            long t1 = hz_in(buf, at + tick, tick);
            long t2 = hz_in(buf, at + 2 * tick, tick);

            CHECK(t1 > t0 + 8 && t2 > t1 + 8,
                  "arpeggio did not step up: %ld, %ld, %ld Hz", t0, t1, t2);
            /* Four semitones up from 129 Hz is about 163, seven is 194.
               Generous bounds: one tick is a short window to count in. */
            CHECK(t1 > 145 && t1 < 185,
                  "arpeggio's +4 semitones measured %ld Hz, expected ~163",
                  t1);
            CHECK(t2 > 175 && t2 < 215,
                  "arpeggio's +7 semitones measured %ld Hz, expected ~194",
                  t2);
            /* And it comes back round: tick 3 is the base note again. */
            {
                long t3 = hz_in(buf, at + 3 * tick, tick);

                CHECK(t3 < t1,
                      "arpeggio did not return to the note: %ld Hz", t3);
            }
        }

        /* Row 4, vibrato (4 88): the pitch moves either side of the note
           and the row still averages near it. Two things to check, because
           a player that bent the base period instead of the played one
           would pass the first and fail the second. */
        {
            long at = 4 * ROW_FRAMES;
            long lo, hi;
            long whole = hz_in(buf, at, ROW_FRAMES);

            hz_range(buf, at, ROW_FRAMES, &lo, &hi);

            /* Depth 8 bends the period by up to 15 either way: 413..443,
               which is 126..134 Hz. The check is that it moved at all and
               by roughly that much -- not the exact sine, which would be
               testing the table against itself. */
            CHECK(hi > lo + 4,
                  "vibrato did not move the pitch: %ld..%ld Hz", lo, hi);
            CHECK(hi < 160 && lo > 100,
                  "vibrato's swing is %ld..%ld Hz, which is far more than "
                  "depth 8 asks for", lo, hi);
            CHECK(whole > 115 && whole < 145,
                  "vibrato's average pitch was %ld Hz, expected about 129",
                  whole);
        }

        /* Rows 5 and 6, tone portamento (3 20): a low C-1 (period 856,
           about 64 Hz), then a row that slides towards 428 at 32 a tick --
           5 ticks x 32 is 160, so it gets to 696 rather than arriving. */
        {
            long low = hz_in(buf, 5 * ROW_FRAMES, ROW_FRAMES);
            long slid = hz_in(buf, 6 * ROW_FRAMES + ROW_FRAMES / 2,
                              ROW_FRAMES / 2);

            CHECK(low > 55 && low < 75,
                  "the low note measured %ld Hz, expected about %ld",
                  low, hz_of_period(856));
            CHECK(slid > low + 5,
                  "tone portamento did not slide up from %ld Hz: %ld Hz",
                  low, slid);
            CHECK(slid < 110,
                  "tone portamento overshot to %ld Hz; it should not reach "
                  "%ld Hz in one row", slid, hz_of_period(428));
        }

        /* Row 7, tremolo (7 84): the volume moves, the pitch does not. */
        {
            long at = 7 * ROW_FRAMES;
            long tick = ROW_FRAMES / 6;
            long pitch = hz_in(buf, at, ROW_FRAMES);
            int loudest = 0, quietest = 127;
            int t;

            for (t = 0; t < 6; t++) {
                long i;
                int peak = 0;

                for (i = 0; i < tick; i++) {
                    int v = buf[at + t * tick + i];

                    if (v < 0) v = -v;
                    if (v > peak) peak = v;
                }
                if (peak > loudest) loudest = peak;
                if (peak < quietest) quietest = peak;
            }

            CHECK(loudest > quietest + 2,
                  "tremolo did not change the volume: peaks %d..%d",
                  quietest, loudest);
            CHECK(pitch > 115 && pitch < 145,
                  "tremolo moved the pitch to %ld Hz", pitch);
        }

        /* Row 8: a plain note again. The bends must have been undone --
           if the vibrato or arpeggio had been left in the base period,
           this row would not be the note it asks for. */
        {
            long back = hz_in(buf, 8 * ROW_FRAMES, ROW_FRAMES);

            CHECK(back > 120 && back < 140,
                  "after the effects the plain note measured %ld Hz, "
                  "expected about 129 -- a bend was left in the period",
                  back);
        }
    }

    free(buf);
    mod_free(&m);
}

/* Render a module to raw signed 8-bit PCM.
 *
 * Not a test in itself: it is the oracle for the end-to-end one. The
 * client's WAV should contain exactly these bytes, which is the difference
 * between "the client wrote a file" and "the client played the tune". */
static int render(const char *path, unsigned long rate, long frames,
                  const char *out)
{
    mod_player  m;
    FILE       *f;
    signed char buf[512];
    long        left = frames;

    if (mod_load(&m, path, rate) != 0) {
        fprintf(stderr, "%s: %s\n", path, mod_error(&m));
        return 1;
    }

    f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", out);
        mod_free(&m);
        return 1;
    }

    /* In the same slice size the client uses, because the mixer's row and
       tick boundaries have to fall in the same places for the bytes to
       match. */
    while (left > 0) {
        long want = left > (long)sizeof buf ? (long)sizeof buf : left;
        long got = mod_mix(&m, buf, want);

        if (got <= 0)
            break;
        fwrite(buf, 1, (size_t)got, f);
        left -= got;
    }

    fclose(f);
    mod_free(&m);
    return 0;
}

int main(int argc, char **argv)
{
    const char *art = (argc > 1) ? argv[1] : "art";

    if (argc > 1 && strcmp(argv[1], "--render") == 0) {
        if (argc < 6) {
            fprintf(stderr,
                    "usage: test_mod --render MOD RATE FRAMES OUT.raw\n");
            return 2;
        }
        return render(argv[2], strtoul(argv[3], NULL, 10),
                      strtol(argv[4], NULL, 10), argv[5]);
    }

    printf("DOSy++ module tests (loader / mixer / pitch / timing)\n");

    snprintf(g_test_mod, sizeof g_test_mod, "%s/TESTTONE.MOD", art);
    snprintf(g_tune_mod, sizeof g_tune_mod, "%s/DOSYPP.MOD", art);
    snprintf(g_fx_mod, sizeof g_fx_mod, "%s/FXTEST.MOD", art);

    {
        FILE *f = fopen(g_test_mod, "rb");

        if (!f) {
            /* Not a skip: a test that passes by testing nothing is how a
               broken mixer ships. `make test` writes the modules first. */
            printf("FAILED: %s is missing -- run: make modules\n",
                   g_test_mod);
            return 1;
        }
        fclose(f);
    }

    test_pitch();
    test_timing();
    test_ends();
    test_tune();
    test_effects();
    test_hostile();

    if (g_fail)
        printf("FAILED: %d of %d checks\n", g_fail, g_run);
    else
        printf("ok: %d checks, 0 failed\n", g_run);
    return g_fail ? 1 : 0;
}
