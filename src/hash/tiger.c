/* tiger.c - Tiger-192, 3 passes. Written from the specification
 * (Anderson & Biham, "Tiger: A Fast New Hash Function").
 *
 * Structure, per block:
 *   save a,b,c -> pass(mul=5) -> key_schedule -> pass(mul=7)
 *              -> key_schedule -> pass(mul=9) -> feedforward
 *
 * Everything is little-endian: the 64-byte block is read as eight 64-bit
 * little-endian words, and the digest words are written back the same way.
 * The loads and stores are done byte-at-a-time rather than by casting, so
 * the code is endian-independent -- it must give identical results on the
 * host (arm64) and on DOS (i386), because that equality is the whole point
 * of testing it natively.
 */
#include "tiger.h"

#include <string.h>

#include "tiger_sbox.h"   /* defines tiger_sbox[4][256]; included once */

#define T1(i) tiger_sbox[0][i]
#define T2(i) tiger_sbox[1][i]
#define T3(i) tiger_sbox[2][i]
#define T4(i) tiger_sbox[3][i]

/* Byte i (0 = least significant) of a 64-bit word. */
#define BYTE_OF(x, i) ((unsigned char)(((x) >> (8 * (i))) & 0xFF))

#define ROUND(a, b, c, x, mul)                                              \
    do {                                                                    \
        (c) ^= (x);                                                         \
        (a) -= T1(BYTE_OF(c, 0)) ^ T2(BYTE_OF(c, 2)) ^                      \
               T3(BYTE_OF(c, 4)) ^ T4(BYTE_OF(c, 6));                       \
        (b) += T4(BYTE_OF(c, 1)) ^ T3(BYTE_OF(c, 3)) ^                      \
               T2(BYTE_OF(c, 5)) ^ T1(BYTE_OF(c, 7));                       \
        (b) *= (tiger_u64)(mul);                                            \
    } while (0)

#define PASS(a, b, c, mul)                                                  \
    do {                                                                    \
        ROUND(a, b, c, x[0], mul);                                           \
        ROUND(b, c, a, x[1], mul);                                           \
        ROUND(c, a, b, x[2], mul);                                           \
        ROUND(a, b, c, x[3], mul);                                           \
        ROUND(b, c, a, x[4], mul);                                           \
        ROUND(c, a, b, x[5], mul);                                           \
        ROUND(a, b, c, x[6], mul);                                           \
        ROUND(b, c, a, x[7], mul);                                           \
    } while (0)

#define KEY_SCHEDULE()                                                      \
    do {                                                                    \
        x[0] -= x[7] ^ 0xA5A5A5A5A5A5A5A5ULL;                               \
        x[1] ^= x[0];                                                       \
        x[2] += x[1];                                                       \
        x[3] -= x[2] ^ ((~x[1]) << 19);                                     \
        x[4] ^= x[3];                                                       \
        x[5] += x[4];                                                       \
        x[6] -= x[5] ^ ((~x[4]) >> 23);                                     \
        x[7] ^= x[6];                                                       \
        x[0] += x[7];                                                       \
        x[1] -= x[0] ^ ((~x[7]) << 19);                                     \
        x[2] ^= x[1];                                                       \
        x[3] += x[2];                                                       \
        x[4] -= x[3] ^ ((~x[2]) >> 23);                                     \
        x[5] ^= x[4];                                                       \
        x[6] += x[5];                                                       \
        x[7] -= x[6] ^ 0x0123456789ABCDEFULL;                               \
    } while (0)

static void tiger_compress(tiger_u64 state[3], const unsigned char *block)
{
    tiger_u64 a = state[0];
    tiger_u64 b = state[1];
    tiger_u64 c = state[2];
    tiger_u64 aa = a, bb = b, cc = c;
    tiger_u64 x[8];
    int i, j;

    /* Load eight little-endian 64-bit words, byte at a time. */
    for (i = 0; i < 8; i++) {
        tiger_u64 w = 0;
        for (j = 7; j >= 0; j--)
            w = (w << 8) | (tiger_u64)block[i * 8 + j];
        x[i] = w;
    }

    PASS(a, b, c, 5);
    KEY_SCHEDULE();
    PASS(c, a, b, 7);
    KEY_SCHEDULE();
    PASS(b, c, a, 9);

    /* Feedforward. Note the asymmetry -- xor, subtract, add -- which is
       easy to get wrong and produces a plausible-looking wrong answer. */
    state[0] = a ^ aa;
    state[1] = b - bb;
    state[2] = c + cc;
}

void tiger_init(tiger_ctx *ctx)
{
    ctx->state[0] = 0x0123456789ABCDEFULL;
    ctx->state[1] = 0xFEDCBA9876543210ULL;
    ctx->state[2] = 0xF096A5B4C3B2E187ULL;
    ctx->length = 0;
    ctx->buflen = 0;
}

void tiger_update(tiger_ctx *ctx, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;

    ctx->length += (tiger_u64)len;

    /* Top up a partial block first. */
    if (ctx->buflen > 0) {
        size_t need = TIGER_BLOCK_LEN - ctx->buflen;
        size_t take = (len < need) ? len : need;

        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p += take;
        len -= take;

        if (ctx->buflen < TIGER_BLOCK_LEN)
            return;

        tiger_compress(ctx->state, ctx->buf);
        ctx->buflen = 0;
    }

    while (len >= TIGER_BLOCK_LEN) {
        tiger_compress(ctx->state, p);
        p += TIGER_BLOCK_LEN;
        len -= TIGER_BLOCK_LEN;
    }

    if (len > 0) {
        memcpy(ctx->buf, p, len);
        ctx->buflen = len;
    }
}

void tiger_final(tiger_ctx *ctx, unsigned char *digest)
{
    tiger_u64 bits = ctx->length << 3;
    size_t i;
    int j;

    /* Tiger1 pads with 0x01, not 0x80. Tiger2 uses 0x80 and is a different
       hash; ADC wants Tiger1. */
    ctx->buf[ctx->buflen++] = 0x01;

    /* Pad to 56 mod 64, leaving room for the 8-byte length. */
    if (ctx->buflen > 56) {
        memset(ctx->buf + ctx->buflen, 0, TIGER_BLOCK_LEN - ctx->buflen);
        tiger_compress(ctx->state, ctx->buf);
        ctx->buflen = 0;
    }
    memset(ctx->buf + ctx->buflen, 0, 56 - ctx->buflen);

    /* Bit count, little-endian. */
    for (j = 0; j < 8; j++)
        ctx->buf[56 + j] = (unsigned char)((bits >> (8 * j)) & 0xFF);

    tiger_compress(ctx->state, ctx->buf);

    for (i = 0; i < 3; i++)
        for (j = 0; j < 8; j++)
            digest[i * 8 + j] =
                (unsigned char)((ctx->state[i] >> (8 * j)) & 0xFF);
}

void tiger(const void *data, size_t len, unsigned char *digest)
{
    tiger_ctx ctx;

    tiger_init(&ctx);
    tiger_update(&ctx, data, len);
    tiger_final(&ctx, digest);
}
