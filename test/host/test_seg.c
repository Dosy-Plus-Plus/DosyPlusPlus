/* test_seg.c - segmented downloads: the plan, not the plumbing.
 *
 * The plumbing (two peerconns moving a segment) is covered in test_peer.c.
 * What matters here is the decision-making, because every mistake in it is
 * silent:
 *
 *   - leaves that do not belong to the file must be refused, or every
 *     segment is then "verified" against hashes a stranger chose;
 *   - the span each leaf covers is derived, not told to us, and getting it
 *     wrong verifies the right bytes against the wrong hash;
 *   - a part already correct on disk must be recognised, which is what
 *     makes resume cheap;
 *   - a part that is wrong on disk must NOT be recognised, which is what
 *     makes resume safe.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/xfer/queue.h"
#include "../../src/common.h"
#include "../../src/hash/tth.h"
#include "../../src/hash/base32.h"

static int g_fail;
static int g_run;

#define CHECK(cond, ...) do {                                              \
        g_run++;                                                           \
        if (!(cond)) {                                                     \
            printf("FAIL line %d: ", __LINE__);                            \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

static char g_dir[256];
static char g_data[320];        /* the file being "downloaded" */
static char g_leaves[320];      /* the leaf hashes, as a peer sends them */
static char g_absent[320];      /* a path with nothing at it, on purpose */

/* The content a peer would be serving. */
static unsigned char byte_at(long i)
{
    return (unsigned char)((i * 37 + (i >> 9)) & 0xFF);
}

static void make_data(long size)
{
    FILE *f = fopen(g_data, "wb");
    long i;

    if (!f) {
        printf("FAIL: cannot write %s\n", g_data);
        g_fail++;
        return;
    }
    for (i = 0; i < size; i++)
        fputc(byte_at(i), f);
    fclose(f);
}

/* Leaves at a given level, computed the way a serving client would. */
static long make_leaves(long size, int level, const char *path)
{
    FILE *out = fopen(path, "wb");
    tth_u64 span = (tth_u64)TTH_LEAF_SIZE << level;
    long at = 0;
    long n = 0;

    if (!out)
        return -1;

    while (at < size) {
        tth_ctx ctx;
        unsigned char hash[TTH_HASH_LEN];
        long len = (long)span;
        long i;

        if (at + len > size)
            len = size - at;

        tth_init(&ctx, NULL, NULL, -1);
        for (i = 0; i < len; i++) {
            unsigned char b = byte_at(at + i);
            tth_update(&ctx, &b, 1);
        }
        tth_final(&ctx, hash);
        fwrite(hash, 1, TTH_HASH_LEN, out);

        at += len;
        n++;
    }

    fclose(out);
    return n;
}

static void root_of(long size, char *b32)
{
    tth_ctx ctx;
    unsigned char root[TTH_HASH_LEN];
    long i;

    tth_init(&ctx, NULL, NULL, -1);
    for (i = 0; i < size; i++) {
        unsigned char b = byte_at(i);
        tth_update(&ctx, &b, 1);
    }
    tth_final(&ctx, root);
    tth_to_base32(root, b32);
}

/* A queued request, built by hand: xq_add needs a search result and the
   plan code only cares about these four fields. */
static xq_request *queue_one(xfer_queue *q, const char *tth, tth_u64 size,
                             const char *local)
{
    xq_request *r;

    q->nreq = 1;
    r = &q->reqs[0];
    memset(r, 0, sizeof *r);
    r->plan = -1;
    snprintf(r->tth, sizeof r->tth, "%s", tth);
    snprintf(r->local, sizeof r->local, "%s", local);
    snprintf(r->name, sizeof r->name, "%s", "TEST.ZIP");
    r->size = size;
    r->status = XQ_WAITING;
    return r;
}

/* Big sizes, fed in blocks.
 *
 * The byte-at-a-time helpers above are fine for a 5KB fixture and hopeless
 * for 100MB. These exist because the bugs being hunted here are
 * size-dependent: a leaf set one node short, a span chosen wrongly, a
 * multiply that overflows. None of them show at 5KB. */
static void feed_blocks(tth_ctx *ctx, tth_u64 from, tth_u64 len)
{
    static unsigned char buf[32768];
    tth_u64 at = from;

    while (len > 0) {
        size_t n = sizeof buf;
        size_t i;

        if ((tth_u64)n > len)
            n = (size_t)len;
        for (i = 0; i < n; i++)
            buf[i] = byte_at(at + i);
        tth_update(ctx, buf, n);
        at += n;
        len -= n;
    }
}

static long make_leaves_fast(tth_u64 size, int level, const char *path)
{
    FILE *out = fopen(path, "wb");
    tth_u64 span = (tth_u64)TTH_LEAF_SIZE << level;
    tth_u64 at = 0;
    long n = 0;

    if (!out)
        return -1;

    while (at < size) {
        tth_ctx ctx;
        unsigned char hash[TTH_HASH_LEN];
        tth_u64 len = span;

        if (at + len > size)
            len = size - at;

        tth_init(&ctx, NULL, NULL, -1);
        feed_blocks(&ctx, at, len);
        tth_final(&ctx, hash);
        fwrite(hash, 1, TTH_HASH_LEN, out);

        at += len;
        n++;
    }

    fclose(out);
    return n;
}

static void root_of_fast(tth_u64 size, char *b32)
{
    tth_ctx ctx;
    unsigned char root[TTH_HASH_LEN];

    tth_init(&ctx, NULL, NULL, -1);
    feed_blocks(&ctx, 0, size);
    tth_final(&ctx, root);
    tth_to_base32(root, b32);
}

/* The level a serving client picks: the smallest whose node count fits in
   1024. Mirrors share_write_tthl, which is the thing being checked. */
static int level_for(tth_u64 size)
{
    int level = 0;

    while (level < 24) {
        tth_u64 span = (tth_u64)TTH_LEAF_SIZE << level;
        tth_u64 count = (size + span - 1) / span;

        if (count <= 1024)
            return level;
        level++;
    }
    return 24;
}

static void test_plan_big(void)
{
    /* 100MB is the sustained-transfer gate's size and the smallest that
       needs a level above zero. 20,000,000 is there because it is the size
       that found the short leaf set: at level 5 it needs 611 nodes and the
       emitter produced 610, so the plan was refused and the download
       reported itself complete without moving a byte. */
    static const tth_u64 SIZES[] = {
        20000000UL, 100000000UL, 1073741824UL
    };
    size_t k;

    for (k = 0; k < sizeof SIZES / sizeof SIZES[0]; k++) {
        tth_u64 size = SIZES[k];
        int level = level_for(size);
        tth_u64 span = (tth_u64)TTH_LEAF_SIZE << level;
        long want_count = (long)((size + span - 1) / span);
        char tth[BASE32_TIGER_LEN + 1];
        xfer_queue q;
        xq_request *r;
        long made, n;

        /* A gigabyte of Tiger takes a few seconds; anything larger belongs
           in a benchmark, not a test. */
        if (size > 1073741824UL)
            continue;

        printf("  %lu bytes: level %d, span %lu, %ld nodes\n",
               (unsigned long)size, level, (unsigned long)span, want_count);

        CHECK(want_count <= 1024,
              "%lu bytes needs %ld nodes, more than the 1024 a plan holds",
              (unsigned long)size, want_count);

        made = make_leaves_fast(size, level, g_leaves);
        CHECK(made == want_count,
              "built %ld leaves for %lu bytes, expected %ld",
              made, (unsigned long)size, want_count);

        root_of_fast(size, tth);

        xq_init(&q, NULL);
        /* A path with nothing at it. Pointing this at the small fixture
           file was the first version's mistake and an instructive one: the
           fixture is the same byte pattern, so segment 0 of a 20MB plan
           really was already on disk and the planner said so correctly.
           A test that cannot tell "resume works" from "resume is broken"
           is worse than no test. */
        r = queue_one(&q, tth, size, g_absent);
        n = xq_plan_load(&q, r, g_leaves);

        /* Segments are groups of leaves: the planner folds them up to
           about 64KB each, so the count is the leaf count divided by the
           group, not the leaf count. */
        if (n > 0) {
            xq_plan *pl0 = xq_plan_of(&q, r);
            long want_seg = pl0 ? (want_count + pl0->group - 1) / pl0->group
                                : want_count;

            CHECK(n == want_seg,
                  "a plan for %lu bytes gave %ld segments, expected %ld "
                  "(%ld leaves, group %ld)",
                  (unsigned long)size, n, want_seg, want_count,
                  pl0 ? pl0->group : 0);
        } else {
            CHECK(0, "a plan for %lu bytes was refused",
                  (unsigned long)size);
        }

        if (n > 0) {
            xq_plan *pl = xq_plan_of(&q, r);

            CHECK(pl != NULL, "no plan after loading one");
            if (pl) {
                CHECK(pl->span == span,
                      "the plan thinks the span is %lu, not %lu",
                      (unsigned long)pl->span, (unsigned long)span);
                CHECK(pl->nleaf == want_count,
                      "the plan has %ld leaves, expected %ld",
                      pl->nleaf, want_count);

                /* The segments have to cover the file exactly: the last
                   one ends at the last byte, not past it and not short of
                   it. That arithmetic is where a 32-bit multiply would
                   have overflowed. */
                {
                    tth_u64 covered = (tth_u64)pl->span *
                                      (tth_u64)pl->group *
                                      (tth_u64)(pl->nseg - 1);

                    CHECK(covered < size,
                          "the segments before the last already cover "
                          "%lu of %lu bytes",
                          (unsigned long)covered, (unsigned long)size);
                    CHECK((tth_u64)pl->span * (tth_u64)pl->group *
                          (tth_u64)pl->nseg >= size,
                          "the segments stop %lu bytes short",
                          (unsigned long)(size -
                              (tth_u64)pl->span * (tth_u64)pl->group *
                              (tth_u64)pl->nseg));
                }

                /* Nothing is on disk, so nothing is done. A missing file
                   read as "already complete" is the other half of the bug
                   this test exists for -- the 19MB download that reported
                   itself finished without moving a byte. */
                {
                    long done = 0, i;

                    for (i = 0; i < pl->nseg; i++)
                        if (pl->done[i]) {
                            if (done == 0)
                                printf("    segment %ld of %ld reads as "
                                       "already present (start %lu, "
                                       "len %lu)\n", i, pl->nseg,
                                       (unsigned long)((tth_u64)pl->span *
                                           pl->group * i),
                                       (unsigned long)((tth_u64)pl->span *
                                           pl->group));
                            done++;
                        }
                    CHECK(done == 0,
                          "%ld of %ld segments were marked done with no "
                          "file on disk", done, pl->nseg);
                }
            }
        }

        xq_free(&q);

        /* And the other way round: with the first segment's bytes on disk,
           at this size, the planner has to notice. Resume at 20MB is the
           case that matters -- it is the one where re-fetching everything
           costs an evening. */
        if (k == 0) {
            FILE *f = fopen(g_absent, "wb");
            xq_plan *pl;

            if (f) {
                tth_u64 at;
                static unsigned char buf[32768];

                for (at = 0; at < 65536; at += sizeof buf) {
                    size_t i;

                    for (i = 0; i < sizeof buf; i++)
                        buf[i] = byte_at(at + i);
                    fwrite(buf, 1, sizeof buf, f);
                }
                fclose(f);

                xq_init(&q, NULL);
                r = queue_one(&q, tth, size, g_absent);
                n = xq_plan_load(&q, r, g_leaves);
                pl = (n > 0) ? xq_plan_of(&q, r) : NULL;

                CHECK(pl && pl->done[0],
                      "64KB of the right bytes on disk, and the planner "
                      "wants the first segment again");
                CHECK(!pl || !pl->done[1],
                      "the second segment was claimed done from a 64KB "
                      "file");
                xq_free(&q);
                remove(g_absent);
            }
        }

        remove(g_leaves);
    }
}

/* The 2GB wall, which is a property of the target and not a choice: every
   seek in the program is a `long`, 32 bits on DJGPP, so a file at or past
   2GB cannot be addressed. The failure without a guard is not an error but
   a seek to the wrong place -- wrong bytes served, wrong bytes written,
   verified against the wrong hash. */
static void test_size_limit(void)
{
    xfer_queue q;
    search_result r;

    CHECK(DOSYPP_MAX_FILE == 2147483647UL,
          "the limit is %lu, expected 2GB-1", (unsigned long)DOSYPP_MAX_FILE);

    xq_init(&q, NULL);

    memset(&r, 0, sizeof r);
    snprintf(r.name, sizeof r.name, "%s", "HUGE.ISO");
    snprintf(r.tth, sizeof r.tth,
             "%s", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    r.nsource = 1;

    /* Just inside: queued. */
    r.size = (tth_u64)DOSYPP_MAX_FILE;
    CHECK(xq_add(&q, &r, ".") != NULL,
          "a file of exactly 2GB-1 was refused");

    /* Just outside: refused, rather than queued and silently wrong. */
    q.nreq = 0;
    r.size = (tth_u64)DOSYPP_MAX_FILE + 1;
    CHECK(xq_add(&q, &r, ".") == NULL,
          "a 2GB file was queued, and every seek past 2GB truncates");

    q.nreq = 0;
    r.size = 4294967296UL;              /* 4GB */
    CHECK(xq_add(&q, &r, ".") == NULL, "a 4GB file was queued");

    xq_free(&q);
}

static void test_plan(void)
{
    xfer_queue q;
    xq_request *r;
    xq_plan *pl;
    char tth[BASE32_TIGER_LEN + 1];
    const long size = 70000;        /* 69 leaves at level 0 */
    long n;

    CHECK(xq_init(&q, NULL) == 0, "xq_init failed");

    make_data(size);
    root_of(size, tth);
    n = make_leaves(size, 0, g_leaves);
    CHECK(n == 69, "made %ld leaves for 70000 bytes, expected 69", n);

    /* Nothing downloaded yet: the plan has every part to fetch. The local
       path deliberately does not exist. */
    {
        char nothing[320];

        snprintf(nothing, sizeof nothing, "%s/not_downloaded.bin", g_dir);
        remove(nothing);
        r = queue_one(&q, tth, (tth_u64)size, nothing);
    }

    n = xq_plan_load(&q, r, g_leaves);
    /* 69 leaves of 1KB grouped into 64KB segments: 64 + 5. */
    CHECK(n == 2, "plan has %ld segments, expected 2", n);

    pl = xq_plan_of(&q, r);
    CHECK(pl != NULL, "no plan after loading one");
    if (!pl) { xq_free(&q); return; }

    CHECK(pl->span == TTH_LEAF_SIZE,
          "span is %lu, expected %d", (unsigned long)pl->span,
          TTH_LEAF_SIZE);
    CHECK(pl->nleaf == 69, "plan kept %ld leaves, expected 69", pl->nleaf);
    CHECK(pl->group == 64, "group is %ld leaves, expected 64", pl->group);
    CHECK(xq_plan_done_bytes(&q, r) == 0,
          "something is done before anything was downloaded");

    /* Handing parts out: each goes to one connection at a time. */
    {
        long first = -1, second = -1;
        long i;

        pl->busy[0] = 1;
        for (i = 0; i < pl->nseg; i++)
            if (!pl->done[i] && !pl->busy[i]) { first = i; break; }
        CHECK(first == 1, "the next free part is %ld, expected 1", first);

        /* Marking one done moves progress by exactly its span. */
        CHECK(xq_segment_done(&q, r, 0) == 0,
              "one of two parts reported the download as finished");
        CHECK(xq_plan_done_bytes(&q, r) == 64 * TTH_LEAF_SIZE,
              "done bytes is %lu after one 64KB part",
              (unsigned long)xq_plan_done_bytes(&q, r));

        /* A failed part goes back in the pool rather than being lost. */
        pl->busy[1] = 1;
        xq_segment_failed(&q, r, 1);
        CHECK(pl->busy[1] == 0, "a failed part is still marked busy");
        CHECK(pl->done[1] == 0, "a failed part was marked done");

        second = 0;
        for (i = 0; i < pl->nseg; i++)
            if (pl->done[i])
                second++;
        CHECK(second == 1, "%ld parts are done, expected 1", second);
    }

    /* The last part is short, and its span must be clamped to the file. */
    {
        tth_u64 total = 0;
        long i;

        for (i = 0; i < pl->nseg; i++)
            pl->done[i] = 1;
        total = xq_plan_done_bytes(&q, r);
        CHECK(total == (tth_u64)size,
              "a fully done plan accounts for %lu bytes, expected %ld",
              (unsigned long)total, size);
    }

    xq_free(&q);
}

static void test_plan_refuses_wrong_leaves(void)
{
    xfer_queue q;
    xq_request *r;
    char tth[BASE32_TIGER_LEN + 1];
    const long size = 40000;
    long n;

    CHECK(xq_init(&q, NULL) == 0, "xq_init failed");
    make_data(size);
    root_of(size, tth);
    make_leaves(size, 0, g_leaves);

    /* One leaf corrupted. The set no longer rebuilds the root, so it must
       be refused outright -- otherwise 39 of 40 parts would be checked
       against the real hashes and one against a stranger's. */
    {
        FILE *f = fopen(g_leaves, "r+b");
        unsigned char b;

        CHECK(f != NULL, "cannot reopen the leaves");
        if (f) {
            fseek(f, 5 * TTH_HASH_LEN, SEEK_SET);
            if (fread(&b, 1, 1, f) == 1) {
                b ^= 0xFF;
                fseek(f, 5 * TTH_HASH_LEN, SEEK_SET);
                fwrite(&b, 1, 1, f);
            }
            fclose(f);
        }

        r = queue_one(&q, tth, (tth_u64)size, g_data);
        n = xq_plan_load(&q, r, g_leaves);
        CHECK(n == 0, "accepted leaves that do not rebuild the root (%ld)",
              n);
        CHECK(xq_plan_of(&q, r) == NULL,
              "a plan was built from leaves that do not match");
    }

    /* Leaves for a different file entirely. */
    make_leaves(size, 0, g_leaves);
    {
        char other[BASE32_TIGER_LEN + 1];

        make_data(size + 1024);
        root_of(size + 1024, other);

        r = queue_one(&q, other, (tth_u64)(size + 1024), g_data);
        n = xq_plan_load(&q, r, g_leaves);
        CHECK(n == 0, "accepted another file's leaves (%ld)", n);
    }

    /* A truncated leaf set: not a whole number of hashes. */
    make_data(size);
    make_leaves(size, 0, g_leaves);
    {
        FILE *f = fopen(g_leaves, "ab");

        if (f) { fputc(0x42, f); fclose(f); }
        r = queue_one(&q, tth, (tth_u64)size, g_data);
        CHECK(xq_plan_load(&q, r, g_leaves) == 0,
              "accepted a leaf set that is not a whole number of hashes");
    }

    /* No leaves at all: fall back, not fail. */
    remove(g_leaves);
    r = queue_one(&q, tth, (tth_u64)size, g_data);
    CHECK(xq_plan_load(&q, r, g_leaves) == 0,
          "a missing leaf file should mean fall back");
    CHECK(xq_plan_of(&q, r) == NULL, "a plan from nothing");

    xq_free(&q);
}

/* The resume case, which is the main reason to have leaves at all: what is
   already right on disk is recognised, and what is wrong is not. */
static void test_plan_resume(void)
{
    xfer_queue q;
    xq_request *r;
    xq_plan *pl;
    char tth[BASE32_TIGER_LEN + 1];
    /* Four 64KB segments and a short fifth, so "some parts done" is a
       state the plan can actually be in. */
    const long size = 4 * 65536 + 5000;
    long i;

    CHECK(xq_init(&q, NULL) == 0, "xq_init failed");
    make_data(size);
    root_of(size, tth);
    make_leaves(size, 0, g_leaves);

    /* A partial file: the first four leaves' worth, correct. */
    {
        FILE *f = fopen(g_data, "rb");
        FILE *w;
        char part[320];
        unsigned char buf[4096];
        long left = 2 * 65536;          /* the first two segments */

        snprintf(part, sizeof part, "%s/seg_part.bin", g_dir);
        w = fopen(part, "wb");
        CHECK(f && w, "cannot build the partial file");
        if (!f || !w) {
            if (f) fclose(f);
            if (w) fclose(w);
            xq_free(&q);
            return;
        }
        while (left > 0) {
            size_t want = (size_t)(left < (long)sizeof buf ? left
                                                           : (long)sizeof buf);
            size_t n = fread(buf, 1, want, f);

            if (n == 0) break;
            fwrite(buf, 1, n, w);
            left -= (long)n;
        }
        fclose(f);
        fclose(w);

        r = queue_one(&q, tth, (tth_u64)size, part);
        CHECK(xq_plan_load(&q, r, g_leaves) == 5, "plan not built");
        pl = xq_plan_of(&q, r);
        CHECK(pl != NULL, "no plan");
        if (!pl) { xq_free(&q); return; }

        for (i = 0; i < 2; i++)
            CHECK(pl->done[i] == 1,
                  "part %ld is on disk and correct but was not recognised",
                  i);
        for (i = 2; i < 5; i++)
            CHECK(pl->done[i] == 0,
                  "part %ld is not on disk but was marked done", i);
        CHECK(xq_plan_done_bytes(&q, r) == 2 * 65536,
              "resume accounted for %lu bytes, expected %d",
              (unsigned long)xq_plan_done_bytes(&q, r), 2 * 65536);

        /* Now corrupt the middle of part 2 and reload: that part must not
           be trusted, and the others must still be. This is the check that
           makes resume safe rather than merely fast. */
        {
            FILE *c = fopen(part, "r+b");
            unsigned char b = 0x00;

            if (c) {
                fseek(c, 65536 + 10, SEEK_SET);   /* inside part 1 */
                fwrite(&b, 1, 1, c);
                fclose(c);
            }

            r = queue_one(&q, tth, (tth_u64)size, part);
            CHECK(xq_plan_load(&q, r, g_leaves) == 5, "plan not rebuilt");
            pl = xq_plan_of(&q, r);
            if (pl) {
                CHECK(pl->done[1] == 0,
                      "a corrupted part on disk was accepted as done");
                CHECK(pl->done[0] == 1,
                      "corrupting one part lost the other");
            }
        }

        remove(part);
    }

    xq_free(&q);
}

/* Leaves at a level above 0, which is what any file over a megabyte gets:
   the span has to be worked out from the count, because ADC never says. */
static void test_plan_higher_level(void)
{
    xfer_queue q;
    xq_request *r;
    xq_plan *pl;
    char tth[BASE32_TIGER_LEN + 1];
    const long size = 100000;
    long n;

    CHECK(xq_init(&q, NULL) == 0, "xq_init failed");
    make_data(size);
    root_of(size, tth);

    /* Level 3: each node covers 8KB, so 13 of them. */
    n = make_leaves(size, 3, g_leaves);
    CHECK(n == 13, "made %ld nodes at level 3, expected 13", n);

    /* A local path that does not exist: nothing is downloaded yet. (Using
       the source file here would mark every part done, correctly -- which
       is what the resume test is for.) */
    {
        char nothing[320];

        snprintf(nothing, sizeof nothing, "%s/not_downloaded.bin", g_dir);
        remove(nothing);
        r = queue_one(&q, tth, (tth_u64)size, nothing);
    }
    n = xq_plan_load(&q, r, g_leaves);
    /* 13 nodes of 8KB, grouped 8 at a time: 2 segments. */
    CHECK(n == 2, "plan has %ld segments, expected 2", n);

    pl = xq_plan_of(&q, r);
    CHECK(pl != NULL, "no plan for level-3 leaves");
    if (pl) {
        CHECK(pl->span == 8 * 1024,
              "derived span %lu, expected 8192", (unsigned long)pl->span);
        CHECK(pl->group == 8, "group is %ld, expected 8", pl->group);
        CHECK(xq_plan_done_bytes(&q, r) == 0, "nothing should be done");
    }

    xq_free(&q);
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    char cmd[512];

    printf("DOSy++ segment tests (plans / leaf verification / resume)\n");

    snprintf(g_dir, sizeof g_dir, "%s/dosypp_seg_test",
             tmp && tmp[0] ? tmp : "/tmp");
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (system(cmd) != 0) {
        printf("FAILED: cannot create %s\n", g_dir);
        return 1;
    }
    snprintf(g_data, sizeof g_data, "%s/data.bin", g_dir);
    snprintf(g_leaves, sizeof g_leaves, "%s/leaves.bin", g_dir);
    snprintf(g_absent, sizeof g_absent, "%s/nothing-here.bin", g_dir);

    test_plan();
    test_plan_big();
    test_size_limit();
    test_plan_refuses_wrong_leaves();
    test_plan_resume();
    test_plan_higher_level();

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    (void)system(cmd);

    if (g_fail)
        printf("FAILED: %d of %d checks\n", g_fail, g_run);
    else
        printf("ok: %d checks, 0 failed\n", g_run);
    return g_fail ? 1 : 0;
}
