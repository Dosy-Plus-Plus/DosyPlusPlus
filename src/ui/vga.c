/* vga.c - DOS text mode, or a UTF-8 terminal on the host. */
#include "vga.h"
#include "cp437.h"

#include <stdio.h>
#include <string.h>

static char g_err[160];
static int g_null = 0;

/* The height the display is actually running at. Both backends set it in
   vga_open(); the null backend takes whatever it was asked for, because
   there is nothing to constrain it and a --screenshot of a 50-row layout
   is a thing worth being able to take. */
static int g_rows = SCREEN_H;

/* 25 or 50, and nothing else: those are the two text modes a VGA has with
   a stock font, and the two the layout is built for. */
static int rows_or_25(int rows)
{
    return (rows >= 50) ? 50 : SCREEN_H;
}

int vga_open_null(int rows)
{
    g_null = 1;
    g_rows = rows_or_25(rows);
    return 0;
}

const char *vga_lasterr(void)
{
    return g_err[0] ? g_err : "no error";
}

/* Shared by both backends: render the buffer as UTF-8 text. */
void vga_dump(const screen *s, void *fp)
{
    FILE *f = (FILE *)fp;
    int x, y;

    for (y = 0; y < s->h; y++) {
        for (x = 0; x < s->w; x++) {
            char utf[4];
            cp437_to_utf8(s->buf[y * s->w + x].ch, utf);
            fputs(utf, f);
        }
        fputc('\n', f);
    }
}

#ifdef DOSYPP_WATT32

/* ------------------------------------------------------------------ DOS */

#include <stdlib.h>
#include <dos.h>
#include <go32.h>
#include <sys/farptr.h>
#include <sys/movedata.h>
#include <dpmi.h>
#include <conio.h>

#define VGA_TEXT_SEG 0xB8000

static int g_old_mode = -1;

/* Set the video mode through the BIOS. */
static void set_mode(int mode)
{
    __dpmi_regs r;

    memset(&r, 0, sizeof r);
    r.x.ax = (unsigned short)(0x0000 | (mode & 0xFF));
    __dpmi_int(0x10, &r);
}

static int get_mode(void)
{
    __dpmi_regs r;

    memset(&r, 0, sizeof r);
    r.x.ax = 0x0F00;
    __dpmi_int(0x10, &r);
    return r.h.al;
}

static void set_cursor_shape(int visible)
{
    __dpmi_regs r;

    memset(&r, 0, sizeof r);
    r.x.ax = 0x0100;
    /* Bit 5 of CH hides the cursor. Shape 6-7 is the usual underline. */
    r.x.cx = visible ? 0x0607 : 0x2000;
    __dpmi_int(0x10, &r);
}

static void move_cursor(int x, int y)
{
    __dpmi_regs r;

    memset(&r, 0, sizeof r);
    r.x.ax = 0x0200;
    r.x.bx = 0;                 /* page 0 */
    r.h.dh = (unsigned char)y;
    r.h.dl = (unsigned char)x;
    __dpmi_int(0x10, &r);
}

int vga_open(int rows)
{
    if (g_null)
        return 0;

    g_rows = rows_or_25(rows);
    g_old_mode = get_mode();

    /* Mode 3: 80x25 colour text. Also resets the palette and clears, which
       is what we want on entry. */
    set_mode(0x03);

    /* 80x50 is mode 3 with the 8x8 font loaded: int 10h AX=1112, BL=0.
       There is no separate mode number for it, which is why this is a
       second call rather than a different argument above. The font load
       also resets the cursor shape, so that is set afterwards.

       A machine whose BIOS does not do this (a genuine CGA or MDA) leaves
       the mode at 80x25 and the rows are read back below rather than
       assumed. */
    if (g_rows == 50) {
        __dpmi_regs r;

        memset(&r, 0, sizeof r);
        r.x.ax = 0x1112;
        r.x.bx = 0x0000;        /* BL=0: load the 8x8 set into block 0 */
        __dpmi_int(0x10, &r);

        /* What the BIOS says it ended up with, at 0040:0084 -- the last
           row index, so one less than the count. Trusting the request
           would mean drawing 50 rows into a 25-row mode on hardware that
           refused. */
        {
            int last = _farpeekb(_dos_ds, 0x484);

            g_rows = (last > 0) ? last + 1 : SCREEN_H;
            if (g_rows != 50)
                snprintf(g_err, sizeof g_err,
                         "the BIOS gave %d rows, not 50", g_rows);
        }
    }

    /* Turn OFF blink so the high background bit gives bright backgrounds
       instead of flashing text. BBS art uses bright backgrounds heavily;
       blinking them is unreadable. Attribute controller index 0x10, bit 3. */
    {
        __dpmi_regs r;
        memset(&r, 0, sizeof r);
        r.x.ax = 0x1003;
        r.x.bx = 0x0000;        /* BL=0: blink off, intensity on */
        __dpmi_int(0x10, &r);
    }

    set_cursor_shape(0);
    return 0;
}

void vga_close(void)
{
    if (g_null)
        return;

    set_cursor_shape(1);
    if (g_old_mode >= 0) {
        set_mode(g_old_mode);
        g_old_mode = -1;
    }
}

void vga_flush(screen *s)
{
    int y;

    if (g_null)
        return;

    /* Write in runs: one movedata per changed span rather than per cell.
       A cell is two bytes, so a 40-column run is one 80-byte transfer
       instead of 40 far-pokes through the DPMI window. */
    for (y = 0; y < s->h; y++) {
        int x = 0;

        while (x < s->w) {
            int i = y * s->w + x;
            int run;

            if (s->buf[i].ch == s->shadow[i].ch &&
                s->buf[i].attr == s->shadow[i].attr) {
                x++;
                continue;
            }

            run = 0;
            while (x + run < s->w) {
                int j = i + run;
                if (s->buf[j].ch == s->shadow[j].ch &&
                    s->buf[j].attr == s->shadow[j].attr)
                    break;
                run++;
            }

            dosmemput(&s->buf[i], (size_t)run * 2,
                      VGA_TEXT_SEG + (unsigned long)i * 2);
            memcpy(&s->shadow[i], &s->buf[i], (size_t)run * sizeof(cell));
            x += run;
        }
    }

    if (s->cursor_on) {
        set_cursor_shape(1);
        move_cursor(s->cursor_x, s->cursor_y);
    } else {
        set_cursor_shape(0);
    }
}

void vga_size(int *w, int *h)
{
    if (w) *w = SCREEN_W;
    if (h) *h = g_rows;
}

int vga_verify(const screen *s)
{
    cell *back;
    int i, n = s->w * s->h;
    int bad = 0;

    if (g_null)
        return -1;

    back = (cell *)malloc((size_t)n * sizeof *back);
    if (!back)
        return -1;

    dosmemget(VGA_TEXT_SEG, (size_t)n * 2, back);

    for (i = 0; i < n; i++)
        if (back[i].ch != s->buf[i].ch || back[i].attr != s->buf[i].attr)
            bad++;

    free(back);
    return bad;
}

#else

/* ----------------------------------------------------------------- host */

#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

static struct termios g_saved;
static int g_raw = 0;

/* VGA colour order is not ANSI's: VGA is K R G Y B M C W with blue and red
   swapped relative to ANSI's K R G Y B M C W... specifically VGA 1 is blue
   while ANSI 1 is red. Map explicitly. */
static const int VGA_TO_ANSI[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

int vga_open(int rows)
{
    struct termios raw;

    if (g_null)
        return 0;

    g_rows = rows_or_25(rows);

    if (!isatty(STDIN_FILENO)) {
        snprintf(g_err, sizeof g_err, "stdin is not a terminal");
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0) {
        snprintf(g_err, sizeof g_err, "tcgetattr failed");
        return -1;
    }

    raw = g_saved;
    raw.c_lflag &= ~(unsigned long)(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(unsigned long)(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        snprintf(g_err, sizeof g_err, "tcsetattr failed");
        return -1;
    }
    g_raw = 1;

    /* Alternate screen, cursor off, clear. */
    fputs("\033[?1049h\033[?25l\033[2J", stdout);
    fflush(stdout);

    /* A terminal shorter than the layout wants would draw the bottom rows
       off the end and scroll the rest, which looks like a bug in the UI.
       Fall back to 25 and say why. */
    {
        struct winsize ws;

        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 &&
            ws.ws_row < g_rows) {
            snprintf(g_err, sizeof g_err,
                     "the terminal has %d rows, not %d",
                     (int)ws.ws_row, g_rows);
            g_rows = (ws.ws_row >= SCREEN_H) ? SCREEN_H : (int)ws.ws_row;
        }
    }
    return 0;
}

void vga_close(void)
{
    if (g_null || !g_raw)
        return;
    fputs("\033[0m\033[?25h\033[?1049l", stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
    g_raw = 0;
}

static void emit_attr(unsigned char attr)
{
    int fg = ATTR_FG(attr);
    int bg = ATTR_BG(attr);
    int bold = (fg & 8) ? 1 : 0;

    /* Bright background: VGA without blink has 16 background colours, but
       ANSI needs the 90-107 range for the top eight. */
    printf("\033[%d;%d;%dm", bold,
           30 + VGA_TO_ANSI[fg & 7],
           ((attr & 0x80) ? 100 : 40) + VGA_TO_ANSI[bg & 7]);
}

void vga_flush(screen *s)
{
    int y;
    int last_attr = -1;

    if (g_null)
        return;

    for (y = 0; y < s->h; y++) {
        int x = 0;
        int dirty = 0;

        for (x = 0; x < s->w; x++) {
            int i = y * s->w + x;
            if (s->buf[i].ch != s->shadow[i].ch ||
                s->buf[i].attr != s->shadow[i].attr) {
                dirty = 1;
                break;
            }
        }
        if (!dirty)
            continue;

        /* Terminals are cheap to write to compared with a DPMI window, so
           redraw the whole dirty row rather than tracking runs. */
        printf("\033[%d;1H", y + 1);
        last_attr = -1;

        for (x = 0; x < s->w; x++) {
            int i = y * s->w + x;
            char utf[4];

            if ((int)s->buf[i].attr != last_attr) {
                emit_attr(s->buf[i].attr);
                last_attr = s->buf[i].attr;
            }
            cp437_to_utf8(s->buf[i].ch, utf);
            fputs(utf, stdout);
        }

        memcpy(&s->shadow[y * s->w], &s->buf[y * s->w],
               (size_t)s->w * sizeof(cell));
    }

    if (s->cursor_on)
        printf("\033[%d;%dH\033[?25h", s->cursor_y + 1, s->cursor_x + 1);
    else
        fputs("\033[?25l", stdout);

    fputs("\033[0m", stdout);
    fflush(stdout);
}

int vga_verify(const screen *s)
{
    /* No readback from a terminal: what the emulator or the user sees is
       beyond our reach here. */
    (void)s;
    return -1;
}

void vga_size(int *w, int *h)
{
    if (w) *w = SCREEN_W;
    if (h) *h = g_rows;
}

#endif
