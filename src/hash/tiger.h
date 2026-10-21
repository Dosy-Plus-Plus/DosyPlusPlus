/* tiger.h - Tiger-192 (3 passes), the hash everything in ADC rests on.
 *
 * Used for three separate jobs, all of which must be byte-exact or nothing
 * works:
 *   - CID derivation:  CID = base32(tiger(PID))
 *   - password auth:   HPAS = base32(tiger(password || challenge))
 *                      (the UTF-8 password bytes, then the DECODED random
 *                      bytes from IGPA -- not the PID; see src/adc/ident.c)
 *   - TTH:             leaf and node hashes of the Merkle tree
 *
 * This is the Tiger1 variant (0x01 padding byte), which is what DC++ and
 * every ADC hub use. Tiger2 differs only in padding (0x80) and is NOT
 * interchangeable.
 *
 * Pure C with no DOS dependency, so it unit-tests natively -- see
 * test/host/test_tiger.c. Do not debug this inside an emulator.
 */
#ifndef DOSYPP_TIGER_H
#define DOSYPP_TIGER_H

#include <stddef.h>

/* DJGPP's GCC is built as gnu89, where `long long` is a GNU extension rather
   than standard. It is still 64-bit there, which is all we need. */
typedef unsigned long long tiger_u64;

#define TIGER_DIGEST_LEN 24   /* 192 bits */
#define TIGER_BLOCK_LEN  64

typedef struct {
    tiger_u64     state[3];                  /* a, b, c */
    tiger_u64     length;                     /* bytes consumed so far */
    unsigned char buf[TIGER_BLOCK_LEN];       /* partial block */
    size_t        buflen;
} tiger_ctx;

void tiger_init(tiger_ctx *ctx);
void tiger_update(tiger_ctx *ctx, const void *data, size_t len);

/* Writes TIGER_DIGEST_LEN bytes. Each of the three 64-bit words is
   serialised little-endian, which is the ordering DC++ and rhash print. */
void tiger_final(tiger_ctx *ctx, unsigned char *digest);

/* One-shot convenience. */
void tiger(const void *data, size_t len, unsigned char *digest);

#endif /* DOSYPP_TIGER_H */
