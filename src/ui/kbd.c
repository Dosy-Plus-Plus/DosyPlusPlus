/* kbd.c - non-blocking keyboard: int 9h on DOS, ANSI parsing on the host. */
#include "kbd.h"

/* --- scancode decoding (portable, so it can be unit tested) ----------
 *
 * The tables are the reason to factor this out: a wrong entry is invisible
 * until someone types that character, and nobody tests every key by hand.
 * The ISR calls kbd_decode(); test/host/test_ui.c checks it.
 */
/* Scancode set 1 -> ASCII, unshifted and shifted. Index is the scancode;
   0 means "not a character key". */
static const unsigned char SC_PLAIN[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6',    /* 00-07 */
    '7',  '8', '9', '0', '-', '=', 8,   9,      /* 08-0F */
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',    /* 10-17 */
    'o',  'p', '[', ']', 13,  0,   'a', 's',    /* 18-1F */
    'd',  'f', 'g', 'h', 'j', 'k', 'l', ';',    /* 20-27 */
    '\'', '`', 0,   '\\','z', 'x', 'c', 'v',    /* 28-2F */
    'b',  'n', 'm', ',', '.', '/', 0,   '*',    /* 30-37 */
    0,    ' ', 0,   0,   0,   0,   0,   0,      /* 38-3F */
    0,    0,   0,   0,   0,   0,   0,   '7',    /* 40-47 */
    '8',  '9', '-', '4', '5', '6', '+', '1',    /* 48-4F */
    '2',  '3', '0', '.', 0,   0,   0,   0,      /* 50-57 */
    0,    0,   0,   0,   0,   0,   0,   0,      /* 58-5F */
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned char SC_SHIFT[128] = {
    0,    27,  '!', '@', '#', '$', '%', '^',
    '&',  '*', '(', ')', '_', '+', 8,   9,
    'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O',  'P', '{', '}', 13,  0,   'A', 'S',
    'D',  'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"',  '~', 0,   '|', 'Z', 'X', 'C', 'V',
    'B',  'N', 'M', '<', '>', '?', 0,   '*',
    0,    ' ', 0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   '7',
    '8',  '9', '-', '4', '5', '6', '+', '1',
    '2',  '3', '0', '.', 0,   0,   0,   0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

/* Scancodes that map to KEY_* regardless of the 0xE0 prefix. The grey
   cursor keys send E0-prefixed codes; the numeric keypad sends the same
   codes without a prefix when Num Lock is off. Both must work. */
int kbd_special_key(unsigned char sc)
{
    switch (sc) {
    case 0x47: return KEY_HOME;
    case 0x48: return KEY_UP;
    case 0x49: return KEY_PGUP;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x4F: return KEY_END;
    case 0x50: return KEY_DOWN;
    case 0x51: return KEY_PGDN;
    case 0x52: return KEY_INS;
    case 0x53: return KEY_DEL;
    case 0x3B: return KEY_F1;
    case 0x3C: return KEY_F2;
    case 0x3D: return KEY_F3;
    case 0x3E: return KEY_F4;
    case 0x3F: return KEY_F5;
    case 0x40: return KEY_F6;
    case 0x41: return KEY_F7;
    case 0x42: return KEY_F8;
    case 0x43: return KEY_F9;
    case 0x44: return KEY_F10;
    default:   return KEY_NONE;
    }
}


int kbd_decode(unsigned char sc, int ext, unsigned mods)
{
    unsigned char code = sc & 0x7F;
    unsigned char ch;

    if (sc & 0x80)
        return KEY_NONE;                 /* a release is not a keypress */

    /* A prefixed 0x1C is the keypad Enter and 0x35 the keypad slash;
       neither is a cursor key, so both fall through to the tables. */
    if (!(ext && (code == 0x1C || code == 0x35))) {
        int key = kbd_special_key(code);
        if (key != KEY_NONE)
            return key;
    }

    ch = (mods & KBD_SHIFT) ? SC_SHIFT[code] : SC_PLAIN[code];
    if (!ch)
        return KEY_NONE;

    /* Ctrl+letter becomes a control code, which is what a DOS program
       expects and what the host terminal delivers too. */
    if (mods & KBD_CTRL) {
        if (ch >= 'a' && ch <= 'z')
            return ch - 'a' + 1;
        if (ch >= 'A' && ch <= 'Z')
            return ch - 'A' + 1;
    }

    return (int)ch;
}

#ifdef DOSYPP_WATT32

#include <conio.h>
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>      /* _exit, for the fatal-signal path */

#define KB_DATA   0x60      /* keyboard controller data */
#define KB_STATUS 0x64
#define PIC_CMD   0x20
#define PIC_EOI   0x20

#define RING_SIZE 64        /* vs the BIOS's 16: enough that a busy main
                               loop cannot lose keystrokes */

/* --- state shared with the ISR ---------------------------------------
 *
 * Everything here is touched from interrupt context, so it is volatile and
 * it is locked into memory at install time. A page fault inside an ISR is
 * not recoverable. */
static volatile int  g_ring[RING_SIZE];
static volatile int  g_head;        /* producer: the ISR */
static volatile int  g_tail;        /* consumer: kbd_get */
static volatile unsigned long g_lost;   /* dropped: the ring filled */
static volatile unsigned long g_got;    /* scancodes the ISR decoded */
static volatile unsigned long g_ints;   /* times the ISR ran at all */
static volatile unsigned char g_ext;        /* 0xE0 prefix seen */
static volatile unsigned char g_mods;
static volatile unsigned char g_down[128];  /* make/break state by scancode */

static void ring_put(int key)
{
    int next = (g_head + 1) % RING_SIZE;

    if (next == g_tail) {
        /* Full. Drop the new key and count it, rather than overwriting the
           oldest -- losing the key someone just pressed is less confusing
           than losing one they pressed earlier. */
        g_lost++;
        return;
    }
    g_ring[g_head] = key;
    g_head = next;
    g_got++;
}

/* The ISR itself. Must touch only locked code and data: no library calls,
   no floating point, nothing that could fault. */
static void kbd_isr(void)
{
    unsigned char sc = inportb(KB_DATA);

    g_ints++;
    unsigned char code = sc & 0x7F;
    int released = (sc & 0x80) != 0;

    if (sc == 0xE0) {
        g_ext = 1;
        outportb(PIC_CMD, PIC_EOI);
        return;
    }
    if (sc == 0xE1) {
        /* Pause/Break sends a longer sequence; ignore the whole thing
           rather than decode half of it as a key. */
        g_ext = 0;
        outportb(PIC_CMD, PIC_EOI);
        return;
    }

    g_down[code] = (unsigned char)(released ? 0 : 1);

    /* Modifiers, tracked on both make and break -- the entire reason for
       having an ISR instead of the BIOS. */
    switch (code) {
    case 0x2A: case 0x36:
        if (released) g_mods &= (unsigned char)~KBD_SHIFT;
        else          g_mods |= KBD_SHIFT;
        g_ext = 0;
        outportb(PIC_CMD, PIC_EOI);
        return;
    case 0x1D:
        if (released) g_mods &= (unsigned char)~KBD_CTRL;
        else          g_mods |= KBD_CTRL;
        g_ext = 0;
        outportb(PIC_CMD, PIC_EOI);
        return;
    case 0x38:
        if (released) g_mods &= (unsigned char)~KBD_ALT;
        else          g_mods |= KBD_ALT;
        g_ext = 0;
        outportb(PIC_CMD, PIC_EOI);
        return;
    default:
        break;
    }

    if (!released) {
        int key = kbd_decode(sc, g_ext, g_mods);
        if (key != KEY_NONE)
            ring_put(key);
    }

    g_ext = 0;
    outportb(PIC_CMD, PIC_EOI);
}

/* --- install / remove ------------------------------------------------ */

static _go32_dpmi_seginfo g_old_vec;
static _go32_dpmi_seginfo g_new_vec;
static int g_installed;
static int g_use_bios = 1;
static void (*g_old_sig[4])(int);

static void kbd_uninstall(void)
{
    if (!g_installed)
        return;

    _go32_dpmi_set_protected_mode_interrupt_vector(9, &g_old_vec);
    _go32_dpmi_free_iret_wrapper(&g_new_vec);
    g_installed = 0;

    /* Restore the BIOS default typematic rate: 10.9 cps, 500ms delay. */
    while (inportb(KB_STATUS) & 0x02)
        ;
    outportb(KB_DATA, 0xF3);
    while (inportb(KB_STATUS) & 0x02)
        ;
    outportb(KB_DATA, 0x2B);
}

/* A crash must not leave int 9h pointing into a freed wrapper: that is a
   machine you have to power-cycle. Chain to whatever was there before so
   DJGPP still prints its traceback. */
static void fatal_signal(int sig)
{
    kbd_uninstall();

    switch (sig) {
    case SIGSEGV: if (g_old_sig[0]) g_old_sig[0](sig); break;
    case SIGFPE:  if (g_old_sig[1]) g_old_sig[1](sig); break;
    case SIGILL:  if (g_old_sig[2]) g_old_sig[2](sig); break;
    case SIGABRT: if (g_old_sig[3]) g_old_sig[3](sig); break;
    default: break;
    }
    _exit(1);
}

static int kbd_install(void)
{
    /* Lock the handler and its data. An unlocked ISR can take a page fault
       with interrupts off, which is unrecoverable. */
    if (_go32_dpmi_lock_code((void *)kbd_isr, 8192) != 0)
        return -1;
    /* kbd_decode is called from interrupt context, so it must be locked
       too -- it lives outside the ISR's own 8KB window. */
    if (_go32_dpmi_lock_code((void *)kbd_decode, 4096) != 0)
        return -1;
    if (_go32_dpmi_lock_data((void *)g_ring, sizeof g_ring) != 0)
        return -1;
    if (_go32_dpmi_lock_data((void *)g_down, sizeof g_down) != 0)
        return -1;
    if (_go32_dpmi_lock_data((void *)&g_head, sizeof g_head) != 0)
        return -1;
    if (_go32_dpmi_lock_data((void *)&g_tail, sizeof g_tail) != 0)
        return -1;
    if (_go32_dpmi_lock_data((void *)&g_mods, sizeof g_mods) != 0)
        return -1;
    if (_go32_dpmi_lock_data((void *)&g_ext, sizeof g_ext) != 0)
        return -1;
    if (_go32_dpmi_lock_data((void *)SC_PLAIN, sizeof SC_PLAIN) != 0)
        return -1;
    if (_go32_dpmi_lock_data((void *)SC_SHIFT, sizeof SC_SHIFT) != 0)
        return -1;

    _go32_dpmi_get_protected_mode_interrupt_vector(9, &g_old_vec);

    g_new_vec.pm_offset = (unsigned long)kbd_isr;
    g_new_vec.pm_selector = _go32_my_cs();
    if (_go32_dpmi_allocate_iret_wrapper(&g_new_vec) != 0)
        return -1;

    if (_go32_dpmi_set_protected_mode_interrupt_vector(9, &g_new_vec) != 0) {
        _go32_dpmi_free_iret_wrapper(&g_new_vec);
        return -1;
    }

    g_installed = 1;

    g_old_sig[0] = signal(SIGSEGV, fatal_signal);
    g_old_sig[1] = signal(SIGFPE,  fatal_signal);
    g_old_sig[2] = signal(SIGILL,  fatal_signal);
    g_old_sig[3] = signal(SIGABRT, fatal_signal);
    atexit(kbd_uninstall);

    /* Fast typematic: 30 cps with a 250ms delay, so a held cursor key moves
       a lightbar at a usable speed instead of the BIOS's crawl. Command
       0xF3, then the rate byte. Failure here is cosmetic, so it is not
       checked. */
    while (inportb(KB_STATUS) & 0x02)
        ;
    outportb(KB_DATA, 0xF3);
    while (inportb(KB_STATUS) & 0x02)
        ;
    outportb(KB_DATA, 0x00);

    return 0;
}

void kbd_open(void)
{
    memset((void *)g_down, 0, sizeof g_down);
    g_head = g_tail = 0;
    g_lost = 0;
    g_got = 0;
    g_ints = 0;
    g_mods = 0;
    g_ext = 0;

    g_use_bios = (kbd_install() != 0);
}

void kbd_open_bios(void)
{
    g_use_bios = 1;
}

void kbd_close(void)
{
    kbd_uninstall();
}

int kbd_have_isr(void)
{
    return !g_use_bios;
}

unsigned kbd_mods(void)
{
    return g_use_bios ? 0u : (unsigned)g_mods;
}

int kbd_inject(unsigned char scancode)
{
    int spin;

    if (g_use_bios)
        return -1;

    /* First wait for the OUTPUT buffer to empty, i.e. for the ISR to have
       read the previous injected byte (status bit 0). Skipping this was a
       real bug: injecting a second byte while the first was still pending
       simply overwrote it, so a two-byte sequence like E0 50 arrived as one
       stray code and only 3 of 10 injections produced an interrupt. */
    for (spin = 0; spin < 100000; spin++)
        if (!(inportb(KB_STATUS) & 0x01))
            break;
    if (inportb(KB_STATUS) & 0x01)
        return -1;

    /* Then wait for the input buffer to drain, tell the controller the next
       byte is to be presented as keyboard output, and hand it over. */
    for (spin = 0; spin < 100000; spin++)
        if (!(inportb(KB_STATUS) & 0x02))
            break;
    if (inportb(KB_STATUS) & 0x02)
        return -1;

    outportb(KB_STATUS, 0xD2);

    for (spin = 0; spin < 100000; spin++)
        if (!(inportb(KB_STATUS) & 0x02))
            break;
    if (inportb(KB_STATUS) & 0x02)
        return -1;

    outportb(KB_DATA, scancode);
    return 0;
}

void kbd_stats(unsigned long *got, unsigned long *lost)
{
    if (got)  *got = g_ints;        /* interrupts seen, not keys queued:
                                       the question is whether we are wired
                                       to the hardware at all */
    if (lost) *lost = g_lost;
}

/* The BIOS path, kept as the fallback. */
static int bios_get(void)
{
    int ch;

    if (!kbhit())
        return KEY_NONE;

    ch = getch();
    if (ch == 0x00 || ch == 0xE0) {
        return kbd_special_key((unsigned char)getch());
    }
    return ch;
}

int kbd_get(void)
{
    int key;

    if (g_use_bios)
        return bios_get();

    if (g_tail == g_head)
        return KEY_NONE;

    key = g_ring[g_tail];
    g_tail = (g_tail + 1) % RING_SIZE;
    return key;
}

#else

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

/* The host terminal backend puts the tty in raw mode with VMIN/VTIME 0, so
   a read returns at once with whatever is buffered. But --null-display never
   touches the terminal, and with stdin a pipe (a test harness, a demo script)
   a plain read then blocks forever: the run-secs deadline never comes round
   and the client hangs with no indication why. So set O_NONBLOCK here too,
   once, whatever the display is. */
static int raw_byte(void)
{
    static int nonblock_set = 0;
    unsigned char c;
    ssize_t n;

    if (!nonblock_set) {
        int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (fl != -1)
            fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
        nonblock_set = 1;
    }

    n = read(STDIN_FILENO, &c, 1);

    return (n == 1) ? (int)c : -1;
}

void kbd_open(void) { }
void kbd_open_bios(void) { }
void kbd_close(void) { }

/* A terminal reports no key-up events and no modifier state, so these are
   honestly zero rather than guessed at. */
unsigned kbd_mods(void) { return 0u; }
int kbd_have_isr(void) { return 0; }
void kbd_stats(unsigned long *got, unsigned long *lost)
{
    if (got)  *got = 0;
    if (lost) *lost = 0;
}

int kbd_inject(unsigned char scancode)
{
    (void)scancode;
    return -1;      /* no keyboard controller to talk to */
}

int kbd_get(void)
{
    int c = raw_byte();

    if (c < 0)
        return KEY_NONE;

    if (c != 27)
        return (c == 127) ? KEY_BACKSPACE : c;

    /* ESC: either a bare Escape or the start of a sequence. A bare Escape
       cannot be distinguished without a timeout, and since the terminal is
       non-blocking, a sequence's remaining bytes are already buffered -- so
       if nothing follows, it really was Escape. */
    c = raw_byte();
    if (c < 0)
        return KEY_ESC;

    if (c == 'O') {                    /* SS3: function keys on some terms */
        int k = raw_byte();
        switch (k) {
        case 'P': return KEY_F1;
        case 'Q': return KEY_F2;
        case 'R': return KEY_F3;
        case 'S': return KEY_F4;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        default:  return KEY_NONE;
        }
    }

    if (c != '[')
        return KEY_ESC;

    c = raw_byte();
    switch (c) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    default: break;
    }

    /* Numeric form: ESC [ <n> ~ */
    if (c >= '0' && c <= '9') {
        int n = c - '0';
        int k;

        while ((k = raw_byte()) >= '0' && k <= '9')
            n = n * 10 + (k - '0');
        /* k is now '~' or something unexpected; either way the sequence is
           consumed. */
        switch (n) {
        case 1:  return KEY_HOME;
        case 2:  return KEY_INS;
        case 3:  return KEY_DEL;
        case 4:  return KEY_END;
        case 5:  return KEY_PGUP;
        case 6:  return KEY_PGDN;
        case 11: return KEY_F1;
        case 12: return KEY_F2;
        case 13: return KEY_F3;
        case 14: return KEY_F4;
        case 15: return KEY_F5;
        case 17: return KEY_F6;
        case 18: return KEY_F7;
        case 19: return KEY_F8;
        case 20: return KEY_F9;
        case 21: return KEY_F10;
        default: return KEY_NONE;
        }
    }

    return KEY_NONE;
}

#endif
