/* musbench.c - what does the music cost?
 *
 * The client mixes four channels from its main loop, in the same pass that
 * hashes a share and polls a hub. Nobody had measured what that costs, and
 * the answer might well have been "turn the music off while downloading".
 * This measures it.
 *
 * Four phases, in the order the question gets asked:
 *
 *   1. The mixer flat out: frames a second, and what one slice costs. At
 *      11025 Hz the ratio is the fraction of the CPU the music wants. Run
 *      for each module named on the command line, because a tune using
 *      vibrato and arpeggio costs more per tick than one that does not,
 *      and the honest number is the one for the module you ship.
 *   2. Hashing on its own, the share workflow's number.
 *   3. Hashing with one HASHQ_BUDGET and one MUS_SLICE per pass -- what
 *      mus_tick() used to do. It is kept because it is the measurement
 *      that changed the code: a slice per pass is a fixed supply against a
 *      demand that depends on how long the hashing took, and on a 486 the
 *      hashing takes 126ms while a slice is 46ms of audio. It falls behind
 *      by about two thirds, which is a stutter for as long as a share is
 *      being hashed.
 *   4. Hashing with the audio mixed until it has caught up with the clock,
 *      which is what mus_tick() does now (bounded by MUS_MAX_SLICES). The
 *      interesting numbers are what it costs the hashing and how much
 *      mixing lands in the worst single pass.
 *
 * Integer arithmetic only: the project is built -msoft-float, so a
 * printf("%f") here would not even link.
 *
 * Builds for host and DOS. On DOS, run it under the cycle-limited profile
 * (make dos-bench-run); at cycles=max the answer flatters us and decides
 * nothing.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../../src/hash/tth.h"
#include "../../src/share/hashq.h"      /* HASHQ_BUDGET */
#include "../../src/snd/mod.h"
#include "../../src/snd/mus.h"          /* MUS_SLICE */

#define RATE       11025UL
#define MIN_TICKS  (CLOCKS_PER_SEC * 2) /* measure for at least ~2s */
#define HASH_BUF   HASHQ_BUDGET         /* one pass of the real loop */

static unsigned char hbuf[HASH_BUF];
static signed char   mbuf[MUS_SLICE];
static mod_player    player;

static unsigned long ms_of(clock_t t)
{
    unsigned long ms = (unsigned long)t * 1000UL / (unsigned long)CLOCKS_PER_SEC;

    return ms ? ms : 1;
}

/* Frames the card would have eaten by `ms` into the run. 64-bit because
   ms * RATE passes 2^32 after about four minutes, and a benchmark that
   silently wraps is worse than no benchmark. */
static unsigned long frames_due(unsigned long ms)
{
    return (unsigned long)(((unsigned long long)ms * RATE) / 1000ULL);
}

/* Phase 1 for one module: mix flat out and report. Returns frames a
   second, or 0 if the module would not load. */
static unsigned long mix_rate_of(const char *path, mod_player *out)
{
    clock_t       start, elapsed;
    unsigned long frames = 0, ms, fps;
    signed char   buf[MUS_SLICE];

    if (mod_load(out, path, RATE) != 0) {
        printf("   %s: %s\n", path, mod_error(out));
        return 0;
    }

    start = clock();
    do {
        long got = mod_mix(out, buf, (long)MUS_SLICE);

        if (got < (long)MUS_SLICE)
            mod_rewind(out);        /* the tune ended; round again */
        frames += (unsigned long)(got > 0 ? got : 0);
        elapsed = clock() - start;
    } while (elapsed < MIN_TICKS);

    ms = ms_of(elapsed);
    fps = frames * 1000UL / ms;

    printf("   %-14s %7lu frames/s", path, fps);
    if (fps > 0) {
        unsigned long permille = RATE * 1000UL / fps;

        printf("  slice %4lu us  %lu.%lu%% of the CPU",
               (unsigned long)MUS_SLICE * 1000000UL / fps,
               permille / 10UL, permille % 10UL);
    }
    {
        char why[96];

        mod_unsupported(out, why, sizeof why);
        if (why[0])
            printf("  [ignores %s]", why);
    }
    printf("\n");
    return fps;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "DOSYPP.MOD";
    clock_t     start, elapsed;
    unsigned long mix_fps = 0, hash_kbps = 0;
    unsigned long i;

    for (i = 0; i < sizeof hbuf; i++)
        hbuf[i] = (unsigned char)((i * 37 + 7) % 256);

    printf("DOSy++ music/hash benchmark\n");
    printf("%d-frame slices, %lu KB hash passes\n",
           (int)MUS_SLICE, (unsigned long)HASH_BUF / 1024UL);


    /* ---- 1. the mixer, flat out, for each module given -------------- */
    printf("\n1. mixer alone, at %lu Hz\n", RATE);
    {
        int a;

        mix_fps = mix_rate_of(path, &player);
        for (a = 2; a < argc; a++) {
            mod_player other;
            unsigned long fps = mix_rate_of(argv[a], &other);

            if (fps)
                mod_free(&other);
        }
    }
    fflush(stdout);

    /* ---- 2. hashing, on its own ------------------------------------- */
    {
        unsigned long bytes = 0, ms;
        tth_ctx ctx;

        tth_init(&ctx, NULL, NULL, -1);

        start = clock();
        do {
            tth_update(&ctx, hbuf, sizeof hbuf);
            bytes += (unsigned long)sizeof hbuf;
            elapsed = clock() - start;
        } while (elapsed < MIN_TICKS);

        ms = ms_of(elapsed);
        hash_kbps = (bytes / 1024UL) * 1000UL / ms;

        printf("\n2. hashing alone\n");
        printf("   hashed      : %lu KB in %lu ms\n", bytes / 1024UL, ms);
        printf("   throughput  : %lu KB/s\n", hash_kbps);
        if (hash_kbps > 0)
            printf("   1 GB share  : %lu min\n",
                   (1024UL * 1024UL / hash_kbps) / 60UL);
        fflush(stdout);
    }

    /* ---- 3. both, as the shipped loop does it ------------------------ */
    {
        unsigned long bytes = 0, frames = 0, passes = 0, ms, due, kbps;
        tth_ctx ctx;

        tth_init(&ctx, NULL, NULL, -1);
        mod_rewind(&player);

        start = clock();
        do {
            long got;

            tth_update(&ctx, hbuf, sizeof hbuf);
            bytes += (unsigned long)sizeof hbuf;

            got = mod_mix(&player, mbuf, (long)MUS_SLICE);
            if (got < (long)MUS_SLICE)
                mod_rewind(&player);
            frames += (unsigned long)(got > 0 ? got : 0);

            passes++;
            elapsed = clock() - start;
        } while (elapsed < MIN_TICKS);

        ms = ms_of(elapsed);
        kbps = (bytes / 1024UL) * 1000UL / ms;
        due = frames_due(ms);

        printf("\n3. hashing, one slice per pass (the policy that failed)\n");
        printf("   passes      : %lu (%lu ms each)\n", passes, ms / passes);
        printf("   hashed      : %lu KB/s", kbps);
        if (hash_kbps > 0)
            printf("  (%lu%% of hashing alone)", kbps * 100UL / hash_kbps);
        printf("\n");
        printf("   audio made  : %lu frames\n", frames);
        printf("   audio needed: %lu frames\n", due);
        if (frames >= due)
            printf("   verdict     : keeps up, %lu frames to spare\n",
                   frames - due);
        else
            printf("   verdict     : FALLS BEHIND by %lu frames (%lu ms of "
                   "silence per second)\n",
                   due - frames,
                   ((due - frames) * 1000UL / RATE) * 1000UL / ms);
        fflush(stdout);
    }

    /* ---- 4. both, mixing until the audio has caught up --------------- */
    {
        unsigned long bytes = 0, frames = 0, passes = 0, ms, due, kbps;
        unsigned long worst_slices = 0;
        tth_ctx ctx;

        tth_init(&ctx, NULL, NULL, -1);
        mod_rewind(&player);

        start = clock();
        do {
            unsigned long want, slices = 0;

            tth_update(&ctx, hbuf, sizeof hbuf);
            bytes += (unsigned long)sizeof hbuf;

            /* As much audio as the clock says is owed, in the same slice
               size -- which is what a mus_tick() that filled the device's
               free space would end up doing. */
            want = frames_due(ms_of(clock() - start));
            while (frames < want && slices < MUS_MAX_SLICES) {
                long got = mod_mix(&player, mbuf, (long)MUS_SLICE);

                if (got < (long)MUS_SLICE)
                    mod_rewind(&player);
                frames += (unsigned long)(got > 0 ? got : 0);
                slices++;
            }
            if (slices > worst_slices)
                worst_slices = slices;

            passes++;
            elapsed = clock() - start;
        } while (elapsed < MIN_TICKS);

        ms = ms_of(elapsed);
        kbps = (bytes / 1024UL) * 1000UL / ms;
        due = frames_due(ms);

        printf("\n4. hashing, mixing until caught up (what ships)\n");
        printf("   passes      : %lu (%lu ms each)\n", passes, ms / passes);
        printf("   hashed      : %lu KB/s", kbps);
        if (hash_kbps > 0)
            printf("  (%lu%% of hashing alone)", kbps * 100UL / hash_kbps);
        printf("\n");
        printf("   audio made  : %lu frames, needed %lu\n", frames, due);
        printf("   worst pass  : %lu slices", worst_slices);
        if (mix_fps > 0)
            printf(" (%lu ms of mixing in one pass)",
                   worst_slices * (unsigned long)MUS_SLICE * 1000UL
                   / mix_fps);
        printf("\n");
        fflush(stdout);
    }

    mod_free(&player);
    printf("\nDONE\n");
    return 0;
}
