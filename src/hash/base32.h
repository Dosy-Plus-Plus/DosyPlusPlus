/* base32.h - RFC 4648 base32, the encoding ADC uses for every hash it names.
 *
 * ADC uses it unpadded and uppercase: a 24-byte Tiger digest becomes 39
 * characters (192 bits / 5 = 38.4, rounded up). CIDs, PIDs, TTH roots and
 * SIDs are all base32.
 *
 * No DOS dependency; unit-tested natively.
 */
#ifndef DOSYPP_BASE32_H
#define DOSYPP_BASE32_H

#include <stddef.h>

/* Encoded length, excluding the NUL terminator. */
#define BASE32_ENC_LEN(nbytes) (((nbytes) * 8 + 4) / 5)

/* A base32 Tiger digest: 39 characters plus NUL. */
#define BASE32_TIGER_LEN 39

/* Encode len bytes into dst (uppercase, unpadded, NUL-terminated).
   Returns the number of characters written, or -1 if dstsz is too small. */
int base32_encode(char *dst, size_t dstsz, const void *src, size_t len);

/* Decode a base32 string into dst. Accepts upper or lower case and ignores
   '=' padding. Returns the number of bytes written, or -1 on an invalid
   character or if dstsz is too small.

   Note: a string whose bit length is not a multiple of 8 carries leftover
   bits. Those must be zero -- a non-zero remainder means the string was not
   produced by a conforming encoder, and we reject it rather than silently
   accept two spellings of the same value. */
int base32_decode(void *dst, size_t dstsz, const char *src);

#endif /* DOSYPP_BASE32_H */
