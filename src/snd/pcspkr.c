/* pcspkr.c - PIT channel 2 through the speaker gate, or nothing. */
#include "pcspkr.h"

#include <string.h>

/* A sound is a list of (frequency, duration) steps. Frequency 0 is a rest.
   Eight steps is enough for anything that should interrupt a BBS user. */
#define MAX_STEPS 8

typedef struct {
    unsigned short hz;
    unsigned short ms;
} step;

static struct {
    step          steps[MAX_STEPS];
    int           count;
    int           at;
    unsigned long next_change;   /* ms timestamp */
    int           playing;
    int           enabled;
} g;

static void tone_on(unsigned short hz);
static void tone_off(void);

static void play(const step *s, int n)
{
    if (!g.enabled)
        return;
    if (n > MAX_STEPS)
        n = MAX_STEPS;

    memcpy(g.steps, s, (size_t)n * sizeof *s);
    g.count = n;
    g.at = -1;                 /* snd_tick starts the first step */
    g.next_change = 0;
    g.playing = 1;
}

void snd_tick(unsigned long now_ms)
{
    if (!g.playing)
        return;

    if (g.at >= 0 && now_ms < g.next_change)
        return;

    g.at++;
    if (g.at >= g.count) {
        tone_off();
        g.playing = 0;
        return;
    }

    if (g.steps[g.at].hz)
        tone_on(g.steps[g.at].hz);
    else
        tone_off();

    g.next_change = now_ms + g.steps[g.at].ms;
}

void snd_init(int enabled)
{
    memset(&g, 0, sizeof g);
    g.enabled = enabled ? 1 : 0;
}

void snd_shutdown(void)
{
    /* Leaving the gate open means a tone that never stops, which outlives
       the program. */
    tone_off();
    g.playing = 0;
}

int snd_enabled(void)
{
    return g.enabled;
}

/* ------------------------------------------------------- the sounds */

void snd_connect(void)
{
    static const step s[] = { {880, 70}, {1175, 70}, {1568, 110} };
    play(s, 3);
}

void snd_disconnect(void)
{
    /* A carrier drop: three descending tones, which is roughly what a modem
       losing the line sounded like through a speaker. */
    static const step s[] = { {1568, 80}, {1047, 80}, {700, 80}, {440, 160} };
    play(s, 4);
}

void snd_message(void)
{
    static const step s[] = { {1319, 60}, {0, 40}, {1319, 60} };
    play(s, 3);
}

void snd_error(void)
{
    static const step s[] = { {220, 160} };
    play(s, 1);
}

void snd_keypress_bad(void)
{
    static const step s[] = { {120, 25} };
    play(s, 1);
}

/* ----------------------------------------------------------- backends */

#ifdef DOSYPP_WATT32

#include <dos.h>
#include <pc.h>
#include <time.h>

/* The PIT runs at 1193182 Hz. Channel 2 feeds the speaker; port 0x61 bits
   0 and 1 gate it through. */
#define PIT_HZ 1193182UL

static void tone_on(unsigned short hz)
{
    unsigned short div;

    if (hz < 20)
        hz = 20;                      /* below this the divisor overflows */
    div = (unsigned short)(PIT_HZ / hz);

    /* Channel 2, mode 3 (square wave), write both divisor bytes. */
    outportb(0x43, 0xB6);
    outportb(0x42, (unsigned char)(div & 0xFF));
    outportb(0x42, (unsigned char)(div >> 8));

    /* Open the gate without disturbing the other bits of 0x61 -- bit 2 is
       the parity check enable on some machines. */
    outportb(0x61, (unsigned char)(inportb(0x61) | 0x03));
}

static void tone_off(void)
{
    outportb(0x61, (unsigned char)(inportb(0x61) & ~0x03));
}

unsigned long snd_millis(void)
{
    /* uclock() is DJGPP's high-resolution counter, driven from the PIT.
       clock() would do, but its 18.2Hz granularity is coarser than the
       shortest note here. */
    return (unsigned long)(uclock() / (UCLOCKS_PER_SEC / 1000));
}

#else

#include <sys/time.h>

static void tone_on(unsigned short hz) { (void)hz; }
static void tone_off(void) { }

unsigned long snd_millis(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (unsigned long)tv.tv_sec * 1000UL +
           (unsigned long)(tv.tv_usec / 1000);
}

#endif
