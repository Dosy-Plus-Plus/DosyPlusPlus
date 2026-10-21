/* test_hash.c - Tiger, base32 and TTH.
 *
 * Builds for BOTH targets: natively (make test) and for DOS (make dos-test,
 * run under DOSBox-X). That duality is the point -- Tiger is 64-bit integer
 * arithmetic and TTH is byte-order sensitive, so "correct on arm64" says
 * nothing about "correct on i386 built by a 2022 GCC in gnu89 mode". The
 * Phase 1 gate is that this passes in both places.
 *
 * Expected values come from two independent oracles, never from this
 * implementation: RHash 1.4.6 (--tiger, --tth) and Python's
 * base64.b32encode.
 */
#include <stdio.h>
#include <string.h>

#include "../../src/hash/tiger.h"
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

static void to_hex(const unsigned char *b, int n, char *out)
{
    static const char H[] = "0123456789abcdef";
    int i;

    for (i = 0; i < n; i++) {
        out[i * 2]     = H[(b[i] >> 4) & 0xF];
        out[i * 2 + 1] = H[b[i] & 0xF];
    }
    out[n * 2] = '\0';
}

/* Deterministic filler, so the host and DOS runs hash identical bytes. */
static void fill(unsigned char *buf, int n, int seed)
{
    int i;

    for (i = 0; i < n; i++)
        buf[i] = (unsigned char)((i * seed + 7) % 256);
}

/* ------------------------------------------------------------------ Tiger */

static void test_tiger_strings(void)
{
    static const struct { const char *in, *want; } v[] = {
        { "",      "3293ac630c13f0245f92bbb1766e16167a4e58492dde73f3" },
        { "abc",   "2aab1484e8c158f2bfb8c5ff41b57a525129131c957b5f93" },
        { "Tiger", "dd00230799f5009fec6debc838bb6a27df2b9d6f110c7937" },
    };
    unsigned char d[TIGER_DIGEST_LEN];
    char hex[TIGER_DIGEST_LEN * 2 + 1];
    size_t i;

    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        tiger(v[i].in, strlen(v[i].in), d);
        to_hex(d, TIGER_DIGEST_LEN, hex);
        CHECK(strcmp(hex, v[i].want) == 0,
              "tiger(\"%s\") = %s, want %s", v[i].in, hex, v[i].want);
    }
}

static void test_tiger_block_boundaries(void)
{
    /* 64-byte blocks, and padding needs 9 spare bytes -- so lengths either
       side of 56 and 64 exercise the extra-compression path in tiger_final.
       Filler is (i*31+7)%256. */
    static const struct { int len; const char *want; } v[] = {
        {  55, "ee7b1146c19b7e19c8680b91327becc15dd7c0ead66fa2ec" },
        {  56, "56fee55d666d9fcc03a5476666c3d13b3480bd2de1241482" },
        {  57, "6d28e3592d18cf85b7dea782e109506a742c03c007f731ad" },
        {  63, "5965afec9459b3dc45a6ea456ceefe1f10afa0188e49c2cc" },
        {  64, "850f79505abb241dea5af3f2f49abab3d48d2e8c77b4eece" },
        {  65, "39f095874601ab1caac5b3f8bf7fb39e0b88bb3eb49d9262" },
        { 119, "07fa270e49aec36b499cade524b95163be15241fe06a74e9" },
        { 128, "b33c647b98a737e75235a011954992a1d998ecad1734c784" },
    };
    unsigned char buf[256];
    unsigned char d[TIGER_DIGEST_LEN];
    char hex[TIGER_DIGEST_LEN * 2 + 1];
    size_t i;

    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        fill(buf, v[i].len, 31);
        tiger(buf, (size_t)v[i].len, d);
        to_hex(d, TIGER_DIGEST_LEN, hex);
        CHECK(strcmp(hex, v[i].want) == 0,
              "tiger(%d bytes) = %s, want %s", v[i].len, hex, v[i].want);
    }
}

static void test_tiger_chunking(void)
{
    /* Streaming in awkward chunk sizes must equal the one-shot result.
       This is what catches an off-by-one in the partial-block buffer. */
    static const int chunks[] = { 1, 3, 7, 17, 31, 63, 64, 65, 100 };
    unsigned char buf[1000];
    unsigned char want[TIGER_DIGEST_LEN], got[TIGER_DIGEST_LEN];
    size_t ci;

    fill(buf, (int)sizeof buf, 13);
    tiger(buf, sizeof buf, want);

    for (ci = 0; ci < sizeof chunks / sizeof chunks[0]; ci++) {
        tiger_ctx c;
        size_t off = 0;
        size_t step = (size_t)chunks[ci];

        tiger_init(&c);
        while (off < sizeof buf) {
            size_t n = (sizeof buf - off < step) ? sizeof buf - off : step;
            tiger_update(&c, buf + off, n);
            off += n;
        }
        tiger_final(&c, got);

        CHECK(memcmp(want, got, TIGER_DIGEST_LEN) == 0,
              "tiger chunked by %d differs from one-shot", chunks[ci]);
    }
}

/* ----------------------------------------------------------------- base32 */

static void test_base32_vectors(void)
{
    /* RFC 4648 section 10, with padding stripped as ADC uses it. */
    static const struct { const char *in, *want; } v[] = {
        { "",       ""           },
        { "f",      "MY"         },
        { "fo",     "MZXQ"       },
        { "foo",    "MZXW6"      },
        { "foob",   "MZXW6YQ"    },
        { "fooba",  "MZXW6YTB"   },
        { "foobar", "MZXW6YTBOI" },
    };
    char out[64];
    size_t i;

    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        int n = base32_encode(out, sizeof out, v[i].in, strlen(v[i].in));
        CHECK(n >= 0 && strcmp(out, v[i].want) == 0,
              "base32(\"%s\") = \"%s\", want \"%s\"", v[i].in, out, v[i].want);
    }
}

static void test_base32_roundtrip(void)
{
    unsigned char buf[40], back[40];
    char enc[80];
    int len, dec;

    for (len = 0; len <= 32; len++) {
        fill(buf, len, 97);
        CHECK(base32_encode(enc, sizeof enc, buf, (size_t)len) ==
                  (int)BASE32_ENC_LEN(len),
              "encoded length wrong for %d bytes", len);
        dec = base32_decode(back, sizeof back, enc);
        CHECK(dec == len, "decoded %d bytes, want %d", dec, len);
        CHECK(memcmp(buf, back, (size_t)len) == 0,
              "roundtrip differs at %d bytes", len);
    }
}

static void test_base32_errors(void)
{
    unsigned char out[40];
    char enc[8];

    CHECK(base32_decode(out, sizeof out, "MZXW6!!!") == -1,
          "accepted invalid characters");
    CHECK(base32_decode(out, sizeof out, "0189") == -1,
          "accepted 0/1/8/9, which are not in the alphabet");
    /* Lower case is tolerated: rhash and some hubs print it that way. */
    CHECK(base32_decode(out, sizeof out, "mzxw6ytb") == 5,
          "rejected lower case");
    /* Padding is ignored rather than rejected. */
    CHECK(base32_decode(out, sizeof out, "MY======") == 1,
          "did not ignore padding");
    /* Non-zero leftover bits mean two spellings of one value. "MZ" has 2
       bits spare; "M" is 0x0C -> leftover must be zero. */
    CHECK(base32_decode(out, sizeof out, "MZ") == -1,
          "accepted non-canonical trailing bits");
    /* Too small a destination must fail, not overflow. */
    CHECK(base32_encode(enc, 4, "foobar", 6) == -1,
          "encoded into a buffer that was too small");
}

/* -------------------------------------------------------------------- TTH */

static void tth_b32_of(const unsigned char *data, size_t len, char *out)
{
    unsigned char root[TTH_HASH_LEN];

    tth(data, len, root);
    tth_to_base32(root, out);
}

static void test_tth_vectors(void)
{
    /* From rhash --tth, uppercased. Filler is (i*37+11)%256; sizes chosen
       for the cases that break naive implementations:
         0        empty file is one empty leaf, not nothing
         1023/25  either side of a leaf boundary
         1024     exactly one leaf
         2049     three leaves: the third is promoted, not paired with itself
         3072     three full leaves
         5000     five leaves, promotion at two levels
       Filler is (i*37+7)%256 -- it MUST match fill(buf, n, 37) below, or
       every vector here is meaningless. Regenerate with
       test/host/crosscheck_rhash.sh --print-vectors. */
    static const struct { int len; const char *want; } v[] = {
        {     0, "LWPNACQDBZRYXW3VHJVCJ64QBZNGHOHHHZWCLNQ" },
        {     1, "2LPKE47MLI76P3JHDH75RQD6SLFNXRTIR6NH57I" },
        {  1023, "KKN2SI6M7S6XYJKFXQV2LP3HWGV7OA7Z6Q5C3FY" },
        {  1024, "QEFEFW5QUW4XATKHQE4MPSPG4OXOIVHQMPM6GYY" },
        {  1025, "ONQLVIKXWYHBF2LJEJ4KKUSDX3WE3XLI5JMAI7Y" },
        {  2048, "S6NJG7AR6HV37WGPQUMFFNZCZT2GIXPPCVI4S5Q" },
        {  2049, "KEUMTH2Y4MQGVBCZHNRI6OL7QKKGANBQR2IK7GI" },
        {  3072, "ULXIYNB3X47MD5MC6XVVIC4GT5DGJV5QPTHXAKQ" },
        {  4096, "ORAIBIKHUHP5RK5RG2P4QYZD5GLG5WEPBJ4TMTA" },
        {  5000, "M4AWC4C3CRUZ43X2VVFGFKOZAA4NNCM74OYBHXY" },
    };
    static unsigned char buf[5000];
    char b32[BASE32_TIGER_LEN + 1];
    size_t i;

    fill(buf, (int)sizeof buf, 37);

    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        tth_b32_of(buf, (size_t)v[i].len, b32);
        CHECK(strcmp(b32, v[i].want) == 0,
              "tth(%d bytes) = %s, want %s", v[i].len, b32, v[i].want);
    }
}

static void test_tth_empty_is_leaf_of_nothing(void)
{
    /* Stated explicitly because it is the single most common TTH bug:
       TTH("") must be tiger(0x00), not tiger(""). */
    unsigned char zero = 0x00;
    unsigned char direct[TIGER_DIGEST_LEN];
    unsigned char root[TTH_HASH_LEN];

    tiger(&zero, 1, direct);
    tth("", 0, root);

    CHECK(memcmp(direct, root, TTH_HASH_LEN) == 0,
          "TTH of an empty file is not tiger(0x00)");
}

static void test_tth_chunking(void)
{
    /* Feeding the same data in different chunk sizes must not change the
       root. The dangerous case is a chunk that ends exactly on a 1024-byte
       leaf boundary: closing the leaf there would split one leaf in two. */
    static const int chunks[] = { 1, 7, 100, 512, 1023, 1024, 1025, 2048, 3000 };
    static unsigned char buf[5000];
    unsigned char want[TTH_HASH_LEN], got[TTH_HASH_LEN];
    size_t ci;

    fill(buf, (int)sizeof buf, 37);
    tth(buf, sizeof buf, want);

    for (ci = 0; ci < sizeof chunks / sizeof chunks[0]; ci++) {
        tth_ctx c;
        size_t off = 0;
        size_t step = (size_t)chunks[ci];

        tth_init(&c, NULL, NULL, -1);
        while (off < sizeof buf) {
            size_t n = (sizeof buf - off < step) ? sizeof buf - off : step;
            tth_update(&c, buf + off, n);
            off += n;
        }
        tth_final(&c, got);

        CHECK(memcmp(want, got, TTH_HASH_LEN) == 0,
              "tth chunked by %d differs from one-shot", chunks[ci]);
    }
}

struct collect {
    int count;
    unsigned char first[TTH_HASH_LEN];
};

static void on_block(void *user, const unsigned char *hash)
{
    struct collect *c = (struct collect *)user;

    if (c->count == 0)
        memcpy(c->first, hash, TTH_HASH_LEN);
    c->count++;
}

static void test_tth_block_callback(void)
{
    static unsigned char buf[5000];
    unsigned char root[TTH_HASH_LEN];
    unsigned char leaf0[TTH_HASH_LEN];
    struct collect c;
    tth_ctx ctx;
    tiger_ctx t;
    unsigned char prefix = 0x00;

    fill(buf, (int)sizeof buf, 37);

    /* Level 0: one callback per 1024-byte leaf. 5000 bytes = 5 leaves. */
    memset(&c, 0, sizeof c);
    tth_init(&ctx, on_block, &c, 0);
    tth_update(&ctx, buf, sizeof buf);
    tth_final(&ctx, root);
    CHECK(c.count == 5, "level 0 gave %d blocks, want 5", c.count);

    /* The first reported hash must be the first leaf: tiger(0x00 || data). */
    tiger_init(&t);
    tiger_update(&t, &prefix, 1);
    tiger_update(&t, buf, TTH_LEAF_SIZE);
    tiger_final(&t, leaf0);
    CHECK(memcmp(c.first, leaf0, TTH_HASH_LEN) == 0,
          "first level-0 block is not tiger(0x00 || first 1024 bytes)");

    /* Level 1 covers 2048 bytes, so 5000 bytes is three nodes: two
       complete ones and a short last one covering the remaining 904 bytes.
       
       This check used to expect two, on the grounds that the unpaired tail
       is promoted rather than reported -- which is what the emitter did,
       and it was wrong. A leaf set has ceil(size / span) nodes: AirDC++
       sends three for a 150,000-byte file at a 65,536-byte span, and a set
       one node short does not describe the file. A receiver that checks
       (and ours does) refuses it, falls back, and a 19MB download reported
       itself complete without moving a byte. */
    memset(&c, 0, sizeof c);
    tth_init(&ctx, on_block, &c, 1);
    tth_update(&ctx, buf, sizeof buf);
    tth_final(&ctx, root);
    CHECK(c.count == 3, "level 1 gave %d blocks, want 3", c.count);

    /* A size that IS a multiple of the span still gives exactly the
       complete nodes and no empty extra one. */
    memset(&c, 0, sizeof c);
    tth_init(&ctx, on_block, &c, 1);
    tth_update(&ctx, buf, 4096);
    tth_final(&ctx, root);
    CHECK(c.count == 2,
          "4096 bytes at level 1 gave %d blocks, want exactly 2", c.count);

    /* An empty file still has one leaf. */
    memset(&c, 0, sizeof c);
    tth_init(&ctx, on_block, &c, 0);
    tth_final(&ctx, root);
    CHECK(c.count == 1, "empty file gave %d level-0 blocks, want 1", c.count);
}

/* ---------------------------------------------------- ADC identity values */

static void test_cid_derivation(void)
{
    /* CID = base32(tiger(PID)). A hub recomputes this and rejects a
       mismatch, so getting it wrong means "Invalid CID" and no login.
       PID here is the bytes 0x00..0x17; oracle is rhash + Python. */
    unsigned char pid[24];
    unsigned char digest[TIGER_DIGEST_LEN];
    char cid[BASE32_TIGER_LEN + 1];
    char pid_b32[BASE32_TIGER_LEN + 1];
    int i;

    for (i = 0; i < 24; i++)
        pid[i] = (unsigned char)i;

    tiger(pid, sizeof pid, digest);
    base32_encode(cid, sizeof cid, digest, TIGER_DIGEST_LEN);

    CHECK(strcmp(cid, "W6AIUW3CLDF6OGHNVE4JPDDJ2P74IWRCF2O36TA") == 0,
          "CID = %s", cid);
    CHECK(strlen(cid) == 39, "CID is %d chars, want 39", (int)strlen(cid));

    base32_encode(pid_b32, sizeof pid_b32, pid, sizeof pid);
    CHECK(strcmp(pid_b32, "AAAQEAYEAUDAOCAJBIFQYDIOB4IBCEQTCQKRMFY") == 0,
          "PID base32 = %s", pid_b32);
}

/* ------------------------------------------------------------------- main */

int main(void)
{
    printf("DOSy++ hash tests (Tiger / base32 / TTH)\n");
    printf("sizeof(tiger_u64) = %d, must be 8\n", (int)sizeof(tiger_u64));

    /* If this is not 8 the rest is meaningless -- say so loudly, because on
       DOS it is the first thing that could be wrong. */
    CHECK(sizeof(tiger_u64) == 8, "64-bit type is %d bytes",
          (int)sizeof(tiger_u64));

    test_tiger_strings();
    test_tiger_block_boundaries();
    test_tiger_chunking();

    test_base32_vectors();
    test_base32_roundtrip();
    test_base32_errors();

    test_tth_vectors();
    test_tth_empty_is_leaf_of_nothing();
    test_tth_chunking();
    test_tth_block_callback();

    test_cid_derivation();

    printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "ok",
           g_run, g_fail);
    return g_fail ? 1 : 0;
}
