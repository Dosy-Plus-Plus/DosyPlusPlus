/* ansi.h - ANSI and PCBoard colour codes from hub text.
 *
 * Hubs send art. A welcome screen is frequently ANSI, and older hubs use
 * PCBoard's @X codes. Two very different jobs follow from that:
 *
 *   ansi_strip()  Remove every escape sequence and @X code, keeping the
 *                 text. Used on ALL hub text before it goes anywhere near
 *                 the screen. This is a security boundary, not a nicety: a
 *                 raw ESC reaching a text-mode screen lets a hub move our
 *                 cursor, clear our display, or on a real terminal set the
 *                 keyboard's answerback string. Nothing from the network
 *                 renders without passing through here first.
 *
 *   ansi_draw()   Deliberately interpret the codes, inside a bounded
 *                 rectangle, for the welcome door. Cursor movement is
 *                 clamped to the rectangle and screen-clear only clears
 *                 that rectangle, so even a hostile MOTD cannot scribble
 *                 over the rest of the UI.
 *
 * Input is CP437 (run cp437_from_utf8 first). Pure C, no DOS, unit tested.
 */
#ifndef DOSYPP_ANSI_H
#define DOSYPP_ANSI_H

#include <stddef.h>

#include "screen.h"

/* Copy src to dst with all escape sequences and @X codes removed.
 * Control characters other than newline become spaces. Newlines are kept:
 * callers that want a single line should split on them.
 * Returns the length written. Always NUL-terminates. */
size_t ansi_strip(char *dst, size_t dstsz, const char *src);

/* Non-zero if the text contains anything ansi_draw would interpret. Used to
 * decide whether a welcome message is worth its own door. */
int ansi_has_codes(const char *src);

/* Render into the rectangle (x, y, w, h), starting from base_attr.
 * Returns the number of rows used, which may exceed h if the art is taller
 * than the box -- the caller can use it to offer scrolling. */
int ansi_draw(screen *s, int x, int y, int w, int h,
              unsigned char base_attr, const char *src, int scroll);

#endif /* DOSYPP_ANSI_H */
