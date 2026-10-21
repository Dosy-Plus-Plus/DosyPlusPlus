/* mus.c - the music glue. See mus.h. */
#include "mus.h"

#include <stdio.h>
#include <string.h>

#include "mod.h"
#include "sndout.h"

static struct {
    int           playing;
    int           loop;
    mod_player    mod;
    unsigned long loops;
    unsigned long fed;          /* frames handed to the device */
    char          caveats[160];
    char          err[192];
} mus;

const char *mus_error(void)
{
    return mus.err[0] ? mus.err : "no error";
}

int mus_playing(void)
{
    return mus.playing;
}

const char *mus_title(void)
{
    return mus.playing ? mus.mod.name : "";
}

const char *mus_device(void)
{
    return mus.playing ? sndout_describe() : "silent";
}

const char *mus_caveats(void)
{
    return mus.caveats;
}

unsigned long mus_fed(void)
{
    return mus.fed;
}

unsigned long mus_frames(void)
{
    return mus.playing ? sndout_played() : 0;
}

unsigned long mus_loops(void)
{
    return mus.loops;
}

void mus_set_loop(int on)
{
    mus.loop = on ? 1 : 0;
}

int mus_start(const char *path, unsigned long rate, const char *wav_path)
{
    mus_stop();

    mus.err[0] = '\0';
    mus.caveats[0] = '\0';
    mus.loops = 0;
    mus.fed = 0;
    mus.loop = 1;

    if (!path || !path[0]) {
        snprintf(mus.err, sizeof mus.err, "no module to play");
        return -1;
    }

    if (mod_load(&mus.mod, path, rate) != 0) {
        snprintf(mus.err, sizeof mus.err, "%s: %s", path,
                 mod_error(&mus.mod));
        return -1;
    }

    /* The card comes second: loading first means a missing module is
       reported as a missing module rather than as a sound problem. */
    if (sndout_open(rate, wav_path) != 0) {
        snprintf(mus.err, sizeof mus.err, "%s", sndout_error());
        mod_free(&mus.mod);
        return -1;
    }

    /* Said once, at the start, rather than discovered by ear: a tune using
       vibrato will play its notes without it. */
    mod_unsupported(&mus.mod, mus.caveats, sizeof mus.caveats);

    mus.playing = 1;

    /* Prime the ring before the first main-loop pass gets here. The card
       starts reading the moment it is programmed, so without this it plays
       the ring's initial silence -- up to a second and a half of nothing --
       while the client finishes logging in. Measured: the DOS rig showed
       played climbing past 7,000 frames with fed still at zero. */
    mus_tick();

    return 0;
}

void mus_stop(void)
{
    if (!mus.playing)
        return;

    sndout_close();
    mod_free(&mus.mod);
    mus.playing = 0;
    mus.caveats[0] = '\0';
}

void mus_tick(void)
{
    signed char buf[MUS_SLICE];
    int slices;

    if (!mus.playing)
        return;

    /* As much as the device will take, in slices, up to MUS_MAX_SLICES.
       See mus.h for why it is not one: a pass of this loop can be 126ms
       long, and one slice is 46ms of audio. */
    for (slices = 0; slices < MUS_MAX_SLICES; slices++) {
        long space = sndout_space();
        long got;

        if (space <= 0)
            return;                 /* the card is still chewing */
        if (space > MUS_SLICE)
            space = MUS_SLICE;

        got = mod_mix(&mus.mod, buf, space);

        if (got < space) {
            /* The module ended. Round again, or pad with silence and
               stop: either way the device is handed a full slice so the
               ring does not run dry mid-phrase. */
            if (mus.loop) {
                mod_rewind(&mus.mod);
                mus.loops++;
                got += mod_mix(&mus.mod, buf + got, space - got);
            }

            if (got < space) {
                memset(buf + got, 0, (size_t)(space - got));
                got = space;

                if (!mus.loop) {
                    mus.fed += (unsigned long)sndout_write(buf, got);
                    mus_stop();
                    return;
                }
            }
        }

        {
            long took = sndout_write(buf, got);

            mus.fed += (unsigned long)(took > 0 ? took : 0);
            if (took < got)
                return;             /* it took less than offered: full */
        }
    }
}
