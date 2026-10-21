/* tth.h - Tiger Tree Hash: how ADC names files.
 *
 * A binary Merkle tree over 1024-byte leaves:
 *
 *   leaf   = tiger(0x00 || up to 1024 bytes of data)
 *   node   = tiger(0x01 || left_hash || right_hash)
 *   root   = the single hash left at the top
 *
 * Three details that are easy to get wrong, and all produce a
 * plausible-looking wrong root:
 *   - the 0x00 / 0x01 prefix bytes are what stop a leaf being confused with
 *     an internal node
 *   - an unpaired node is PROMOTED unchanged to the next level, never
 *     duplicated or hashed with itself
 *   - an empty file is one empty leaf, so its TTH is tiger(0x00) -- not the
 *     hash of nothing
 *
 * Streaming: feed data with tth_update in any chunk sizes. Memory is
 * O(log n) -- a stack of at most TTH_MAX_LEVELS hashes -- so a 2GB file
 * costs under 2KB of state.
 *
 * Block hashes (for segmented download verification, the ADC `tthl` data)
 * are delivered through a callback as they complete, rather than buffered:
 * a 2GB file has two million leaves, which at 24 bytes each would not fit
 * in RAM on the target machine. See tth_init's emit_level.
 */
#ifndef DOSYPP_TTH_H
#define DOSYPP_TTH_H

#include <stddef.h>

#include "tiger.h"
#include "base32.h"   /* tth_to_base32's output size is defined there */

#define TTH_LEAF_SIZE  1024
#define TTH_HASH_LEN   TIGER_DIGEST_LEN   /* 24 */

/* 64 levels is 2^64 leaves; the stack can never exceed this. */
#define TTH_MAX_LEVELS 64

typedef tiger_u64 tth_u64;

/* Called as each node at the requested level completes, in file order.
   hash is TTH_HASH_LEN bytes and is not retained after the call. */
typedef void (*tth_block_fn)(void *user, const unsigned char *hash);

typedef struct {
    tiger_ctx     leaf;                                  /* leaf in progress */
    size_t        leaf_fill;
    int           leaf_started;

    unsigned char stack[TTH_MAX_LEVELS][TTH_HASH_LEN];
    int           level[TTH_MAX_LEVELS];
    int           sp;

    tth_u64       total;

    tth_block_fn  on_block;
    void         *user;
    int           emit_level;
} tth_ctx;

/* emit_level selects which tree level is reported through on_block:
     0  = every 1024-byte leaf
     n  = every node covering 1024 << n bytes
     -1 = no callback
   Pick n so the block count stays bounded for large files; level 0 on a 2GB
   file means 48MB of hashes. on_block may be NULL. */
void tth_init(tth_ctx *ctx, tth_block_fn on_block, void *user, int emit_level);

void tth_update(tth_ctx *ctx, const void *data, size_t len);

/* Writes TTH_HASH_LEN bytes. Also flushes any trailing partial block to
   on_block first, so the callback sequence is complete. */
void tth_final(tth_ctx *ctx, unsigned char *root);

/* One-shot: root of a buffer. */
void tth(const void *data, size_t len, unsigned char *root);

/* Root as the 39-character base32 string ADC puts in TR fields.
   out must be at least BASE32_TIGER_LEN + 1 bytes. */
void tth_to_base32(const unsigned char *root, char *out);

#endif /* DOSYPP_TTH_H */
