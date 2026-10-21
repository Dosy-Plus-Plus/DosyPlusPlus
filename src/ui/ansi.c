/* ansi.c - ANSI SGR and PCBoard @X, stripped or rendered. */
#include "ansi.h"

#include <string.h>

/* ANSI colour order differs from VGA's: ANSI 1 is red, VGA 1 is blue. */
static const unsigned char ANSI_TO_VGA[8] = {
    C_BLACK, C_RED, C_GREEN, C_BROWN, C_BLUE, C_MAGENTA, C_CYAN, C_LGREY
};

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Skip one CSI sequence starting at the ESC. Returns the number of bytes
   consumed. A sequence that never terminates consumes just the ESC, so a
   truncated or malicious string cannot run off the end. */
static size_t skip_escape(const char *p)
{
    size_t i = 1;

    if (p[1] == '\0')
        return 1;

    if (p[1] == '[') {
        i = 2;
        /* Parameter and intermediate bytes, then a final byte 0x40-0x7E. */
        while (p[i] && (unsigned char)p[i] >= 0x20 &&
               (unsigned char)p[i] <= 0x3F)
            i++;
        if (p[i] && (unsigned char)p[i] >= 0x40 &&
            (unsigned char)p[i] <= 0x7E)
            i++;
        return i;
    }

    /* OSC and friends run until BEL or ST. These can carry arbitrary text,
       including an answerback string on a real terminal, so they are
       consumed whole and dropped. */
    if (p[1] == ']' || p[1] == 'P' || p[1] == '^' || p[1] == '_') {
        i = 2;
        while (p[i]) {
            if (p[i] == '\a')
                return i + 1;
            if (p[i] == 27 && p[i + 1] == '\\')
                return i + 2;
            i++;
        }
        return i;
    }

    /* Two-character sequences: ESC c, ESC M, ESC 7 ... */
    return 2;
}

size_t ansi_strip(char *dst, size_t dstsz, const char *src)
{
    size_t o = 0;

    if (dstsz == 0)
        return 0;

    while (*src && o + 1 < dstsz) {
        if (*src == 27) {
            src += skip_escape(src);
            continue;
        }

        /* PCBoard: @X followed by two hex digits is background+foreground.
           "@@" is a literal at-sign. */
        if (src[0] == '@' && (src[1] == 'X' || src[1] == 'x') &&
            hexval(src[2]) >= 0 && hexval(src[3]) >= 0) {
            src += 4;
            continue;
        }
        if (src[0] == '@' && src[1] == '@') {
            dst[o++] = '@';
            src += 2;
            continue;
        }

        if (*src == '\n') {
            dst[o++] = '\n';
            src++;
            continue;
        }

        /* Everything else below space would corrupt a text-mode screen. */
        if ((unsigned char)*src < 0x20) {
            dst[o++] = ' ';
            src++;
            continue;
        }

        dst[o++] = *src++;
    }

    dst[o] = '\0';
    return o;
}

int ansi_has_codes(const char *src)
{
    for (; *src; src++) {
        if (*src == 27)
            return 1;
        if (src[0] == '@' && (src[1] == 'X' || src[1] == 'x') &&
            hexval(src[2]) >= 0 && hexval(src[3]) >= 0)
            return 1;
    }
    return 0;
}

/* Renderer state. */
typedef struct {
    int x, y;                /* cursor, relative to the rectangle */
    int w, h;
    unsigned char attr;
    unsigned char base;
    int fg, bg, bold, blink, reverse;
    int max_row;             /* highest row written, for scrolling */
} rstate;

static void apply_sgr(rstate *r)
{
    int fg = r->fg;
    int bg = r->bg;

    if (r->bold)
        fg |= 8;
    if (r->reverse) {
        int t = fg & 7;
        fg = (bg & 7) | (fg & 8);
        bg = t;
    }

    r->attr = ATTR(fg, bg);
    if (r->blink)
        r->attr |= ATTR_BLINK;
}

static void reset_sgr(rstate *r)
{
    r->fg = ATTR_FG(r->base) & 7;
    r->bg = ATTR_BG(r->base);
    r->bold = (ATTR_FG(r->base) & 8) ? 1 : 0;
    r->blink = 0;
    r->reverse = 0;
    apply_sgr(r);
}

static void put(screen *s, rstate *r, int ox, int oy, int scroll,
                unsigned char ch)
{
    int row = r->y - scroll;

    if (r->y > r->max_row)
        r->max_row = r->y;

    if (row >= 0 && row < r->h && r->x >= 0 && r->x < r->w)
        screen_putc(s, ox + r->x, oy + row, ch, r->attr);

    r->x++;
    if (r->x >= r->w) {       /* wrap: art wider than the box */
        r->x = 0;
        r->y++;
    }
}

int ansi_draw(screen *s, int x, int y, int w, int h,
              unsigned char base_attr, const char *src, int scroll)
{
    rstate r;
    const char *p = src;

    memset(&r, 0, sizeof r);
    r.w = w;
    r.h = h;
    r.base = base_attr;
    reset_sgr(&r);

    screen_fill(s, x, y, w, h, ' ', base_attr);

    while (*p) {
        if (*p == 27 && p[1] == '[') {
            /* Parse parameters, then dispatch on the final byte. */
            int params[8];
            int nparam = 0;
            const char *q = p + 2;
            int val = 0;
            int have = 0;

            while (*q && (unsigned char)*q >= 0x20 &&
                   (unsigned char)*q <= 0x3F) {
                if (*q >= '0' && *q <= '9') {
                    val = val * 10 + (*q - '0');
                    have = 1;
                } else if (*q == ';') {
                    if (nparam < 8)
                        params[nparam++] = have ? val : 0;
                    val = 0;
                    have = 0;
                }
                q++;
            }
            if (have && nparam < 8)
                params[nparam++] = val;

            switch (*q) {
            case 'm': {
                int i;
                if (nparam == 0)
                    reset_sgr(&r);
                for (i = 0; i < nparam; i++) {
                    int c = params[i];
                    if (c == 0)                reset_sgr(&r);
                    else if (c == 1)           r.bold = 1;
                    else if (c == 2 || c == 22) r.bold = 0;
                    else if (c == 5)           r.blink = 1;
                    else if (c == 25)          r.blink = 0;
                    else if (c == 7)           r.reverse = 1;
                    else if (c == 27)          r.reverse = 0;
                    else if (c >= 30 && c <= 37) r.fg = ANSI_TO_VGA[c - 30];
                    else if (c >= 40 && c <= 47) r.bg = ANSI_TO_VGA[c - 40];
                    else if (c >= 90 && c <= 97) {
                        r.fg = ANSI_TO_VGA[c - 90];
                        r.bold = 1;
                    }
                    apply_sgr(&r);
                }
                break;
            }

            /* Cursor movement is honoured but CLAMPED to the rectangle.
               This is the whole point: real ANSI art positions its cursor,
               and a hostile MOTD would otherwise walk out of the box and
               overwrite the rest of the interface. */
            case 'H':
            case 'f': {
                int row = nparam > 0 ? params[0] - 1 : 0;
                int col = nparam > 1 ? params[1] - 1 : 0;
                r.y = row < 0 ? 0 : row;
                r.x = col < 0 ? 0 : (col >= w ? w - 1 : col);
                break;
            }
            case 'A': r.y -= nparam ? params[0] : 1; if (r.y < 0) r.y = 0; break;
            case 'B': r.y += nparam ? params[0] : 1; break;
            case 'C': r.x += nparam ? params[0] : 1;
                      if (r.x >= w) r.x = w - 1; break;
            case 'D': r.x -= nparam ? params[0] : 1;
                      if (r.x < 0) r.x = 0; break;

            case 'J':
                /* Clear: only ever our own rectangle, never the screen. */
                screen_fill(s, x, y, w, h, ' ', r.attr);
                if (nparam > 0 && params[0] == 2) {
                    r.x = 0;
                    r.y = 0;
                }
                break;

            case 'K': {
                int col;
                int row = r.y - scroll;
                if (row >= 0 && row < h)
                    for (col = r.x; col < w; col++)
                        screen_putc(s, x + col, y + row, ' ', r.attr);
                break;
            }

            /* Everything else -- scroll regions, device queries, mode
               changes, save/restore cursor -- is dropped. A hub has no
               business driving those, and a query would make us reply. */
            default:
                break;
            }

            p = q + (*q ? 1 : 0);
            continue;
        }

        if (*p == 27) {
            p += skip_escape(p);
            continue;
        }

        /* PCBoard @X<bg><fg>. */
        if (p[0] == '@' && (p[1] == 'X' || p[1] == 'x') &&
            hexval(p[2]) >= 0 && hexval(p[3]) >= 0) {
            r.bg = hexval(p[2]) & 7;
            r.fg = hexval(p[3]);
            r.bold = (r.fg & 8) ? 1 : 0;
            r.fg &= 7;
            r.blink = (hexval(p[2]) & 8) ? 1 : 0;
            apply_sgr(&r);
            p += 4;
            continue;
        }
        if (p[0] == '@' && p[1] == '@') {
            put(s, &r, x, y, scroll, '@');
            p += 2;
            continue;
        }

        if (*p == '\n') {
            r.x = 0;
            r.y++;
            if (r.y > r.max_row)
                r.max_row = r.y;
            p++;
            continue;
        }
        if (*p == '\r') {
            r.x = 0;
            p++;
            continue;
        }
        if ((unsigned char)*p < 0x20) {
            /* A bare control byte in art is noise; drop rather than draw a
               glyph the author did not intend. */
            p++;
            continue;
        }

        put(s, &r, x, y, scroll, (unsigned char)*p);
        p++;
    }

    return r.max_row + 1;
}
