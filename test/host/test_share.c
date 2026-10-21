/* test_share.c - the share index, scanner, hash queue and file list.
 *
 * The scanner and hasher touch a real directory tree, built under the
 * system temp directory and removed afterwards. That is deliberate: the
 * interesting failures here are about what the filesystem actually hands
 * back, and a mocked readdir would test the mock.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../src/share/index.h"
#include "../../src/share/scan.h"
#include "../../src/share/hashq.h"
#include "../../src/share/filelist.h"
#include "../../src/share/tthlcache.h"
#include "../../src/adc/tokenize.h"

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

#define CHECK_STR(got, want) do {                                          \
        g_run++;                                                           \
        if (!(got) || strcmp((got), (want)) != 0) {                        \
            printf("FAIL line %d: got \"%s\" want \"%s\"\n",               \
                   __LINE__, (got) ? (got) : "(null)", (want));            \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

static char g_root[256];

static void mkfile(const char *rel, int size, int seed)
{
    char path[512];
    FILE *f;
    int i;

    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    f = fopen(path, "wb");
    if (!f)
        return;
    for (i = 0; i < size; i++)
        fputc((i * seed + 7) & 0xFF, f);
    fclose(f);
}

static void mkdirp(const char *rel)
{
    char path[512];

    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    mkdir(path, 0755);
}

static void build_tree(void)
{
    snprintf(g_root, sizeof g_root, "/tmp/dosypp-test-share-%d", (int)getpid());
    mkdir(g_root, 0755);
    mkdirp("warez");
    mkdirp("warez/doom");
    mkdirp("mods");
    mkdirp("empty");

    mkfile("warez/doom/DOOM.ZIP", 5000, 31);
    mkfile("warez/PKZIP.EXE", 2048, 17);
    mkfile("mods/TUNE.MOD", 3000, 13);
    mkfile("README.TXT", 100, 7);

    /* Things that must NOT be shared. */
    mkfile(".hidden", 50, 3);
    mkfile("DOSYPP.TTH", 50, 3);
    mkfile("part.PART", 50, 3);
    mkfile("ZERO.BIN", 0, 1);
}

static void destroy_tree(void)
{
    char cmd[512];

    if (g_root[0]) {
        snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s.tth' '%s.xml'",
                 g_root, g_root, g_root);
        if (system(cmd) != 0)
            printf("note: could not remove %s\n", g_root);
    }
}

/* ---------------------------------------------------------------- index */

static void test_index(void)
{
    share_index ix;
    share_file *a, *b;
    unsigned char tth1[TTH_HASH_LEN], tth2[TTH_HASH_LEN];

    CHECK(share_init(&ix) == 0, "init failed");

    memset(tth1, 0x11, sizeof tth1);
    memset(tth2, 0x22, sizeof tth2);

    a = share_add_file(&ix, "DL/a.zip", "C:\\DL\\a.zip", 100, 1);
    b = share_add_file(&ix, "DL/sub/b.zip", "C:\\DL\\sub\\b.zip", 200, 2);
    CHECK(a && b, "add failed");
    CHECK(ix.count == 2, "count is %u", ix.count);
    CHECK(ix.total_size == 300, "total is %lu",
          (unsigned long)ix.total_size);

    /* Paths are interned; the basename is derived, not supplied. */
    CHECK_STR(share_virt(&ix, a), "DL/a.zip");
    CHECK_STR(share_local(&ix, a), "C:\\DL\\a.zip");
    CHECK_STR(share_name(&ix, a), "a.zip");
    CHECK_STR(share_name(&ix, b), "b.zip");

    /* An invalid offset must read as empty, not walk off the pool. */
    CHECK_STR(share_str(&ix, 999999), "");

    /* Nothing is hashed yet, so nothing is findable by hash -- which is
       the point: we must not claim a file we cannot address. */
    CHECK(share_find_tth(&ix, tth1) == NULL, "found an unhashed file");
    CHECK(ix.hashed_count == 0, "hashed_count is %u", ix.hashed_count);
    CHECK(share_next_unhashed(&ix) == a, "wrong first unhashed file");

    share_set_tth(&ix, a, tth1);
    CHECK(ix.hashed_count == 1, "hashed_count is %u", ix.hashed_count);
    CHECK(ix.hashed_size == 100, "hashed_size is %lu",
          (unsigned long)ix.hashed_size);
    CHECK(share_find_tth(&ix, tth1) == a, "lookup by hash failed");
    CHECK(share_find_tth(&ix, tth2) == NULL, "wrong hash matched");
    CHECK(share_next_unhashed(&ix) == b, "did not advance");

    /* Hashing the same file twice must not double-count the accounting. */
    share_set_tth(&ix, a, tth1);
    CHECK(ix.hashed_count == 1, "double-counted a rehash");
    CHECK(ix.hashed_size == 100, "double-counted the bytes");

    share_set_tth(&ix, b, tth2);
    CHECK(share_next_unhashed(&ix) == NULL, "share should be complete");

    /* base32 lookup, and rejection of anything that is not a hash. */
    {
        char b32[BASE32_TIGER_LEN + 1];
        base32_encode(b32, sizeof b32, tth1, TTH_HASH_LEN);
        CHECK(share_find_tth_b32(&ix, b32) == a, "base32 lookup failed");
        CHECK(share_find_tth_b32(&ix, "nonsense") == NULL,
              "accepted a non-hash");
        CHECK(share_find_tth_b32(&ix, NULL) == NULL, "accepted NULL");
    }

    /* Roots. */
    CHECK(share_add_root(&ix, "C:\\DL") == 0, "add_root failed");
    CHECK(share_add_root(&ix, "") == -1, "accepted an empty root");
    CHECK(share_add_root(&ix, NULL) == -1, "accepted a NULL root");
    {
        char huge[SHARE_ROOT_MAX + 10];
        memset(huge, 'x', sizeof huge - 1);
        huge[sizeof huge - 1] = '\0';
        CHECK(share_add_root(&ix, huge) == -1, "accepted an over-long root");
    }

    /* Growth: well past the initial capacity. */
    {
        int i;
        for (i = 0; i < 1000; i++) {
            char v[64];
            snprintf(v, sizeof v, "DL/bulk/f%d.bin", i);
            CHECK(share_add_file(&ix, v, v, 10, 1) != NULL,
                  "add %d failed", i);
        }
        CHECK(ix.count == 1002, "count is %u after growth", ix.count);
        /* The pool survived: offsets still resolve after 1000 inserts.
           Looked up by index rather than through `a`, because files_grow()
           reallocs ix->files and every pointer handed out before it is
           dangling afterwards -- which this check used to do, and which
           read back as "DL/a.zip" on one allocator and "" on another. */
        CHECK_STR(share_virt(&ix, &ix.files[0]), "DL/a.zip");
    }

    share_free(&ix);
}

/* ------------------------------------------------------------- matching */

static share_index *match_fixture(void)
{
    static share_index ix;
    static unsigned char t[8][TTH_HASH_LEN];
    share_file *f;
    int i = 0;

    share_init(&ix);

#define ADD(path, size) do {                                               \
        f = share_add_file(&ix, path, path, size, 1);                      \
        memset(t[i], (unsigned char)(0x40 + i), TTH_HASH_LEN);             \
        share_set_tth(&ix, f, t[i]);                                       \
        i++;                                                              \
    } while (0)

    ADD("DL/warez/DOOM.ZIP", 5000);
    ADD("DL/warez/DOOM2.ZIP", 90000);
    ADD("DL/mods/TUNE.MOD", 3000);
    ADD("DL/docs/READ.TXT", 100);
    ADD("DL/apps/PKZIP.EXE", 2048);

#undef ADD

    /* One file left unhashed on purpose. */
    share_add_file(&ix, "DL/warez/PENDING.ZIP", "x", 1234, 1);

    return &ix;
}

static int match_line(share_index *ix, const char *line, share_file **out,
                      int max)
{
    adc_msg m;

    if (adc_parse(&m, line) != 0)
        return -1;
    return share_match(ix, &m, out, max);
}

/* The hash index: the lookup an incoming CGET and every TR search make.
 *
 * Worth testing beyond "it finds things" because an open-addressed table
 * has three ways to go wrong that a small case never reaches -- growth
 * rehashing, a probe chain broken by a free bucket, and colliding keys. */
static void test_tth_index(void)
{
    share_index ix;
    unsigned char tth[TTH_HASH_LEN];
    char virt[64];
    int i;
    const int n = 3000;        /* forces several growths from 512 */

    CHECK(share_init(&ix) == 0, "init failed");

    for (i = 0; i < n; i++) {
        share_file *f;

        snprintf(virt, sizeof virt, "DL/f%04d.zip", i);
        f = share_add_file(&ix, virt, virt, (tth_u64)(1000 + i), 0);
        CHECK(f != NULL || i > 0, "add failed at %d", i);
        if (!f)
            break;

        /* Distinct hashes, and every fourth one deliberately shares its
           first four bytes with another so collisions are exercised rather
           than assumed away. */
        memset(tth, 0, sizeof tth);
        tth[0] = (unsigned char)(i & 0xFF);
        tth[1] = (unsigned char)((i >> 8) & 0xFF);
        if (i % 4 == 0)
            tth[0] = tth[1] = 0x5A;
        tth[20] = (unsigned char)(i & 0xFF);
        tth[21] = (unsigned char)((i >> 8) & 0xFF);
        share_set_tth(&ix, f, tth);
    }

    CHECK(ix.hashed_count == (unsigned int)n, "hashed %u of %d",
          ix.hashed_count, n);
    CHECK(ix.tth_idx != NULL, "no index was built");
    CHECK(ix.tth_used == (unsigned int)n, "index holds %u of %d",
          ix.tth_used, n);
    CHECK(ix.tth_cap >= (unsigned int)n * 4 / 3,
          "index is too full: %u entries in %u buckets",
          ix.tth_used, ix.tth_cap);

    /* Every file must still be findable after all that growth, and must
       come back as itself rather than as a colliding neighbour. */
    {
        int wrong = 0, missing = 0;

        for (i = 0; i < n; i++) {
            share_file *f;

            memset(tth, 0, sizeof tth);
            tth[0] = (unsigned char)(i & 0xFF);
            tth[1] = (unsigned char)((i >> 8) & 0xFF);
            if (i % 4 == 0)
                tth[0] = tth[1] = 0x5A;
            tth[20] = (unsigned char)(i & 0xFF);
            tth[21] = (unsigned char)((i >> 8) & 0xFF);

            f = share_find_tth(&ix, tth);
            if (!f)
                missing++;
            else if (f->size != (tth_u64)(1000 + i))
                wrong++;
        }
        CHECK(missing == 0, "%d of %d hashes were not found", missing, n);
        CHECK(wrong == 0, "%d lookups returned the wrong file", wrong);
    }

    /* A hash nobody has must not match, and must not run off the end of a
       full-ish table either. */
    memset(tth, 0xEE, sizeof tth);
    CHECK(share_find_tth(&ix, tth) == NULL, "matched a hash we never set");

    /* /rehash: the flags and the table have to be forgotten together. A
       table left populated would hand back records claiming no hash. */
    share_unhash_all(&ix);
    CHECK(ix.hashed_count == 0, "hashed_count is %u after unhash",
          ix.hashed_count);
    CHECK(ix.hashed_size == 0, "hashed_size survived unhash");
    CHECK(ix.tth_used == 0, "the index survived unhash");
    memset(tth, 0, sizeof tth);
    tth[0] = 0x5A; tth[1] = 0x5A; tth[20] = 0; tth[21] = 0;
    CHECK(share_find_tth(&ix, tth) == NULL,
          "found a file by hash after unhashing everything");
    CHECK(ix.count == (unsigned int)n, "unhash lost the file list");

    /* And rehashing works on the emptied table. */
    {
        share_file *f = share_next_unhashed(&ix);
        memset(tth, 0x77, sizeof tth);
        CHECK(f != NULL, "nothing to rehash");
        if (f) {
            share_set_tth(&ix, f, tth);
            CHECK(share_find_tth(&ix, tth) == f, "rehash is not findable");
        }
    }

    share_free(&ix);
}

/* The letter mask that keeps share_match off most of the strings. A false
   positive only costs time; a false negative silently hides a file from a
   search, so these cases are all about terms that MUST still match. */
static void test_name_filter(void)
{
    share_index ix;
    share_file *f;
    share_file *hits[8];
    unsigned char tth[TTH_HASH_LEN];
    adc_msg m;
    int n;

    CHECK(share_init(&ix) == 0, "init failed");

    f = share_add_file(&ix, "Warez/Doom 19 (1994) [ANSi].ZIP",
                       "C:\\DL\\DOOM19.ZIP", 4096, 0);
    CHECK(f != NULL, "add failed");
    memset(tth, 0x31, sizeof tth);
    share_set_tth(&ix, f, tth);

#define MATCHES(line) (adc_parse(&m, line) == 0 ? \
                       share_match(&ix, &m, hits, 8) : -1)

    n = MATCHES("BSCH AAAA ANdoom");
    CHECK(n == 1, "lower-case term did not match (%d)", n);
    n = MATCHES("BSCH AAAA ANDOOM");
    CHECK(n == 1, "upper-case term did not match (%d)", n);
    n = MATCHES("BSCH AAAA ANwarez ANzip");
    CHECK(n == 1, "two terms from different parts of the path (%d)", n);

    /* Digits all share one bit, so a term of digits must not be filtered
       out by the mask -- the string compare decides. */
    n = MATCHES("BSCH AAAA AN1994");
    CHECK(n == 1, "digits did not match (%d)", n);
    n = MATCHES("BSCH AAAA AN1234");
    CHECK(n == 0, "digits matched a number that is not there (%d)", n);

    /* Punctuation and separators are outside the mask entirely, which must
       mean "no opinion", not "reject". */
    n = MATCHES("BSCH AAAA AN[ANSi]");
    CHECK(n == 1, "bracketed term did not match (%d)", n);
    n = MATCHES("BSCH AAAA AN(1994)");
    CHECK(n == 1, "parenthesised term did not match (%d)", n);

    /* The rejections the mask exists for. */
    n = MATCHES("BSCH AAAA ANquake");
    CHECK(n == 0, "matched a name with letters we do not have (%d)", n);
    n = MATCHES("BSCH AAAA ANdoom ANquake");
    CHECK(n == 0, "one impossible term did not disqualify the file (%d)", n);

    /* A NO term's letters must never be required: this file has no "x", so
       excluding "xyz" leaves it matching. */
    n = MATCHES("BSCH AAAA ANdoom NOxyz");
    CHECK(n == 1, "a NO term was treated as required (%d)", n);
    n = MATCHES("BSCH AAAA ANdoom NOansi");
    CHECK(n == 0, "a NO term that is present did not exclude (%d)", n);

    /* More terms than the fast path holds. Nothing may be dropped: a lost
       AN term over-matches, and a lost NO term answers with a file the
       asker said they did not want. */
    n = MATCHES("BSCH AAAA ANw ANa ANr ANe ANz ANd ANo ANm ANi ANn ANs "
                "ANp ANz ANa");
    CHECK(n == 1, "fourteen satisfiable terms did not match (%d)", n);

    n = MATCHES("BSCH AAAA ANw ANa ANr ANe ANz ANd ANo ANm ANi ANn ANs "
                "ANp ANz ANquake");
    CHECK(n == 0, "an impossible fifteenth term was dropped (%d)", n);

    n = MATCHES("BSCH AAAA ANw ANa ANr ANe ANz ANd ANo ANm ANi ANn ANs "
                "ANp ANz NOansi");
    CHECK(n == 0, "a NO term past the fast path was dropped (%d)", n);

    /* A term longer than the match buffer. For AN that simply does not
       match; for NO we must decline rather than assume it is absent. */
    {
        char line[400];
        char longterm[200];

        memset(longterm, 'z', sizeof longterm - 1);
        longterm[sizeof longterm - 1] = '\0';

        snprintf(line, sizeof line, "BSCH AAAA AN%s", longterm);
        n = MATCHES(line);
        CHECK(n == 0, "an impossibly long AN term matched (%d)", n);

        snprintf(line, sizeof line, "BSCH AAAA ANdoom NO%s", longterm);
        n = MATCHES(line);
        CHECK(n == 0, "answered despite a NO term we could not evaluate "
                      "(%d)", n);
    }

#undef MATCHES

    share_free(&ix);
}

/* Leaf hashes, for a peer that wants to verify parts of a file on its own.
 *
 * Checking that the file is non-empty would prove nothing: the hashes have
 * to be the RIGHT ones. So the test rebuilds the Merkle root from the
 * leaves it was given and compares that with the root we advertise. If the
 * level, the order, the prefix bytes or the promotion rule were wrong, the
 * roots would differ.
 *
 * A real client asked for these and dropped us as a source when we refused,
 * which is how this came to exist. */
/* Fold a leaf set up to a root, the way a receiving client does: pair with
   a 0x01 prefix, promote an odd one unchanged. */
static void fold_leaves(unsigned char *cur, long count,
                        unsigned char *root_out)
{
    while (count > 1) {
        long out = 0, i;

        for (i = 0; i + 1 < count; i += 2) {
            tiger_ctx tc;
            unsigned char pfx = 0x01;

            tiger_init(&tc);
            tiger_update(&tc, &pfx, 1);
            tiger_update(&tc, cur + i * TTH_HASH_LEN, TTH_HASH_LEN);
            tiger_update(&tc, cur + (i + 1) * TTH_HASH_LEN, TTH_HASH_LEN);
            tiger_final(&tc, cur + out * TTH_HASH_LEN);
            out++;
        }
        if (i < count) {
            memmove(cur + out * TTH_HASH_LEN, cur + i * TTH_HASH_LEN,
                    TTH_HASH_LEN);
            out++;
        }
        count = out;
    }
    memcpy(root_out, cur, TTH_HASH_LEN);
}

/* One size, end to end: hash it, write its leaf set, and check the set
   describes the file -- the right number of nodes, and a fold that
   rebuilds the root.
 *
 * The sizes that matter are the ones where the level is above zero AND the
 * size is not a multiple of the span, because that is where the emitter
 * used to drop the tail and produce a set one node short. The receiver
 * refuses such a set, so the symptom was a download that reported itself
 * complete without moving a byte. */
static void tthl_case(tth_u64 size, int want_level)
{
    share_index ix;
    share_file *f;
    char path[384], leafpath[384];
    unsigned char root[TTH_HASH_LEN], rebuilt[TTH_HASH_LEN];
    tth_u64 span = (tth_u64)TTH_LEAF_SIZE << want_level;
    long want_count = (long)((size + span - 1) / span);
    int level = -1;
    long n;

    snprintf(path, sizeof path, "%s/tthl_big.bin", g_root);
    snprintf(leafpath, sizeof leafpath, "%s/tthl_big_out.bin", g_root);

    {
        FILE *w = fopen(path, "wb");
        unsigned char buf[8192];
        tth_u64 at;

        CHECK(w != NULL, "cannot write the fixture");
        if (!w)
            return;
        for (at = 0; at < size; at += sizeof buf) {
            size_t i;
            size_t want = sizeof buf;

            if (at + want > size)
                want = (size_t)(size - at);
            for (i = 0; i < want; i++)
                buf[i] = (unsigned char)(((at + i) * 31 +
                                          ((at + i) >> 7)) & 0xFF);
            fwrite(buf, 1, want, w);
        }
        fclose(w);
    }

    {
        tth_ctx ctx;
        FILE *r = fopen(path, "rb");
        unsigned char buf[8192];
        size_t got;

        tth_init(&ctx, NULL, NULL, -1);
        while ((got = fread(buf, 1, sizeof buf, r)) > 0)
            tth_update(&ctx, buf, got);
        fclose(r);
        tth_final(&ctx, root);
    }

    CHECK(share_init(&ix) == 0, "share_init failed");
    f = share_add_file(&ix, "DL/big.bin", path, size, 0);
    CHECK(f != NULL, "add failed for %lu bytes", (unsigned long)size);
    if (!f) { share_free(&ix); return; }
    share_set_tth(&ix, f, root);

    n = share_write_tthl(&ix, f, leafpath, &level);
    CHECK(n > 0, "no leaves for %lu bytes", (unsigned long)size);
    CHECK(level == want_level,
          "%lu bytes chose level %d, expected %d",
          (unsigned long)size, level, want_level);
    CHECK(n / TTH_HASH_LEN == want_count,
          "%lu bytes gave %ld nodes at level %d, expected %ld -- a set one "
          "short does not describe the file",
          (unsigned long)size, n / TTH_HASH_LEN, level, want_count);

    if (n > 0) {
        FILE *r = fopen(leafpath, "rb");
        unsigned char *cur = (unsigned char *)malloc((size_t)n);

        if (r && cur && fread(cur, 1, (size_t)n, r) == (size_t)n) {
            fold_leaves(cur, n / TTH_HASH_LEN, rebuilt);
            CHECK(memcmp(rebuilt, root, TTH_HASH_LEN) == 0,
                  "the %ld-node set for %lu bytes does not rebuild the root",
                  n / TTH_HASH_LEN, (unsigned long)size);
        } else {
            CHECK(0, "could not read the leaves back");
        }
        if (r) fclose(r);
        free(cur);
    }

    remove(path);
    remove(leafpath);
    share_free(&ix);
}

static void test_tthl_sizes(void)
{
    /* Each of these needs a level above zero and has a short last node.
       3,000,000 is level 2 with 733 nodes (732.4 before rounding);
       9,000,000 is level 4 with 550. The 20,000,000 case that found the
       bug is level 5 with 611 and lives in test_seg, where the plan is
       what gets checked. */
    tthl_case(3000000UL, 2);
    tthl_case(9000000UL, 4);

    /* And one that divides exactly. 2,097,152 bytes is 1024 nodes at
       level 1 -- exactly the cap, which is why that level is chosen rather
       than level 2 -- with nothing left over, so there must be no extra
       empty node at the end. */
    tthl_case(2097152UL, 1);
}

static void test_tthl(void)
{
    share_index ix;
    share_file *f;
    char path[384];
    char leafpath[384];
    unsigned char root[TTH_HASH_LEN];
    unsigned char rebuilt[TTH_HASH_LEN];
    long n;
    int level = -1;
    const long size = 70000;    /* 69 leaves at level 0: not a round tree */

    snprintf(path, sizeof path, "%s/tthl_src.bin", g_root);
    snprintf(leafpath, sizeof leafpath, "%s/tthl_out.bin", g_root);

    /* A file with varied content, hashed the ordinary way first. */
    {
        FILE *w = fopen(path, "wb");
        long i;

        CHECK(w != NULL, "cannot write the fixture");
        if (!w)
            return;
        for (i = 0; i < size; i++)
            fputc((int)((i * 31 + (i >> 7)) & 0xFF), w);
        fclose(w);
    }

    {
        tth_ctx ctx;
        FILE *r = fopen(path, "rb");
        unsigned char buf[4096];
        size_t got;

        tth_init(&ctx, NULL, NULL, -1);
        while ((got = fread(buf, 1, sizeof buf, r)) > 0)
            tth_update(&ctx, buf, got);
        fclose(r);
        tth_final(&ctx, root);
    }

    CHECK(share_init(&ix) == 0, "share_init failed");
    f = share_add_file(&ix, "DL/tthl.bin", path, (tth_u64)size, 0);
    CHECK(f != NULL, "add failed");
    if (!f) { share_free(&ix); return; }

    /* Unhashed: there is nothing to serve leaves against. */
    CHECK(share_write_tthl(&ix, f, leafpath, &level) < 0,
          "wrote leaves for a file we have not hashed");

    share_set_tth(&ix, f, root);

    n = share_write_tthl(&ix, f, leafpath, &level);
    CHECK(n > 0, "no leaf hashes were written");
    CHECK(n % TTH_HASH_LEN == 0,
          "%ld bytes of leaves is not a whole number of hashes", n);
    CHECK(level == 0, "level %d for a 70KB file, expected 0", level);
    CHECK(n / TTH_HASH_LEN == (size + TTH_LEAF_SIZE - 1) / TTH_LEAF_SIZE,
          "%ld leaves for %ld bytes", n / TTH_HASH_LEN, size);

    /* Rebuild the root from the leaves: pair them up, prefix 0x01, and
       promote an odd one unchanged. That last rule is the one that silently
       produces a plausible wrong answer. */
    {
        FILE *r = fopen(leafpath, "rb");
        unsigned char *cur;
        long count = n / TTH_HASH_LEN;
        long i;

        CHECK(r != NULL, "cannot read the leaves back");
        cur = (unsigned char *)malloc((size_t)n);
        CHECK(cur != NULL, "out of memory");
        if (!r || !cur) {
            if (r) fclose(r);
            free(cur);
            share_free(&ix);
            return;
        }
        CHECK(fread(cur, 1, (size_t)n, r) == (size_t)n, "short read");
        fclose(r);

        while (count > 1) {
            long out = 0;

            for (i = 0; i + 1 < count; i += 2) {
                tiger_ctx tc;
                unsigned char pfx = 0x01;

                tiger_init(&tc);
                tiger_update(&tc, &pfx, 1);
                tiger_update(&tc, cur + i * TTH_HASH_LEN, TTH_HASH_LEN);
                tiger_update(&tc, cur + (i + 1) * TTH_HASH_LEN,
                             TTH_HASH_LEN);
                tiger_final(&tc, cur + out * TTH_HASH_LEN);
                out++;
            }
            if (i < count) {
                memmove(cur + out * TTH_HASH_LEN, cur + i * TTH_HASH_LEN,
                        TTH_HASH_LEN);
                out++;
            }
            count = out;
        }

        memcpy(rebuilt, cur, TTH_HASH_LEN);
        free(cur);

        CHECK(memcmp(rebuilt, root, TTH_HASH_LEN) == 0,
              "the leaves do not rebuild the root we advertise");
    }

    /* A big file must not produce megabytes of leaves: the level has to
       rise with the size. 400MB at level 0 would be 9.6MB of hashes. */
    {
        share_file *big = share_add_file(&ix, "DL/big.bin", path,
                                         (tth_u64)400 * 1024 * 1024, 0);
        int biglevel = -1;

        CHECK(big != NULL, "add failed");
        if (big) {
            unsigned char fake[TTH_HASH_LEN];

            memset(fake, 0x99, sizeof fake);
            share_set_tth(&ix, big, fake);

            /* It will fail -- the file on disk is 70KB, not 400MB, so the
               root will not match -- but the level it chose is visible in
               the attempt, and the refusal is the right outcome: serving
               leaves that do not match the advertised root would make the
               asker reject a good file later. */
            CHECK(share_write_tthl(&ix, big, leafpath, &biglevel) < 0,
                  "served leaves whose root does not match");
            CHECK(biglevel == -1 || biglevel > 0,
                  "level %d for a 400MB file: that is 9.6MB of hashes",
                  biglevel);
        }
    }

    remove(path);
    remove(leafpath);
    share_free(&ix);
}

/* The leaf cache. The interesting question is not "does it store bytes"
   but "did the second request avoid reading the file", and the way to see
   that without timing anything is to take the file away: a cache hit does
   not need it, a recomputation does. */
static void test_tthl_cache(void)
{
    share_index ix;
    share_file *f;
    char src[384], leaves[384], store[384];
    unsigned char root[TTH_HASH_LEN];
    long first, second;
    int level = -1;
    const long size = 50000;

    snprintf(src, sizeof src, "%s/tthlc_src.bin", g_root);
    snprintf(leaves, sizeof leaves, "%s/tthlc_out.bin", g_root);
    snprintf(store, sizeof store, "%s", TTHL_CACHE_FILE);
    remove(store);

    {
        FILE *w = fopen(src, "wb");
        long i;

        CHECK(w != NULL, "cannot write the fixture");
        if (!w)
            return;
        for (i = 0; i < size; i++)
            fputc((int)((i * 17 + (i >> 5)) & 0xFF), w);
        fclose(w);
    }

    {
        tth_ctx ctx;
        FILE *r = fopen(src, "rb");
        unsigned char buf[4096];
        size_t got;

        tth_init(&ctx, NULL, NULL, -1);
        while ((got = fread(buf, 1, sizeof buf, r)) > 0)
            tth_update(&ctx, buf, got);
        fclose(r);
        tth_final(&ctx, root);
    }

    CHECK(share_init(&ix) == 0, "share_init failed");
    f = share_add_file(&ix, "DL/cached.bin", src, (tth_u64)size, 0);
    CHECK(f != NULL, "add failed");
    if (!f) { share_free(&ix); return; }
    share_set_tth(&ix, f, root);

    /* Nothing cached yet: this one reads the file. */
    CHECK(tthl_cache_stat(store, NULL) < 0,
          "the store exists before anything was cached");
    first = share_write_tthl(&ix, f, leaves, &level);
    CHECK(first > 0, "no leaves were written");
    CHECK(tthl_cache_stat(store, NULL) == 1,
          "the first request did not fill the cache");

    /* Take the source away. A hit does not need it. */
    remove(src);
    remove(leaves);

    level = -1;
    second = share_write_tthl(&ix, f, leaves, &level);
    CHECK(second == first,
          "the cached answer is %ld bytes, the computed one was %ld",
          second, first);
    CHECK(level == 0, "the cached level came back as %d", level);

    /* And byte for byte the same, since that is what a peer will verify
       against the root. */
    {
        FILE *r = fopen(leaves, "rb");
        unsigned char got[TTH_HASH_LEN];
        unsigned char want[TTH_HASH_LEN];
        tth_ctx ctx;
        long i;
        int bad = 0;

        CHECK(r != NULL, "the cached leaves were not written out");
        if (r) {
            /* Recompute leaf 0 from what the file used to be: the content
               is deterministic, so this is an independent check rather
               than a comparison of the cache with itself. */
            tth_init(&ctx, NULL, NULL, -1);
            for (i = 0; i < TTH_LEAF_SIZE; i++) {
                unsigned char b = (unsigned char)((i * 17 + (i >> 5)) & 0xFF);
                tth_update(&ctx, &b, 1);
            }
            tth_final(&ctx, want);

            if (fread(got, 1, TTH_HASH_LEN, r) != TTH_HASH_LEN)
                bad = 1;
            fclose(r);

            CHECK(!bad, "could not read the cached leaves back");
            CHECK(memcmp(got, want, TTH_HASH_LEN) == 0,
                  "the cached first leaf is not the right hash");
        }
    }

    /* A hash nobody cached is a miss, not a wrong answer. */
    {
        unsigned char other[TTH_HASH_LEN];

        memset(other, 0xAB, sizeof other);
        CHECK(tthl_cache_get(store, other, leaves, &level) < 0,
              "the cache answered for a hash it does not have");
    }

    /* A store full of rubbish must read as empty rather than as records.
       It is a cache: throwing it away is always allowed. */
    {
        FILE *w = fopen(store, "wb");

        if (w) {
            fputs("not a leaf store at all", w);
            fclose(w);
        }
        CHECK(tthl_cache_stat(store, NULL) < 0,
              "junk in the store was read as records");
        CHECK(tthl_cache_get(store, root, leaves, &level) < 0,
              "junk in the store produced an answer");

        /* And a put over the junk replaces it rather than appending to
           something it cannot read. */
        {
            unsigned char fake[TTH_HASH_LEN * 2];

            memset(fake, 0x5A, sizeof fake);
            CHECK(tthl_cache_put(store, root, 0, fake, sizeof fake) == 0,
                  "could not write over a junk store");
            CHECK(tthl_cache_stat(store, NULL) == 1,
                  "the store was not replaced");
        }
    }

    remove(store);
    remove(leaves);
    share_free(&ix);
}

/* The cache is bounded, because leaves are bulky: a thousand-file share
   would otherwise grow a 24MB store beside a 16MB machine. Past the cap the
   oldest half goes, which is the part that has to keep working -- a cache
   that stops accepting entries stops helping the moment somebody adds
   files. */
static void test_tthl_cache_bounded(void)
{
    char store[384];
    char out[384];
    unsigned char leaves[1024 * TTH_HASH_LEN];
    unsigned char key[TTH_HASH_LEN];
    long bytes = 0;
    int i;
    int records;

    snprintf(store, sizeof store, "%s/tthlc_big.bin", g_root);
    snprintf(out, sizeof out, "%s/tthlc_big_out.bin", g_root);
    remove(store);

    memset(leaves, 0x11, sizeof leaves);

    /* Each record is 24KB of leaves, so this is about 1.1MB offered to a
       512KB store. */
    for (i = 0; i < 48; i++) {
        memset(key, 0, sizeof key);
        key[0] = (unsigned char)i;
        key[1] = (unsigned char)(i >> 8);
        /* Vary the content so a mix-up between records would show. */
        leaves[0] = (unsigned char)i;
        CHECK(tthl_cache_put(store, key, 0, leaves, sizeof leaves) == 0,
              "put %d failed", i);
    }

    records = tthl_cache_stat(store, &bytes);
    CHECK(records > 0, "the store is empty after 48 puts");
    CHECK(bytes <= (long)TTHL_CACHE_MAX_BYTES + 32 * 1024,
          "the store grew to %ld bytes, cap is %lu",
          bytes, TTHL_CACHE_MAX_BYTES);

    /* The most recent one must still be there: eviction drops the oldest,
       and dropping what was just asked for would make the cache useless
       exactly when it is being used. */
    memset(key, 0, sizeof key);
    key[0] = 47;
    CHECK(tthl_cache_get(store, key, out, NULL) ==
          (long)sizeof leaves,
          "the newest record was evicted");

    /* And the content belongs to that record, not a neighbour. */
    {
        FILE *r = fopen(out, "rb");
        int first = -1;

        if (r) {
            first = fgetc(r);
            fclose(r);
        }
        CHECK(first == 47, "the newest record came back with content %d",
              first);
    }

    /* The oldest is gone, which is the point. */
    memset(key, 0, sizeof key);
    key[0] = 0;
    CHECK(tthl_cache_get(store, key, out, NULL) < 0,
          "nothing was evicted, so the cap does not hold");

    /* Putting the same hash twice does not grow the store. */
    {
        long before = 0, after = 0;

        tthl_cache_stat(store, &before);
        memset(key, 0, sizeof key);
        key[0] = 47;
        CHECK(tthl_cache_put(store, key, 0, leaves, sizeof leaves) == 0,
              "a repeat put failed");
        tthl_cache_stat(store, &after);
        CHECK(after == before, "a repeat put grew the store by %ld bytes",
              after - before);
    }

    remove(store);
    remove(out);
}

static void test_matching(void)
{
    share_index *ix = match_fixture();
    share_file *hits[16];
    int n;

    /* A term matches anywhere in the virtual path, so a directory name
       counts -- which is what people expect from searching "warez". */
    n = match_line(ix, "BSCH AAAC ANwarez", hits, 16);
    CHECK(n == 2, "warez matched %d, want 2 (the third is unhashed)", n);

    n = match_line(ix, "BSCH AAAC ANdoom", hits, 16);
    CHECK(n == 2, "doom matched %d", n);

    /* Case-insensitive. */
    n = match_line(ix, "BSCH AAAC ANDOOM", hits, 16);
    CHECK(n == 2, "uppercase matched %d", n);

    /* Every term must match. */
    n = match_line(ix, "BSCH AAAC ANdoom ANzip", hits, 16);
    CHECK(n == 2, "two terms matched %d", n);
    n = match_line(ix, "BSCH AAAC ANdoom ANmod", hits, 16);
    CHECK(n == 0, "contradictory terms matched %d", n);

    /* Exclusions. */
    n = match_line(ix, "BSCH AAAC ANzip NOdoom", hits, 16);
    CHECK(n == 1, "exclusion gave %d, want 1 (PKZIP.EXE has no .zip)", n);

    /* Size bounds. */
    n = match_line(ix, "BSCH AAAC ANzip GE10000", hits, 16);
    CHECK(n == 1, "GE gave %d", n);
    n = match_line(ix, "BSCH AAAC ANzip LE10000", hits, 16);
    CHECK(n == 2, "LE gave %d", n);
    n = match_line(ix, "BSCH AAAC EQ3000", hits, 16);
    CHECK(n == 1, "EQ gave %d", n);

    /* Groups, by extension. */
    n = match_line(ix, "BSCH AAAC ANdl GR2", hits, 16);   /* archive */
    CHECK(n == 2, "archive group gave %d", n);
    n = match_line(ix, "BSCH AAAC ANdl GR1", hits, 16);   /* audio */
    CHECK(n == 1, "audio group gave %d", n);
    n = match_line(ix, "BSCH AAAC ANdl GR4", hits, 16);   /* executable */
    CHECK(n == 1, "executable group gave %d", n);
    /* Group 7 is directories, and we share none. Claiming otherwise would
       invite a browse we cannot serve. */
    n = match_line(ix, "BSCH AAAC ANdl GR7", hits, 16);
    CHECK(n == 0, "directory group gave %d, want 0", n);

    /* An exact hash search returns at most one file. */
    {
        char b32[BASE32_TIGER_LEN + 1];
        char line[128];
        unsigned char want[TTH_HASH_LEN];

        memset(want, 0x41, sizeof want);        /* the second file */
        base32_encode(b32, sizeof b32, want, TTH_HASH_LEN);
        snprintf(line, sizeof line, "BSCH AAAC TR%s", b32);
        n = match_line(ix, line, hits, 16);
        CHECK(n == 1, "hash search gave %d", n);

        snprintf(line, sizeof line,
                 "BSCH AAAC TRQAHL3II3MKN3H3CBMEPNGTAGLIQKURGL2VM2UII");
        n = match_line(ix, line, hits, 16);
        CHECK(n == 0, "an unknown hash matched %d", n);
    }

    /* An unhashed file is never offered, however well it matches. */
    n = match_line(ix, "BSCH AAAC ANpending", hits, 16);
    CHECK(n == 0, "offered an unhashed file");

    /* The reply cap is honoured, because flooding a hub with results is
       something hubs act on. */
    n = match_line(ix, "BSCH AAAC ANdl", hits, 2);
    CHECK(n == 2, "cap of 2 gave %d", n);
    n = match_line(ix, "BSCH AAAC ANdl", hits, 0);
    CHECK(n == 0, "cap of 0 gave %d", n);

    share_free(ix);
}

/* ---------------------------------------------------------------- scan */

static void test_scan(void)
{
    share_index ix;
    scan_state *ss = NULL;
    int guard;

    /* The skip rules are policy worth asserting directly. */
    CHECK(scan_should_skip(".") == 1, "did not skip .");
    CHECK(scan_should_skip("..") == 1, "did not skip ..");
    CHECK(scan_should_skip(".hidden") == 1, "did not skip a dotfile");
    CHECK(scan_should_skip("DOSYPP.TTH") == 1, "did not skip our cache");
    CHECK(scan_should_skip("dosypp.ini") == 1, "skip is case-sensitive");
    CHECK(scan_should_skip("FILES.XML") == 1, "did not skip the file list");
    CHECK(scan_should_skip("thing.part") == 1,
          "did not skip a partial download");
    CHECK(scan_should_skip("thing.TMP") == 1, "did not skip a temp file");
    CHECK(scan_should_skip("DOOM.ZIP") == 0, "skipped a real file");
    CHECK(scan_should_skip("") == 1, "did not skip an empty name");
    CHECK(scan_should_skip(NULL) == 1, "did not skip NULL");

    share_init(&ix);

    /* Nothing configured: begin must refuse rather than scan the world. */
    CHECK(scan_begin(&ss, &ix) == -1, "scanned with no roots");

    share_add_root(&ix, g_root);
    CHECK(scan_begin(&ss, &ix) == 0, "begin failed");
    CHECK(ss != NULL, "no scan state");

    /* Incremental: each step is bounded, so the UI keeps running. */
    for (guard = 0; guard < 1000 && ss; guard++)
        if (scan_step(&ss, 4) == 0)
            break;
    CHECK(ss == NULL, "scan did not finish");
    CHECK(guard > 1, "scan finished in one step; it is not incremental");

    CHECK(ix.count == 4, "found %u files, want 4", ix.count);
    CHECK(ix.total_size == 5000 + 2048 + 3000 + 100,
          "total is %lu", (unsigned long)ix.total_size);

    /* The advertised path starts at the root's own name, so the local
       layout is not leaked. */
    {
        unsigned int i;
        int saw_doom = 0, saw_hidden = 0, saw_zero = 0;

        for (i = 0; i < ix.count; i++) {
            const char *v = share_virt(&ix, &ix.files[i]);
            if (strstr(v, "DOOM.ZIP"))
                saw_doom = 1;
            if (strstr(v, "hidden"))
                saw_hidden = 1;
            if (strstr(v, "ZERO"))
                saw_zero = 1;
            CHECK(strchr(v, '\\') == NULL,
                  "advertised path uses a backslash: %s", v);
            CHECK(v[0] != '/', "advertised path is absolute: %s", v);
        }
        CHECK(saw_doom, "DOOM.ZIP was not indexed");
        CHECK(!saw_hidden, "a hidden file was shared");
        CHECK(!saw_zero, "a zero-length file was shared");
    }

    /* Cancelling part-way must not leak or crash. */
    ss = NULL;
    scan_begin(&ss, &ix);
    scan_step(&ss, 2);
    scan_cancel(&ss);
    CHECK(ss == NULL, "cancel did not clear the state");

    share_free(&ix);
}

/* --------------------------------------------------------------- hashq */

static void scan_all(share_index *ix)
{
    scan_state *ss = NULL;
    int guard;

    scan_begin(&ss, ix);
    for (guard = 0; guard < 10000 && ss; guard++)
        if (scan_step(&ss, 32) == 0)
            break;
}

static void test_hashq(void)
{
    share_index ix;
    hashq *hq = NULL;
    char cache[320];
    int guard;

    /* Deliberately outside the share tree: anything inside it gets
       indexed, which is how this test first "failed". A real install keeps
       DOSYPP.TTH beside the program, not in the share. */
    snprintf(cache, sizeof cache, "%s.tth", g_root);

    share_init(&ix);
    share_add_root(&ix, g_root);
    scan_all(&ix);
    CHECK(ix.count == 4, "scan found %u", ix.count);

    CHECK(hashq_init(&hq, &ix) == 0, "hashq init failed");
    CHECK(hashq_load(hq, cache) == 0, "a missing cache is not an error");
    CHECK(hashq_percent(hq) == 0, "percent is %d before hashing",
          hashq_percent(hq));

    /* Budgeted: a tiny budget must make progress without finishing, which
       is what keeps the UI alive on a slow machine. */
    CHECK(hashq_tick(hq, 512) == 1, "a small tick claimed completion");
    CHECK(ix.hashed_count < ix.count, "one tick hashed everything");

    for (guard = 0; guard < 10000; guard++)
        if (hashq_tick(hq, 4096) == 0)
            break;
    CHECK(ix.hashed_count == 4, "hashed %u of 4", ix.hashed_count);
    CHECK(hashq_percent(hq) == 100, "percent is %d when done",
          hashq_percent(hq));
    CHECK(hashq_files_left(hq) == 0, "%u files left", hashq_files_left(hq));

    /* The hashes must match a straight TTH of the same bytes. */
    {
        unsigned int i;
        for (i = 0; i < ix.count; i++) {
            share_file *f = &ix.files[i];
            FILE *fp = fopen(share_local(&ix, f), "rb");
            unsigned char buf[8192], want[TTH_HASH_LEN];
            tth_ctx c;
            size_t got;

            CHECK(fp != NULL, "cannot reopen %s", share_local(&ix, f));
            if (!fp)
                continue;
            tth_init(&c, NULL, NULL, -1);
            while ((got = fread(buf, 1, sizeof buf, fp)) > 0)
                tth_update(&c, buf, got);
            fclose(fp);
            tth_final(&c, want);
            CHECK(memcmp(want, f->tth, TTH_HASH_LEN) == 0,
                  "hash mismatch for %s", share_virt(&ix, f));
        }
    }

    CHECK(hashq_save(hq, cache) == 0, "save failed");
    hashq_free(&hq);

    /* A fresh index over the same tree must come back entirely from the
       cache. This is what makes a restart cost seconds instead of
       re-hashing the whole share. */
    {
        share_index ix2;
        hashq *hq2 = NULL;

        share_init(&ix2);
        share_add_root(&ix2, g_root);
        scan_all(&ix2);
        hashq_init(&hq2, &ix2);
        CHECK(hashq_load(hq2, cache) == 4, "cache satisfied %d of 4",
              hashq_load(hq2, cache));
        CHECK(hashq_tick(hq2, 4096) == 0,
              "there was still work after loading the cache");
        hashq_free(&hq2);
        share_free(&ix2);
    }

    /* A file whose size changed must NOT be taken from the cache: serving
       a stale hash would fail verification at the far end. */
    {
        share_index ix3;
        hashq *hq3 = NULL;

        mkfile("warez/doom/DOOM.ZIP", 6000, 31);     /* different size */

        share_init(&ix3);
        share_add_root(&ix3, g_root);
        scan_all(&ix3);
        hashq_init(&hq3, &ix3);
        CHECK(hashq_load(hq3, cache) == 3,
              "a resized file was taken from the cache");
        hashq_free(&hq3);
        share_free(&ix3);
    }

    /* A corrupt cache line is skipped, not fatal. */
    {
        FILE *f = fopen(cache, "a");
        share_index ix4;
        hashq *hq4 = NULL;

        if (f) {
            fprintf(f, "this is not a cache line at all\n");
            fprintf(f, "NOTBASE32!!! 1 2 some/path\n");
            fclose(f);
        }
        share_init(&ix4);
        share_add_root(&ix4, g_root);
        scan_all(&ix4);
        hashq_init(&hq4, &ix4);
        CHECK(hashq_load(hq4, cache) >= 0, "a corrupt cache was fatal");
        hashq_free(&hq4);
        share_free(&ix4);
    }

    share_free(&ix);
}

/* ------------------------------------------------------------- filelist */

static void test_filelist(void)
{
    share_index ix;
    hashq *hq = NULL;
    char path[320];
    char line[512];
    FILE *f;
    int guard, n;
    int saw_header = 0, saw_dir = 0, saw_file = 0, saw_pending = 0;

    /* Escaping first: a wrongly escaped list is silently rejected by
       peers, which is the worst kind of bug to chase. */
    {
        char out[128];

        CHECK(filelist_escape(out, sizeof out, "plain") == 5, "plain");
        CHECK_STR(out, "plain");
        filelist_escape(out, sizeof out, "a&b");
        CHECK_STR(out, "a&amp;b");
        filelist_escape(out, sizeof out, "<tag>");
        CHECK_STR(out, "&lt;tag&gt;");
        filelist_escape(out, sizeof out, "say \"hi\"");
        CHECK_STR(out, "say &quot;hi&quot;");
        filelist_escape(out, sizeof out, "it's");
        CHECK_STR(out, "it&apos;s");
        /* Control bytes are not legal XML at all; dropping them beats
           emitting a list no peer can parse. */
        filelist_escape(out, sizeof out, "a\x01\x02" "b");
        CHECK_STR(out, "ab");
        CHECK(filelist_escape(out, 4, "&&&&") == -1,
              "did not refuse to overflow");
    }

    snprintf(path, sizeof path, "%s.xml", g_root);

    share_init(&ix);
    share_add_root(&ix, g_root);
    scan_all(&ix);
    hashq_init(&hq, &ix);
    for (guard = 0; guard < 10000; guard++)
        if (hashq_tick(hq, 8192) == 0)
            break;

    /* One file left unhashed: it must not appear, because a peer reading
       the list would ask for it by a hash we do not have. */
    share_add_file(&ix, "pending/NOHASH.BIN", "nope", 10, 1);

    n = filelist_write(&ix, path, "TESTCID");
    CHECK(n == 4, "wrote %d entries, want 4", n);

    f = fopen(path, "r");
    CHECK(f != NULL, "no file list written");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (strstr(line, "<FileListing") && strstr(line, "TESTCID"))
                saw_header = 1;
            if (strstr(line, "<Directory Name="))
                saw_dir = 1;
            if (strstr(line, "<File Name=") && strstr(line, "TTH="))
                saw_file = 1;
            if (strstr(line, "NOHASH"))
                saw_pending = 1;
        }
        fclose(f);
    }
    CHECK(saw_header, "no FileListing header with our CID");
    CHECK(saw_dir, "no Directory elements -- the tree was flattened");
    CHECK(saw_file, "no File elements with a TTH");
    CHECK(!saw_pending, "an unhashed file appeared in the list");

    hashq_free(&hq);
    share_free(&ix);
}

/* Partial lists: one directory at a time.
 *
 * The fixture is warez/doom/DOOM.ZIP, warez/PKZIP.EXE, mods/TUNE.MOD and
 * README.TXT, so the root has two directories and one file, and
 * warez/ has one directory and one file. Those numbers are what make the
 * checks below mean something: a writer that quietly emitted the whole
 * share would pass a test that only looked for the file it asked about.
 */
static int count_in_file(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    int n = 0;

    if (!f)
        return -1;
    while (fgets(line, sizeof line, f))
        if (strstr(line, needle))
            n++;
    fclose(f);
    return n;
}

static int file_has(const char *path, const char *needle)
{
    return count_in_file(path, needle) > 0;
}

static void test_partial_list(void)
{
    share_index ix;
    hashq *hq;
    char path[512];
    int guard, n;

    snprintf(path, sizeof path, "%s.part.xml", g_root);

    share_init(&ix);
    share_add_root(&ix, g_root);
    scan_all(&ix);
    hashq_init(&hq, &ix);
    for (guard = 0; guard < 10000; guard++)
        if (hashq_tick(hq, 8192) == 0)
            break;

    /* A share root appears as a directory named after itself, so every
       virtual path starts with it -- "<root>/warez/PKZIP.EXE". The bases
       below are built from that rather than written out, because the
       fixture's name has the pid in it. The first version of this test
       asked for "/warez/" and the writer correctly said there is no such
       directory. */
    {
        const char *slash = strrchr(g_root, '/');
        const char *root_name = slash ? slash + 1 : g_root;
        char base_root[320], base_warez[320], base_doom[320];

        snprintf(base_root, sizeof base_root, "/%s/", root_name);
        snprintf(base_warez, sizeof base_warez, "/%s/warez/", root_name);
        snprintf(base_doom, sizeof base_doom, "/%s/warez/doom/",
                 root_name);

        /* ---- the actual root: one directory, nothing else */
        n = filelist_write_partial(&ix, "/", 0, path, "TESTCID");
        CHECK(n == 0, "the root holds no files of its own, wrote %d", n);
        CHECK(file_has(path, "Base=\"/\""),
              "no Base attribute on the root list");
        CHECK(count_in_file(path, "<Directory") == 1,
              "the root should list 1 share root, found %d",
              count_in_file(path, "<Directory"));
        CHECK(count_in_file(path, "Incomplete=\"1\"") == 1,
              "the share root is not marked incomplete");
        CHECK(!file_has(path, "README.TXT"),
              "a one-level list of / included a file inside the share");

        /* ---- the share root, one level deep */
        n = filelist_write_partial(&ix, base_root, 0, path, "TESTCID");
        CHECK(n == 1, "the share root holds 1 file at its level, wrote %d",
              n);
        CHECK(file_has(path, "README.TXT"),
              "the share root's own file is missing");
        CHECK(count_in_file(path, "<Directory") == 2,
              "the share root should list 2 directories, found %d",
              count_in_file(path, "<Directory"));
        CHECK(count_in_file(path, "Incomplete=\"1\"") == 2,
              "the subdirectories are not marked incomplete");
        /* The whole point: what is inside those directories is not here. */
        CHECK(!file_has(path, "DOOM.ZIP"),
              "a one-level list included a file two levels down");
        CHECK(!file_has(path, "PKZIP.EXE"),
              "a one-level list included a file one level down");
        CHECK(!file_has(path, "TUNE.MOD"),
              "a one-level list included another directory's file");

        /* ---- a subdirectory */
        n = filelist_write_partial(&ix, base_warez, 0, path, "TESTCID");
        CHECK(n == 1, "warez/ holds 1 file at its own level, wrote %d", n);
        CHECK(file_has(path, "warez/\""),
              "the Base attribute does not name the directory asked for");
        CHECK(file_has(path, "PKZIP.EXE"), "warez/'s own file is missing");
        CHECK(count_in_file(path, "<Directory") == 1,
              "warez/ should list 1 directory, found %d",
              count_in_file(path, "<Directory"));
        CHECK(!file_has(path, "DOOM.ZIP"),
              "the subdirectory's contents came with it");
        CHECK(!file_has(path, "README.TXT"),
              "a list of warez/ included the level above");

        /* Both spellings of the same directory: a client may or may not
           send the trailing slash, and refusing one of them would be a
           browse that works against one client and not another. */
        {
            char no_slash[320];

            snprintf(no_slash, sizeof no_slash, "/%s/warez", root_name);
            n = filelist_write_partial(&ix, no_slash, 0, path, "TESTCID");
            CHECK(n == 1, "a base without a trailing slash wrote %d", n);
            CHECK(file_has(path, "PKZIP.EXE"),
                  "a base without a trailing slash lost the files");
        }

        /* ---- recursive, which is what RE1 asks for */
        n = filelist_write_partial(&ix, base_warez, 1, path, "TESTCID");
        CHECK(n == 2, "a recursive list of warez/ has 2 files, wrote %d", n);
        CHECK(file_has(path, "DOOM.ZIP"),
              "a recursive list of warez/ is missing the nested file");
        CHECK(file_has(path, "<Directory Name=\"doom\">"),
              "a recursive list did not nest the subdirectory");
        CHECK(!file_has(path, "Incomplete"),
              "a recursive list marked something incomplete");
        CHECK(!file_has(path, "README.TXT"),
              "a recursive list of warez/ escaped upwards");

        /* The deepest directory, recursively: one file, no nesting. */
        n = filelist_write_partial(&ix, base_doom, 1, path, "TESTCID");
        CHECK(n == 1, "warez/doom/ has 1 file, wrote %d", n);
        CHECK(file_has(path, "DOOM.ZIP"),
              "the file in warez/doom/ is missing");
        CHECK(!file_has(path, "<Directory"),
              "a list of the deepest directory contains a directory");

        /* ---- a directory that is not there is refused, not answered
           empty. "Not shared" and "shared but empty" are different
           answers, and a client that cannot tell them apart shows an empty
           folder for a typo. */
        CHECK(filelist_write_partial(&ix, "/nosuch/", 0, path,
                                     "TESTCID") == -1,
              "a base that names nothing was answered rather than refused");
        {
            char nested[320];

            snprintf(nested, sizeof nested, "/%s/nosuch/", root_name);
            CHECK(filelist_write_partial(&ix, nested, 1, path,
                                         "TESTCID") == -1,
                  "a nested base that names nothing was answered");
        }

        /* A path trying to climb out of the share is simply not found: the
           base is matched against virtual paths and never used to open
           anything, so this is a miss rather than a traversal. */
        CHECK(filelist_write_partial(&ix, "/../", 0, path, "TESTCID") == -1,
              "a base of /../ was not a miss");
        {
            char dots[320];

            snprintf(dots, sizeof dots, "/%s/warez/../mods/", root_name);
            CHECK(filelist_write_partial(&ix, dots, 0, path,
                                         "TESTCID") == -1,
                  "a base with .. in it was resolved rather than missed");
        }
    }

    /* ---- the compressed form, which is what actually goes on the wire */
    {
        char bz2[512];

        snprintf(bz2, sizeof bz2, "%s.part.bz2", g_root);
        n = filelist_publish_partial(&ix, "/", 0, path, bz2, "TESTCID");
        CHECK(n == 0,
              "publishing a partial list of / wrote %d entries", n);
        {
            FILE *f = fopen(bz2, "rb");
            char magic[3] = { 0, 0, 0 };

            CHECK(f != NULL, "no compressed partial list");
            if (f) {
                CHECK(fread(magic, 1, 2, f) == 2 &&
                      magic[0] == 'B' && magic[1] == 'Z',
                      "the compressed partial list is not bzip2");
                fclose(f);
            }
            remove(bz2);
        }
    }

    remove(path);
    hashq_free(&hq);
    share_free(&ix);
}

int main(void)
{
    printf("DOSy++ share tests (index / scan / hash cache / files.xml)\n");

    build_tree();

    test_index();
    test_tth_index();
    test_name_filter();
    test_tthl();
    test_tthl_sizes();
    test_tthl_cache();
    test_tthl_cache_bounded();
    test_matching();
    test_scan();
    test_hashq();
    test_filelist();
    test_partial_list();

    destroy_tree();

    printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "ok",
           g_run, g_fail);
    return g_fail ? 1 : 0;
}
