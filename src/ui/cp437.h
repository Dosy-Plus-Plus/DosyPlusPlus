/* cp437.h - text coming from a hub is UTF-8; the screen is CP437.
 *
 * Hub nicks, descriptions and filenames are UTF-8 by specification. The
 * display is a 1994 IBM code page. Converting is display-only: the original
 * bytes are kept for protocol use, because a filename that has been
 * transliterated is no longer the filename the peer will serve.
 *
 * Accented Latin that CP437 lacks falls back to the base letter -- a nick
 * reading "Bjork" is more use than "Bj?rk". Anything genuinely unmappable
 * (CJK, emoji) becomes '?'.
 */
#ifndef DOSYPP_CP437_H
#define DOSYPP_CP437_H

#include <stddef.h>

/* Flags for cp437_from_utf8_ex. */
#define CP437_KEEP_ESC 0x01   /* keep 0x1B so ansi_strip can see sequences */
#define CP437_KEEP_NL  0x02   /* keep 0x0A as a line break */

/* Convert UTF-8 to CP437. Returns the number of bytes written, excluding the
   NUL. Always NUL-terminates when dstsz > 0; truncates rather than failing,
   since this is only for display.
 *
 * By default every control byte becomes a space. That is the safe default,
 * but it must NOT be used before ansi_strip(): flattening the ESC first
 * leaves the rest of the sequence behind as visible junk like "[1;36m",
 * and collapses the newlines that separate lines of art. Pass
 * CP437_KEEP_ESC | CP437_KEEP_NL when stripping or rendering follows. */
size_t cp437_from_utf8(char *dst, size_t dstsz, const char *src);
size_t cp437_from_utf8_ex(char *dst, size_t dstsz, const char *src,
                          unsigned flags);

/* One CP437 byte as UTF-8, for the host backend. Writes up to 3 bytes plus a
   NUL and returns the length. */
size_t cp437_to_utf8(unsigned char ch, char *out);

/* The Unicode code point a CP437 byte displays as. */
unsigned short cp437_to_ucs(unsigned char ch);

/* Strip control characters that would corrupt a text-mode screen, in place:
   anything below 0x20 becomes a space. Hub text has no business carrying a
   bare CR, a BEL or an ESC -- and an ESC is the start of an attack on the
   display (see ansi.c). */
void cp437_sanitize(char *s);

#endif /* DOSYPP_CP437_H */
