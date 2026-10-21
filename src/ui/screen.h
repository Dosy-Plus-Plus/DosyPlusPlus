/* screen.h - an 80x25 cell buffer with dirty tracking.
 *
 * Everything the UI draws goes into `buf`. vga_flush() then compares it
 * against `shadow` and writes only the cells that changed. Two reasons that
 * matters on the target:
 *
 *   - Writing all 2,000 cells every frame is 4,000 bytes through a DPMI
 *     far-memory window on a 486. Diffing turns a redraw into a handful of
 *     short runs, which is the difference between a UI that feels alive and
 *     one that visibly repaints.
 *   - Unchanged cells are never touched, so there is no flicker and no snow
 *     on real hardware without waiting for retrace.
 *
 * Pure C, no DOS dependency: the whole layout engine unit-tests natively,
 * which is the only sane way to develop a text UI.
 *
 * An attribute is the VGA byte: bit 7 blink, bits 6-4 background, bits 3-0
 * foreground.
 */
#ifndef DOSYPP_SCREEN_H
#define DOSYPP_SCREEN_H

#include <stddef.h>

#define SCREEN_W 80
#define SCREEN_H 25

/* VGA text colours. */
#define C_BLACK    0
#define C_BLUE     1
#define C_GREEN    2
#define C_CYAN     3
#define C_RED      4
#define C_MAGENTA  5
#define C_BROWN    6
#define C_LGREY    7
#define C_DGREY    8
#define C_LBLUE    9
#define C_LGREEN   10
#define C_LCYAN    11
#define C_LRED     12
#define C_LMAGENTA 13
#define C_YELLOW   14
#define C_WHITE    15

#define ATTR(fg, bg)  ((unsigned char)(((bg) << 4) | ((fg) & 0x0F)))
#define ATTR_BLINK    0x80
#define ATTR_FG(a)    ((a) & 0x0F)
#define ATTR_BG(a)    (((a) >> 4) & 0x07)

/* CP437 box drawing and shading, named so the drawing code reads. */
#define CH_SHADE_LIGHT  0xB0   /* ░ */
#define CH_SHADE_MED    0xB1   /* ▒ */
#define CH_SHADE_DARK   0xB2   /* ▓ */
#define CH_BLOCK_FULL   0xDB   /* █ */
#define CH_BLOCK_LEFT   0xDD   /* ▌ */
#define CH_BLOCK_RIGHT  0xDE   /* ▐ */
#define CH_BLOCK_LOWER  0xDC   /* ▄ */
#define CH_BLOCK_UPPER  0xDF   /* ▀ */
#define CH_BULLET       0x07   /* • */
#define CH_NOTE         0x0E   /* ♫ */
#define CH_ARROW_RIGHT  0x10   /* ► */
#define CH_ARROW_LEFT   0x11   /* ◄ */
#define CH_ARROW_UP     0x1E   /* ▲ */
#define CH_ARROW_DOWN   0x1F   /* ▼ */

typedef struct {
    unsigned char ch;
    unsigned char attr;
} cell;

typedef struct {
    int   w, h;
    cell *buf;        /* what we want on screen */
    cell *shadow;     /* what is on screen now */
    int   cursor_x, cursor_y;
    int   cursor_on;
} screen;

/* Box styles for screen_box. */
typedef enum {
    BOX_SINGLE,       /* ┌─┐ */
    BOX_DOUBLE,       /* ╔═╗ */
    BOX_SINGLE_V_DOUBLE_H,
    BOX_NONE          /* spaces: a filled panel with no border */
} box_style;

int  screen_init(screen *s, int w, int h);
void screen_free(screen *s);

/* Mark everything dirty, so the next flush rewrites the whole display.
   Needed after a mode switch or when something else has written to video
   memory behind us. */
void screen_invalidate(screen *s);

void screen_clear(screen *s, unsigned char attr);
void screen_fill(screen *s, int x, int y, int w, int h,
                 unsigned char ch, unsigned char attr);

/* All drawing clips silently rather than asserting: a UI that crashes
   because a nick was long is worse than one that truncates. */
void screen_putc(screen *s, int x, int y, unsigned char ch,
                 unsigned char attr);

/* Writes CP437 bytes as-is. Use cp437_from_utf8() first for hub text. */
void screen_puts(screen *s, int x, int y, const char *str,
                 unsigned char attr);

/* Writes at most n columns, padding with spaces to exactly n. The padding
   matters: it is what erases the previous, longer value in a list. */
void screen_puts_pad(screen *s, int x, int y, const char *str, int n,
                     unsigned char attr);

/* Centred within [x, x+w). */
void screen_puts_center(screen *s, int x, int y, int w, const char *str,
                        unsigned char attr);

void screen_hline(screen *s, int x, int y, int w, unsigned char ch,
                  unsigned char attr);
void screen_vline(screen *s, int x, int y, int h, unsigned char ch,
                  unsigned char attr);

/* Border plus interior fill. */
void screen_box(screen *s, int x, int y, int w, int h, box_style style,
                unsigned char attr);

/* A title sitting in the top border, BBS style: ─┤ TITLE ├─ */
void screen_box_title(screen *s, int x, int y, int w, const char *title,
                      unsigned char border_attr, unsigned char text_attr);

/* Scroll a rectangle up by n lines, filling the bottom. Used by the chat
   pane; cheaper than redrawing every line from the scrollback. */
void screen_scroll_up(screen *s, int x, int y, int w, int h, int n,
                      unsigned char attr);

/* Set the attribute of a run without touching its characters -- how the
   lightbar highlights a row. */
void screen_set_attr(screen *s, int x, int y, int w, unsigned char attr);

void screen_cursor(screen *s, int x, int y);
void screen_cursor_hide(screen *s);

/* Read back, for tests. Returns a space-filled cell when out of bounds. */
cell screen_at(const screen *s, int x, int y);

/* Copy a row's characters into out (NUL-terminated, at least w+1 bytes).
   Tests assert on this rather than on individual cells. */
void screen_row_text(const screen *s, int y, char *out);

#endif /* DOSYPP_SCREEN_H */
