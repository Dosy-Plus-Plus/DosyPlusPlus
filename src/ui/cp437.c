/* cp437.c - UTF-8 <-> CP437 for display. */
#include "cp437.h"

#include <string.h>

#include "cp437_table.h"

/* Decode one UTF-8 sequence. Returns the code point and advances *src.
   Invalid bytes yield U+FFFD and consume one byte, so a malformed string
   cannot cause an infinite loop or walk off the end. */
static unsigned long utf8_next(const char **src)
{
    const unsigned char *p = (const unsigned char *)*src;
    unsigned long cp;
    int extra, i;

    if (p[0] < 0x80) {
        *src += 1;
        return p[0];
    }

    if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; extra = 1; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; extra = 2; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; extra = 3; }
    else { *src += 1; return 0xFFFD; }

    for (i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            /* Truncated or invalid: consume what we have looked at. */
            *src += i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (unsigned long)(p[i] & 0x3F);
    }

    *src += extra + 1;
    return cp;
}

static int rev_lookup(unsigned long cp, unsigned char *out)
{
    size_t lo = 0, hi = CP437_FROM_UCS_COUNT;

    if (cp > 0xFFFF)
        return 0;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        unsigned short v = CP437_FROM_UCS[mid].ucs;

        if (v == (unsigned short)cp) {
            *out = CP437_FROM_UCS[mid].cp437;
            return 1;
        }
        if (v < (unsigned short)cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

size_t cp437_from_utf8(char *dst, size_t dstsz, const char *src)
{
    return cp437_from_utf8_ex(dst, dstsz, src, 0);
}

size_t cp437_from_utf8_ex(char *dst, size_t dstsz, const char *src,
                          unsigned flags)
{
    size_t o = 0;

    if (dstsz == 0)
        return 0;

    while (*src && o + 1 < dstsz) {
        unsigned long cp = utf8_next(&src);
        unsigned char b;

        if (cp == 0x1B && (flags & CP437_KEEP_ESC)) {
            b = 0x1B;
        } else if (cp == '\n' && (flags & CP437_KEEP_NL)) {
            b = '\n';
        } else if (cp < 0x20) {
            /* Control characters never reach the screen. */
            b = ' ';
        } else if (cp < 0x7F) {
            b = (unsigned char)cp;
        } else if (rev_lookup(cp, &b)) {
            if (b == 0)
                continue;              /* table says drop it (e.g. U+200B) */
        } else {
            b = '?';
        }

        dst[o++] = (char)b;
    }

    dst[o] = '\0';
    return o;
}

unsigned short cp437_to_ucs(unsigned char ch)
{
    return CP437_TO_UCS[ch];
}

size_t cp437_to_utf8(unsigned char ch, char *out)
{
    unsigned short cp = CP437_TO_UCS[ch];

    if (cp < 0x80) {
        out[0] = (char)cp;
        out[1] = '\0';
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = '\0';
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    out[3] = '\0';
    return 3;
}

void cp437_sanitize(char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20)
            *s = ' ';
}
