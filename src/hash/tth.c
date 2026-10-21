/* tth.c - Tiger Tree Hash over a stack of pending nodes.
 *
 * The stack holds at most one node per level, smallest level on top. When
 * two nodes share a level they combine into one at the next level up, which
 * keeps memory at O(log n) and never needs the whole tree in RAM.
 */
#include "tth.h"
#include "base32.h"

#include <string.h>

#define LEAF_PREFIX 0x00
#define NODE_PREFIX 0x01

static void combine(const unsigned char *left, const unsigned char *right,
                    unsigned char *out)
{
    tiger_ctx t;
    unsigned char prefix = NODE_PREFIX;

    tiger_init(&t);
    tiger_update(&t, &prefix, 1);
    tiger_update(&t, left, TTH_HASH_LEN);
    tiger_update(&t, right, TTH_HASH_LEN);
    tiger_final(&t, out);
}

/* Push a completed node at `level`, then collapse equal-level pairs. */
static void push(tth_ctx *ctx, const unsigned char *hash, int level)
{
    if (level == ctx->emit_level && ctx->on_block)
        ctx->on_block(ctx->user, hash);

    memcpy(ctx->stack[ctx->sp], hash, TTH_HASH_LEN);
    ctx->level[ctx->sp] = level;
    ctx->sp++;

    while (ctx->sp >= 2 &&
           ctx->level[ctx->sp - 1] == ctx->level[ctx->sp - 2]) {
        unsigned char merged[TTH_HASH_LEN];
        int newlevel = ctx->level[ctx->sp - 1] + 1;

        combine(ctx->stack[ctx->sp - 2], ctx->stack[ctx->sp - 1], merged);
        ctx->sp -= 2;

        if (newlevel == ctx->emit_level && ctx->on_block)
            ctx->on_block(ctx->user, merged);

        memcpy(ctx->stack[ctx->sp], merged, TTH_HASH_LEN);
        ctx->level[ctx->sp] = newlevel;
        ctx->sp++;
    }
}

static void start_leaf(tth_ctx *ctx)
{
    unsigned char prefix = LEAF_PREFIX;

    tiger_init(&ctx->leaf);
    tiger_update(&ctx->leaf, &prefix, 1);
    ctx->leaf_fill = 0;
    ctx->leaf_started = 1;
}

static void finish_leaf(tth_ctx *ctx)
{
    unsigned char hash[TTH_HASH_LEN];

    tiger_final(&ctx->leaf, hash);
    ctx->leaf_started = 0;
    ctx->leaf_fill = 0;
    push(ctx, hash, 0);
}

void tth_init(tth_ctx *ctx, tth_block_fn on_block, void *user, int emit_level)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->on_block = on_block;
    ctx->user = user;
    ctx->emit_level = emit_level;
}

void tth_update(tth_ctx *ctx, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;

    ctx->total += (tth_u64)len;

    while (len > 0) {
        size_t room, take;

        if (!ctx->leaf_started)
            start_leaf(ctx);

        room = TTH_LEAF_SIZE - ctx->leaf_fill;
        take = (len < room) ? len : room;

        tiger_update(&ctx->leaf, p, take);
        ctx->leaf_fill += take;
        p += take;
        len -= take;

        /* Only close the leaf once it is full. A leaf that happens to end
           exactly on the buffer boundary must stay open in case more data
           follows -- closing it here would split one leaf into two and
           change the root. */
        if (ctx->leaf_fill == TTH_LEAF_SIZE)
            finish_leaf(ctx);
    }
}

void tth_final(tth_ctx *ctx, unsigned char *root)
{
    /* A trailing partial leaf, or the single empty leaf of an empty file. */
    if (ctx->leaf_started)
        finish_leaf(ctx);
    else if (ctx->total == 0) {
        start_leaf(ctx);
        finish_leaf(ctx);
    }

    /* The last node of the emitted level, which is usually a short one.
     *
     * Nodes are emitted as they complete, so a file whose size is not a
     * multiple of the level's span leaves a tail: a few leaves that never
     * pair up to the emitted level and so were never reported. The leaf
     * set was therefore one node short of describing the file, which the
     * receiving side correctly refuses -- "the set does not describe this
     * file" -- and which cost a 19MB download that reported itself
     * complete without moving a byte.
     *
     * The tail is every stack entry below the emitted level, folded into
     * one hash: exactly what a node at that level would be if the file
     * continued with zero bytes, which is how every other client writes
     * it. */
    if (ctx->emit_level > 0 && ctx->on_block && ctx->sp > 0) {
        int top = ctx->sp - 1;

        if (ctx->level[top] < ctx->emit_level) {
            unsigned char tail[TTH_HASH_LEN];
            int at = top;

            memcpy(tail, ctx->stack[at], TTH_HASH_LEN);
            while (at > 0 && ctx->level[at - 1] < ctx->emit_level) {
                unsigned char merged[TTH_HASH_LEN];

                combine(ctx->stack[at - 1], tail, merged);
                memcpy(tail, merged, TTH_HASH_LEN);
                at--;
            }
            ctx->on_block(ctx->user, tail);
        }
    }

    /* Fold what remains. Levels on the stack are strictly decreasing from
       bottom to top, so combining from the top up promotes each unpaired
       node without duplicating it. */
    while (ctx->sp > 1) {
        unsigned char merged[TTH_HASH_LEN];

        combine(ctx->stack[ctx->sp - 2], ctx->stack[ctx->sp - 1], merged);
        ctx->sp--;
        memcpy(ctx->stack[ctx->sp - 1], merged, TTH_HASH_LEN);
        ctx->level[ctx->sp - 1]++;
    }

    memcpy(root, ctx->stack[0], TTH_HASH_LEN);
}

void tth(const void *data, size_t len, unsigned char *root)
{
    tth_ctx ctx;

    tth_init(&ctx, NULL, NULL, -1);
    tth_update(&ctx, data, len);
    tth_final(&ctx, root);
}

void tth_to_base32(const unsigned char *root, char *out)
{
    base32_encode(out, BASE32_TIGER_LEN + 1, root, TTH_HASH_LEN);
}
