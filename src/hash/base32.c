/* base32.c - RFC 4648 base32, unpadded. */
#include "base32.h"

static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

int base32_encode(char *dst, size_t dstsz, const void *src, size_t len)
{
    const unsigned char *p = (const unsigned char *)src;
    size_t need = BASE32_ENC_LEN(len);
    unsigned long bits = 0;
    int nbits = 0;
    size_t o = 0;
    size_t i;

    if (dstsz < need + 1)
        return -1;

    for (i = 0; i < len; i++) {
        bits = (bits << 8) | p[i];
        nbits += 8;
        while (nbits >= 5) {
            nbits -= 5;
            dst[o++] = ALPHABET[(bits >> nbits) & 0x1F];
        }
    }

    /* Trailing bits are left-aligned and zero-padded, per RFC 4648. */
    if (nbits > 0)
        dst[o++] = ALPHABET[(bits << (5 - nbits)) & 0x1F];

    dst[o] = '\0';
    return (int)o;
}

static int decode_char(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a';
    if (c >= '2' && c <= '7')
        return c - '2' + 26;
    return -1;
}

int base32_decode(void *dst, size_t dstsz, const char *src)
{
    unsigned char *out = (unsigned char *)dst;
    unsigned long bits = 0;
    int nbits = 0;
    size_t o = 0;

    for (; *src; src++) {
        int v;

        if (*src == '=')
            break;              /* padding: nothing meaningful follows */

        v = decode_char(*src);
        if (v < 0)
            return -1;

        bits = (bits << 5) | (unsigned long)v;
        nbits += 5;

        if (nbits >= 8) {
            nbits -= 8;
            if (o >= dstsz)
                return -1;
            out[o++] = (unsigned char)((bits >> nbits) & 0xFF);
        }
    }

    /* Leftover bits must be zero. Anything else means two different strings
       would decode to the same bytes, and we would accept a CID that no
       conforming encoder could have produced. */
    if (nbits > 0 && (bits & ((1UL << nbits) - 1)) != 0)
        return -1;

    return (int)o;
}
