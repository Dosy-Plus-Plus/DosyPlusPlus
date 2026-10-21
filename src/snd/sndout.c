/* sndout.c - the Sound Blaster and the WAV file. See sndout.h. */
#include "sndout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pcspkr.h"             /* snd_millis: one clock for the program */
#include "../hash/tiger.h"      /* tiger_u64, for the pacing arithmetic */

static char g_err[160];
static char g_desc[80];

const char *sndout_error(void)
{
    return g_err[0] ? g_err : "no error";
}

const char *sndout_describe(void)
{
    return g_desc[0] ? g_desc : "nothing";
}

#ifdef DOSYPP_WATT32

/* ------------------------------------------------------------------ DOS
 *
 * The card is programmed once for auto-init DMA over a ring in
 * conventional memory and then left alone. Refilling is driven by the DMA
 * controller's current-count register: it says how far through the ring the
 * card has read, which is all the information a producer needs.
 *
 * Everything here is ordinary port I/O. The sequences are the documented
 * ones from the Sound Blaster programming guide, and the comments say what
 * each is for, because a bare sequence of outportb calls is unreadable and
 * unverifiable six months later.
 */
#include <dos.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>
#include <pc.h>

#define DSP_RESET(base)      ((base) + 0x6)
#define DSP_READ(base)       ((base) + 0xA)
#define DSP_WRITE(base)      ((base) + 0xC)
#define DSP_WRITE_STATUS(base) ((base) + 0xC)
#define DSP_READ_STATUS(base)  ((base) + 0xE)

/* 8-bit DMA channels live on the first controller; the registers differ per
   channel, hence the tables rather than arithmetic. */
static const unsigned char DMA_PAGE[4]  = { 0x87, 0x83, 0x81, 0x82 };
static const unsigned char DMA_ADDR[4]  = { 0x00, 0x02, 0x04, 0x06 };
static const unsigned char DMA_COUNT[4] = { 0x01, 0x03, 0x05, 0x07 };

static struct {
    int            open;
    unsigned       base;        /* 0x220, 0x240, ... */
    int            irq;
    int            dma;
    unsigned long  rate;

    int            dos_seg;     /* conventional-memory segment */
    int            dos_sel;
    unsigned long  phys;        /* physical address of the ring */
    long           ring_size;

    long           write_pos;   /* where we fill next */
    unsigned long  played;      /* frames the card has consumed */
    unsigned long  written;     /* frames handed to the ring */
    long           last_remain; /* DMA count at the previous poll */

    /* When the DMA controller will not say where it is.
     *
     * This design polls the 8237's current-count register, because a
     * measurement beats a promise. Some controllers do not implement that
     * read: v86's does not -- it moves bytes for the sound card without
     * exposing a decrementing count -- so the count sits at its initial
     * value, this code concludes the card has played nothing, and refuses
     * to write any more. The symptom is silence with everything else
     * apparently fine, which is how the browser demo had no music.
     *
     * So: if the count has not moved for a quarter of a second, while a
     * card playing at 11kHz would have moved it by nearly three thousand,
     * fall back to the clock. A card consumes at the sampling rate and not
     * one frame faster, which is enough to keep the ring fed. It is an
     * estimate and the describe() string says so. */
    int            timed;       /* using the clock, not the count */
    unsigned long  timed_from;  /* ms when that started */
    unsigned long  timed_base;  /* frames played at that moment */
    unsigned long  stuck_since; /* ms the count stopped moving */
} sb;

static void dsp_write(unsigned char val)
{
    int spins = 0;

    /* Bit 7 of the write-status port is "busy". A card that never clears
       it is a card that is not there, and spinning forever on it would
       hang the client rather than disable the music. */
    while ((inportb(DSP_WRITE_STATUS(sb.base)) & 0x80) && ++spins < 10000)
        ;
    outportb(DSP_WRITE(sb.base), val);
}

static int dsp_read(unsigned char *out)
{
    int spins = 0;

    while (!(inportb(DSP_READ_STATUS(sb.base)) & 0x80) && ++spins < 10000)
        ;
    if (spins >= 10000)
        return -1;
    *out = inportb(DSP_READ(sb.base));
    return 0;
}

/* The documented reset: 1 into the reset port, a short wait, 0, then the
   card answers 0xAA on the read port. */
static int dsp_reset(void)
{
    unsigned char got;
    int i;

    outportb(DSP_RESET(sb.base), 1);
    for (i = 0; i < 100; i++)
        (void)inportb(DSP_READ_STATUS(sb.base));   /* ~3us each, enough */
    outportb(DSP_RESET(sb.base), 0);

    for (i = 0; i < 200; i++) {
        if (inportb(DSP_READ_STATUS(sb.base)) & 0x80) {
            if (dsp_read(&got) == 0 && got == 0xAA)
                return 0;
        }
    }
    return -1;
}

/* BLASTER=A220 I5 D1 H5 T6. Set by the card's own driver; a machine
   without it has no card configured, whatever hardware is in it. */
static int parse_blaster(void)
{
    const char *env = getenv("BLASTER");

    sb.base = 0;
    sb.irq = -1;
    sb.dma = -1;

    if (!env) {
        snprintf(g_err, sizeof g_err,
                 "BLASTER is not set, so there is no sound card to find");
        return -1;
    }

    while (*env) {
        char key = *env++;
        unsigned long v = 0;
        int hex = (key == 'A' || key == 'a');

        while (*env == ' ')
            env++;

        while ((*env >= '0' && *env <= '9') ||
               (hex && ((*env >= 'a' && *env <= 'f') ||
                        (*env >= 'A' && *env <= 'F')))) {
            int d = (*env <= '9') ? *env - '0'
                                  : (*env | 0x20) - 'a' + 10;

            v = v * (hex ? 16 : 10) + (unsigned long)d;
            env++;
        }

        switch (key) {
        case 'A': case 'a': sb.base = (unsigned)v; break;
        case 'I': case 'i': sb.irq = (int)v; break;
        case 'D': case 'd': sb.dma = (int)v; break;
        default: break;             /* H and T are for 16-bit and type */
        }

        while (*env == ' ')
            env++;
    }

    if (sb.base == 0) {
        snprintf(g_err, sizeof g_err, "BLASTER has no base address in it");
        return -1;
    }
    if (sb.dma < 0 || sb.dma > 3) {
        snprintf(g_err, sizeof g_err,
                 "BLASTER names DMA %d, which is not an 8-bit channel",
                 sb.dma);
        return -1;
    }
    return 0;
}

/* Conventional memory that the DMA controller can reach, not crossing a
   64KB boundary. Twice the ring is allocated and the aligned half used:
   asking the DPMI host for alignment is not something it can promise. */
static int alloc_ring(void)
{
    int seg;
    unsigned long phys;
    unsigned long want = (unsigned long)SNDOUT_RING;

    seg = __dpmi_allocate_dos_memory((int)((want * 2) / 16), &sb.dos_sel);
    if (seg < 0) {
        snprintf(g_err, sizeof g_err,
                 "no conventional memory for a %lu byte sound buffer",
                 want);
        return -1;
    }
    sb.dos_seg = seg;

    phys = (unsigned long)seg * 16UL;

    /* Move up to the next 64KB boundary if the buffer would straddle
       one. There is room because twice the size was taken. */
    if ((phys & 0xFFFF0000UL) != ((phys + want - 1) & 0xFFFF0000UL))
        phys = (phys + 0xFFFFUL) & 0xFFFF0000UL;

    sb.phys = phys;
    sb.ring_size = (long)want;
    return 0;
}

static void ring_clear(void)
{
    long i;

    /* Silence, which for signed 8-bit is zero. Written through the DOS
       memory selector rather than a pointer: there is no flat mapping of
       conventional memory in protected mode. */
    for (i = 0; i < sb.ring_size; i++)
        _farpokeb(_dos_ds, sb.phys + (unsigned long)i, 0);
}

static void program_dma(void)
{
    unsigned char ch = (unsigned char)sb.dma;
    unsigned long offset = sb.phys & 0xFFFFUL;
    unsigned long count = (unsigned long)sb.ring_size - 1;

    /* Mask the channel while it is reprogrammed, or the controller will
       act on a half-written address. */
    outportb(0x0A, (unsigned char)(0x04 | ch));

    outportb(0x0C, 0);                      /* clear the flip-flop */

    /* Single-mode, auto-init, read (card reads memory), address increment:
       0x58 | channel. Auto-init is what makes this a ring rather than a
       series of blocks with gaps between them. */
    outportb(0x0B, (unsigned char)(0x58 | ch));

    outportb(DMA_ADDR[ch], (unsigned char)(offset & 0xFF));
    outportb(DMA_ADDR[ch], (unsigned char)((offset >> 8) & 0xFF));

    outportb(DMA_COUNT[ch], (unsigned char)(count & 0xFF));
    outportb(DMA_COUNT[ch], (unsigned char)((count >> 8) & 0xFF));

    outportb(DMA_PAGE[ch], (unsigned char)((sb.phys >> 16) & 0xFF));

    outportb(0x0A, ch);                     /* unmask */
}

static void program_dsp(void)
{
    unsigned long tc;

    dsp_write(0xD1);                        /* speaker on */

    /* The time constant is 256 - 1000000/rate, which is how a card before
       the SB16 was told its sampling rate. */
    tc = 256UL - (1000000UL / sb.rate);
    dsp_write(0x40);
    dsp_write((unsigned char)(tc & 0xFF));

    /* Block size for auto-init, as length-1. The card will raise its IRQ
       every block; nothing listens, which is fine -- the IRQ is not
       enabled at the PIC and the transfer continues regardless. */
    {
        unsigned long block = (unsigned long)sb.ring_size - 1;

        dsp_write(0x48);
        dsp_write((unsigned char)(block & 0xFF));
        dsp_write((unsigned char)((block >> 8) & 0xFF));
    }

    dsp_write(0x1C);                        /* 8-bit auto-init DMA output */
}

/* How many bytes the controller still has to read in this pass. */
static long dma_remaining(void)
{
    unsigned char ch = (unsigned char)sb.dma;
    unsigned lo, hi;

    outportb(0x0C, 0);                      /* latch a consistent pair */
    lo = inportb(DMA_COUNT[ch]);
    hi = inportb(DMA_COUNT[ch]);

    return (long)(((unsigned)hi << 8) | lo);
}

static long dma_play_pos(void)
{
    long remain = dma_remaining();
    long pos = sb.ring_size - 1 - remain;

    if (pos < 0)
        pos = 0;
    if (pos >= sb.ring_size)
        pos = sb.ring_size - 1;
    return pos;
}

int sndout_open(unsigned long rate, const char *path)
{
    (void)path;

    g_err[0] = '\0';
    memset(&sb, 0, sizeof sb);
    sb.dos_seg = -1;

    if (rate < 4000 || rate > 23000) {
        snprintf(g_err, sizeof g_err,
                 "%lu Hz is outside what an 8-bit card will take", rate);
        return -1;
    }
    sb.rate = rate;

    if (parse_blaster() != 0)
        return -1;

    if (dsp_reset() != 0) {
        snprintf(g_err, sizeof g_err,
                 "no card answered at %03Xh", sb.base);
        return -1;
    }

    if (alloc_ring() != 0)
        return -1;

    ring_clear();
    program_dma();
    program_dsp();

    sb.open = 1;
    sb.write_pos = 0;
    sb.played = 0;
    sb.last_remain = dma_remaining();

    snprintf(g_desc, sizeof g_desc,
             "Sound Blaster at %03Xh, IRQ %d, DMA %d, %lu Hz",
             sb.base, sb.irq, sb.dma, sb.rate);
    return 0;
}

void sndout_close(void)
{
    if (!sb.open)
        return;

    dsp_write(0xD0);                        /* pause the transfer */
    dsp_write(0xDA);                        /* leave auto-init */
    dsp_write(0xD3);                        /* speaker off */

    outportb(0x0A, (unsigned char)(0x04 | (unsigned char)sb.dma));

    if (sb.dos_seg >= 0)
        __dpmi_free_dos_memory(sb.dos_sel);

    sb.open = 0;
    g_desc[0] = '\0';
}

/* The played counter has to survive the DMA count wrapping round the ring,
   which it does every ring_size bytes with nothing to announce it. */
/* Frames the card has taken, by the clock rather than the counter. */
static unsigned long timed_played(void)
{
    unsigned long ms = snd_millis() - sb.timed_from;

    return sb.timed_base +
           (unsigned long)(((tiger_u64)ms * sb.rate) / 1000UL);
}

static void account(void)
{
    long remain = dma_remaining();
    long used;

    /* Acknowledge the card's end-of-block interrupt by reading the
       read-buffer status port. Nothing is listening for the IRQ -- there is
       no handler -- but the DSP latches it, and a latched interrupt that is
       never acknowledged is where playback stops: DOSBox-X ran exactly two
       blocks and then sat there, which is what sent this code looking. The
       read is the documented acknowledge and costs one port access per
       poll. */
    (void)inportb(DSP_READ_STATUS(sb.base));

    if (remain <= sb.last_remain)
        used = sb.last_remain - remain;
    else
        used = sb.last_remain + (sb.ring_size - remain);

    /* Does the count move at all? A card playing 11,025 frames a second
       moves it by 2,756 in a quarter of a second, so standing still for
       that long means nobody is updating it. */
    if (!sb.timed) {
        if (remain == sb.last_remain) {
            unsigned long now = snd_millis();

            if (sb.stuck_since == 0)
                sb.stuck_since = now;
            else if (now - sb.stuck_since > 250) {
                sb.timed = 1;
                sb.timed_from = now;
                sb.timed_base = sb.played;
                snprintf(g_desc, sizeof g_desc,
                         "Sound Blaster at %03Xh, IRQ %d, DMA %d, %lu Hz "
                         "(timed: no DMA count)",
                         sb.base, sb.irq, sb.dma, sb.rate);
            }
        } else {
            sb.stuck_since = 0;
        }
    }

    sb.played += (unsigned long)used;
    sb.last_remain = remain;
}

long sndout_space(void)
{
    long play;
    long free_bytes;

    if (!sb.open)
        return 0;

    account();

    /* By the clock, when the counter is no use: how much of what has been
       written the card should have eaten by now, and therefore how much
       room there is. Same shape as the host's WAV backend, for the same
       reason. */
    if (sb.timed) {
        unsigned long eaten = timed_played();
        long outstanding;

        if (eaten > sb.written)
            eaten = sb.written;         /* it cannot be ahead of us */

        outstanding = (long)(sb.written - eaten);
        free_bytes = sb.ring_size - outstanding - (sb.ring_size / 32);
        return (free_bytes > 0) ? free_bytes : 0;
    }

    play = dma_play_pos();

    /* Keep a gap ahead of the card: writing right up to where it is
       reading means a slow refill is heard as a click. A thirty-second of
       the ring is about 45ms at 11kHz. */
    free_bytes = play - sb.write_pos;
    if (free_bytes < 0)
        free_bytes += sb.ring_size;
    free_bytes -= sb.ring_size / 32;

    return (free_bytes > 0) ? free_bytes : 0;
}

long sndout_write(const signed char *buf, long frames)
{
    long space = sndout_space();
    long done = 0;

    if (!sb.open || frames <= 0)
        return 0;
    if (frames > space)
        frames = space;

    while (done < frames) {
        long chunk = sb.ring_size - sb.write_pos;
        long i;

        if (chunk > frames - done)
            chunk = frames - done;

        for (i = 0; i < chunk; i++)
            _farpokeb(_dos_ds,
                      sb.phys + (unsigned long)(sb.write_pos + i),
                      (unsigned char)buf[done + i]);

        sb.write_pos += chunk;
        if (sb.write_pos >= sb.ring_size)
            sb.write_pos = 0;
        done += chunk;
    }

    sb.written += (unsigned long)done;
    return done;
}

unsigned long sndout_played(void)
{
    if (!sb.open)
        return 0;

    account();

    if (sb.timed) {
        unsigned long eaten = timed_played();

        return (eaten > sb.written) ? sb.written : eaten;
    }
    return sb.played;
}

#else

/* ----------------------------------------------------------------- host
 *
 * A WAV file. The point is not playback -- it is that the mixer can be
 * tested, and that a build can produce something a person can listen to
 * without a DOS machine.
 */
static struct {
    FILE          *fp;
    unsigned long  rate;
    unsigned long  frames;
    unsigned long  started_ms;  /* when the device opened */
} wav;

static void put_u32(FILE *f, unsigned long v)
{
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f);
    fputc((int)((v >> 24) & 0xFF), f);
}

static void put_u16(FILE *f, unsigned v)
{
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
}

int sndout_open(unsigned long rate, const char *path)
{
    g_err[0] = '\0';
    memset(&wav, 0, sizeof wav);

    if (!path || !path[0])
        path = "DOSYPP.WAV";

    wav.fp = fopen(path, "wb");
    if (!wav.fp) {
        snprintf(g_err, sizeof g_err, "cannot write %s", path);
        return -1;
    }
    wav.rate = rate ? rate : 11025;
    wav.started_ms = snd_millis();

    /* The header is written now with zero lengths and corrected on close:
       the alternative is holding the whole render in memory to count it,
       which is the one thing this program does not do. */
    fwrite("RIFF", 1, 4, wav.fp);
    put_u32(wav.fp, 0);
    fwrite("WAVE", 1, 4, wav.fp);
    fwrite("fmt ", 1, 4, wav.fp);
    put_u32(wav.fp, 16);
    put_u16(wav.fp, 1);                     /* PCM */
    put_u16(wav.fp, 1);                     /* mono */
    put_u32(wav.fp, wav.rate);
    put_u32(wav.fp, wav.rate);              /* bytes per second */
    put_u16(wav.fp, 1);                     /* block align */
    put_u16(wav.fp, 8);                     /* bits */
    fwrite("data", 1, 4, wav.fp);
    put_u32(wav.fp, 0);

    snprintf(g_desc, sizeof g_desc, "WAV file at %lu Hz, %s", wav.rate,
             path);
    return 0;
}

void sndout_close(void)
{
    if (!wav.fp)
        return;

    /* Fill in the two lengths now that they are known. */
    if (fseek(wav.fp, 4, SEEK_SET) == 0)
        put_u32(wav.fp, 36 + wav.frames);
    if (fseek(wav.fp, 40, SEEK_SET) == 0)
        put_u32(wav.fp, wav.frames);

    fclose(wav.fp);
    wav.fp = NULL;
    g_desc[0] = '\0';
}

long sndout_space(void)
{
    unsigned long due;
    long space;

    if (!wav.fp)
        return 0;

    /* Paced by the clock, like a card.
     *
     * This used to return a fixed ring's worth, which meant a file was
     * never busy: the mixer wrote as fast as the loop could call it and
     * the client turned into a WAV generator -- megabytes a second, the
     * keyboard starved, and a test that measured "frames mixed per
     * second" measuring the host's disk instead. A sound card takes audio
     * at the sampling rate and not one frame faster, and so does this. */
    due = (unsigned long)(((tiger_u64)(snd_millis() - wav.started_ms) *
                           wav.rate) / 1000UL);

    if (due <= wav.frames)
        return 0;

    space = (long)(due - wav.frames);

    /* And never more than the ring would hold, so the host and DOS paths
       behave the same way when the loop is slow. */
    if (space > SNDOUT_RING)
        space = SNDOUT_RING;
    return space;
}

long sndout_write(const signed char *buf, long frames)
{
    long i;

    if (!wav.fp || frames <= 0)
        return 0;

    /* WAV 8-bit is unsigned, the mixer is signed: the conversion is the
       bias, and getting it wrong sounds like loud static. */
    for (i = 0; i < frames; i++)
        fputc((int)((unsigned char)(buf[i] + 128)), wav.fp);

    wav.frames += (unsigned long)frames;
    return frames;
}

unsigned long sndout_played(void)
{
    /* What a card would have consumed by now, which for a paced writer is
       what has been written -- the file is the device. */
    return wav.frames;
}

#endif
