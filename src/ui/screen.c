/* screen.c - cell buffer and drawing primitives. */
#include "screen.h"

#include <stdlib.h>
#include <string.h>

/* Border glyphs per style: TL TR BL BR H V */
static const unsigned char BORDERS[][6] = {
    { 0xDA, 0xBF, 0xC0, 0xD9, 0xC4, 0xB3 },   /* BOX_SINGLE  ┌┐└┘─│ */
    { 0xC9, 0xBB, 0xC8, 0xBC, 0xCD, 0xBA },   /* BOX_DOUBLE  ╔╗╚╝═║ */
    { 0xD5, 0xB8, 0xD4, 0xBE, 0xCD, 0xB3 },   /* single V, double H */
    { ' ',  ' ',  ' ',  ' ',  ' ',  ' '  }    /* BOX_NONE */
};

int screen_init(screen *s, int w, int h)
{
    memset(s, 0, sizeof *s);

    if (w <= 0 || h <= 0)
        return -1;

    s->buf = (cell *)malloc((size_t)(w * h) * sizeof *s->buf);
    s->shadow = (cell *)malloc((size_t)(w * h) * sizeof *s->shadow);
    if (!s->buf || !s->shadow) {
        free(s->buf);
        free(s->shadow);
        memset(s, 0, sizeof *s);
        return -1;
    }

    s->w = w;
    s->h = h;
    s->cursor_on = 0;

    screen_clear(s, ATTR(C_LGREY, C_BLACK));
    screen_invalidate(s);
    return 0;
}

void screen_free(screen *s)
{
    free(s->buf);
    free(s->shadow);
    memset(s, 0, sizeof *s);
}

void screen_invalidate(screen *s)
{
    /* Make every shadow cell differ from anything drawable, so the next
       flush writes the lot. 0xFF attribute with a NUL char is not a cell any
       drawing path produces. */
    int i, n = s->w * s->h;

    for (i = 0; i < n; i++) {
        s->shadow[i].ch = 0;
        s->shadow[i].attr = 0xFF;
    }
}

void screen_clear(screen *s, unsigned char attr)
{
    int i, n = s->w * s->h;

    for (i = 0; i < n; i++) {
        s->buf[i].ch = ' ';
        s->buf[i].attr = attr;
    }
}

void screen_putc(screen *s, int x, int y, unsigned char ch,
                 unsigned char attr)
{
    cell *c;

    if (x < 0 || y < 0 || x >= s->w || y >= s->h)
        return;                      /* clip, do not complain */

    c = &s->buf[y * s->w + x];
    c->ch = ch;
    c->attr = attr;
}

void screen_fill(screen *s, int x, int y, int w, int h,
                 unsigned char ch, unsigned char attr)
{
    int ix, iy;

    for (iy = y; iy < y + h; iy++)
        for (ix = x; ix < x + w; ix++)
            screen_putc(s, ix, iy, ch, attr);
}

void screen_puts(screen *s, int x, int y, const char *str,
                 unsigned char attr)
{
    int i;

    if (!str)
        return;
    for (i = 0; str[i]; i++)
        screen_putc(s, x + i, y, (unsigned char)str[i], attr);
}

void screen_puts_pad(screen *s, int x, int y, const char *str, int n,
                     unsigned char attr)
{
    int i;
    size_t len = str ? strlen(str) : 0;

    for (i = 0; i < n; i++) {
        unsigned char ch = (i < (int)len) ? (unsigned char)str[i] : ' ';
        screen_putc(s, x + i, y, ch, attr);
    }
}

void screen_puts_center(screen *s, int x, int y, int w, const char *str,
                        unsigned char attr)
{
    int len = str ? (int)strlen(str) : 0;
    int off = (w - len) / 2;

    if (off < 0)
        off = 0;
    screen_puts(s, x + off, y, str, attr);
}

void screen_hline(screen *s, int x, int y, int w, unsigned char ch,
                  unsigned char attr)
{
    int i;

    for (i = 0; i < w; i++)
        screen_putc(s, x + i, y, ch, attr);
}

void screen_vline(screen *s, int x, int y, int h, unsigned char ch,
                  unsigned char attr)
{
    int i;

    for (i = 0; i < h; i++)
        screen_putc(s, x, y + i, ch, attr);
}

void screen_box(screen *s, int x, int y, int w, int h, box_style style,
                unsigned char attr)
{
    const unsigned char *b;

    if (w < 2 || h < 2)
        return;
    if (style < 0 || style > BOX_NONE)
        style = BOX_SINGLE;
    b = BORDERS[style];

    screen_fill(s, x + 1, y + 1, w - 2, h - 2, ' ', attr);

    screen_hline(s, x + 1, y,         w - 2, b[4], attr);
    screen_hline(s, x + 1, y + h - 1, w - 2, b[4], attr);
    screen_vline(s, x,         y + 1, h - 2, b[5], attr);
    screen_vline(s, x + w - 1, y + 1, h - 2, b[5], attr);

    screen_putc(s, x,         y,         b[0], attr);
    screen_putc(s, x + w - 1, y,         b[1], attr);
    screen_putc(s, x,         y + h - 1, b[2], attr);
    screen_putc(s, x + w - 1, y + h - 1, b[3], attr);
}

void screen_box_title(screen *s, int x, int y, int w, const char *title,
                      unsigned char border_attr, unsigned char text_attr)
{
    int len = title ? (int)strlen(title) : 0;
    int tx;

    if (len == 0 || w < len + 6)
        return;

    /* ─┤ TITLE ├─ two columns in from the corner, so the corner glyph is
       never overwritten even on a narrow panel. */
    tx = x + 2;
    screen_putc(s, tx, y, 0xB4, border_attr);          /* ┤ */
    screen_putc(s, tx + 1, y, ' ', text_attr);
    screen_puts(s, tx + 2, y, title, text_attr);
    screen_putc(s, tx + 2 + len, y, ' ', text_attr);
    screen_putc(s, tx + 3 + len, y, 0xC3, border_attr); /* ├ */
}

void screen_scroll_up(screen *s, int x, int y, int w, int h, int n,
                      unsigned char attr)
{
    int iy;

    if (n <= 0 || h <= 0)
        return;
    if (n >= h) {
        screen_fill(s, x, y, w, h, ' ', attr);
        return;
    }

    for (iy = y; iy < y + h - n; iy++) {
        int src = iy + n;
        int ix;
        for (ix = x; ix < x + w; ix++) {
            if (ix < 0 || ix >= s->w || iy < 0 || iy >= s->h ||
                src < 0 || src >= s->h)
                continue;
            s->buf[iy * s->w + ix] = s->buf[src * s->w + ix];
        }
    }
    screen_fill(s, x, y + h - n, w, n, ' ', attr);
}

void screen_set_attr(screen *s, int x, int y, int w, unsigned char attr)
{
    int i;

    if (y < 0 || y >= s->h)
        return;
    for (i = 0; i < w; i++) {
        int ix = x + i;
        if (ix < 0 || ix >= s->w)
            continue;
        s->buf[y * s->w + ix].attr = attr;
    }
}

void screen_cursor(screen *s, int x, int y)
{
    s->cursor_x = x;
    s->cursor_y = y;
    s->cursor_on = 1;
}

void screen_cursor_hide(screen *s)
{
    s->cursor_on = 0;
}

cell screen_at(const screen *s, int x, int y)
{
    cell blank;

    if (x < 0 || y < 0 || x >= s->w || y >= s->h) {
        blank.ch = ' ';
        blank.attr = 0;
        return blank;
    }
    return s->buf[y * s->w + x];
}

void screen_row_text(const screen *s, int y, char *out)
{
    int x;

    for (x = 0; x < s->w; x++)
        out[x] = (char)screen_at(s, x, y).ch;
    out[s->w] = '\0';
}
