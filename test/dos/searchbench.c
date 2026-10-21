/* searchbench.c - how long does answering a search take?
 *
 * A hub broadcasts every member's searches to everyone, so on a busy hub
 * this runs constantly, in the same single thread as the UI and the hub
 * connection. TODO.md claimed the linear scans "would want a hash index at
 * 10,000 files" -- this is the measurement that claim deserved, and the
 * check that the index and the letter mask actually earn their keep.
 *
 * Four numbers:
 *
 *   TR indexed  an exact hash search, and what every incoming CGET does.
 *               Compared against the linear scan it replaced, written out
 *               here rather than kept in the client.
 *   AN rejected a name search whose letters no file has: the mask answers
 *               it without touching a single string.
 *   AN full     a name search whose letters every file has but whose text
 *               almost none do, so every path goes through the substring
 *               compare. This is the worst case, and the one the mask is
 *               measured against.
 *   AN limited  a search with many hits, which stops at the reply limit
 *               after a handful of files. Cheap, and worth showing so the
 *               worst case is not mistaken for the common one.
 *
 * Integer arithmetic only: the project is built -msoft-float.
 *
 * Run it on DOS under a cycle-limited DOSBox-X profile
 * (`make dos-bench-run`). The host figures are flattering to the point of
 * being useless -- a modern out-of-order CPU overlaps whole lookups and
 * keeps the table in L1 -- and the decision this informs is about a 486.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../../src/share/index.h"
#include "../../src/adc/tokenize.h"

#define FILES     10000
#define MIN_TICKS (CLOCKS_PER_SEC)      /* measure for at least ~1s */

static share_index ix;

/* Paths that look like a real share: a few deep directories, repetitive
   prefixes, 8.3 names. The prefixes matter -- they are what makes a
   substring search expensive. */
static void build(void)
{
    static const char *const dirs[] = {
        "DL/WAREZ/APPS", "DL/WAREZ/GAMES", "DL/WAREZ/UTILS",
        "DL/MODS/TRACKS", "DL/ANSI/PACKS", "DL/DOCS/NFO",
        "DL/ISO/CD1", "DL/TEMP/INCOMING"
    };
    static const char *const exts[] = { "ZIP", "ARJ", "LHA", "MOD", "NFO" };
    char virt[64];
    char local[72];
    unsigned char tth[TTH_HASH_LEN];
    int i;

    if (share_init(&ix) != 0) {
        printf("out of memory\n");
        return;
    }

    for (i = 0; i < FILES; i++) {
        share_file *f;

        sprintf(virt, "%s/FILE%04d.%s",
                dirs[i % 8], i, exts[i % 5]);
        sprintf(local, "C:\\%.60s", virt);

        f = share_add_file(&ix, virt, local, (tth_u64)(1024 * (i + 1)), 0);
        if (!f) {
            printf("only %d files fitted\n", i);
            break;
        }

        memset(tth, 0, sizeof tth);
        tth[0] = (unsigned char)(i & 0xFF);
        tth[1] = (unsigned char)((i >> 8) & 0xFF);
        tth[2] = (unsigned char)(i & 0x0F);
        tth[23] = (unsigned char)(i & 0xFF);
        share_set_tth(&ix, f, tth);
    }
}

/* The scan share_find_tth used to do. Kept here, in the benchmark, so the
   comparison is against real code rather than an estimate. */
static share_file *find_tth_linear(const unsigned char *tth)
{
    unsigned int i;

    for (i = 0; i < ix.count; i++)
        if (ix.files[i].hashed &&
            memcmp(ix.files[i].tth, tth, TTH_HASH_LEN) == 0)
            return &ix.files[i];
    return NULL;
}

static unsigned long to_ms(clock_t elapsed)
{
    /* CLOCKS_PER_SEC is 1,000,000 on the host and 91 on DJGPP, so this has
       to divide before it multiplies on one and not the other. Doing it in
       this order keeps both inside 32 bits. */
    unsigned long t = (unsigned long)elapsed;
    unsigned long ms = (t / (unsigned long)CLOCKS_PER_SEC) * 1000UL +
                       ((t % (unsigned long)CLOCKS_PER_SEC) * 1000UL) /
                       (unsigned long)CLOCKS_PER_SEC;

    return ms ? ms : 1;
}

/* ops/second without overflowing a 32-bit long: `ops * 1000` passes four
   billion at four million operations, which DJGPP would wrap silently and
   report as a suspiciously fast result. */
static unsigned long per_sec(unsigned long ops, unsigned long ms)
{
    return (ops / ms) * 1000UL + ((ops % ms) * 1000UL) / ms;
}

static void report(const char *what, unsigned long ops, clock_t elapsed)
{
    unsigned long ms = to_ms(elapsed);
    unsigned long rate = per_sec(ops, ms);
    unsigned long ns = rate ? 1000000000UL / rate : 0;

    printf("%-12s: %9lu /s  %8lu ns each   (%lu ops in %lu ms)\n",
           what, rate, ns, ops, ms);
}

/* The hash of file number n, as build() made it. */
static void tth_of(int n, unsigned char *tth)
{
    memset(tth, 0, TTH_HASH_LEN);
    tth[0] = (unsigned char)(n & 0xFF);
    tth[1] = (unsigned char)((n >> 8) & 0xFF);
    tth[2] = (unsigned char)(n & 0x0F);
    tth[23] = (unsigned char)(n & 0xFF);
}

static void bench_tth(void)
{
    unsigned char tth[TTH_HASH_LEN];
    clock_t start, elapsed;
    unsigned long ops;
    int n = 0;

    start = clock();
    ops = 0;
    do {
        int k;
        for (k = 0; k < 256; k++) {
            tth_of(n, tth);
            if (!share_find_tth(&ix, tth))
                printf("  (indexed lookup missed file %d)\n", n);
            n = (n + 7919) % FILES;    /* a prime stride: no locality */
            ops++;
        }
        elapsed = clock() - start;
    } while (elapsed < MIN_TICKS);
    report("TR indexed", ops, elapsed);

    start = clock();
    ops = 0;
    do {
        int k;
        for (k = 0; k < 16; k++) {
            tth_of(n, tth);
            if (!find_tth_linear(tth))
                printf("  (linear lookup missed file %d)\n", n);
            n = (n + 7919) % FILES;
            ops++;
        }
        elapsed = clock() - start;
    } while (elapsed < MIN_TICKS);
    report("TR linear", ops, elapsed);
}

static void bench_match(const char *label, const char *line)
{
    share_file *hits[SHARE_MAX_REPLIES_ACTIVE];
    adc_msg m;
    clock_t start, elapsed;
    unsigned long ops = 0;
    int last = 0;

    if (adc_parse(&m, line) != 0) {
        printf("%-12s: could not parse %s\n", label, line);
        return;
    }

    start = clock();
    do {
        int k;
        for (k = 0; k < 16; k++) {
            last = share_match(&ix, &m, hits, SHARE_MAX_REPLIES_ACTIVE);
            ops++;
        }
        elapsed = clock() - start;
    } while (elapsed < MIN_TICKS);

    report(label, ops, elapsed);
    printf("              (%d hit%s)\n", last, last == 1 ? "" : "s");
}

int main(void)
{
    printf("DOSy++ search benchmark\n");
    printf("building a synthetic share of %d files...\n", FILES);
    fflush(stdout);

    build();
    if (ix.count == 0) {
        printf("FAILED to build the share\n");
        return 1;
    }

    printf("%u files, %u hashed, %u index buckets, %lu bytes of pool\n",
           ix.count, ix.hashed_count, ix.tth_cap,
           (unsigned long)ix.pool_len);
    printf("record size: %lu bytes (%lu KB for the table)\n",
           (unsigned long)sizeof(share_file),
           (unsigned long)(ix.count * sizeof(share_file) / 1024));
    printf("\n");
    fflush(stdout);

    bench_tth();
    printf("\n");

    /* No file has a 'q' or an 'x', so the letter mask answers this without
       looking at a single path. */
    bench_match("AN rejected", "BSCH AAAA ANquxz");

    /* Letters every file has, text almost none do: the mask cannot help,
       so all 10,000 paths get the substring compare. The worst case. */
    bench_match("AN full", "BSCH AAAA ANfile0042");

    /* Many hits, so it stops at the reply limit almost immediately. */
    bench_match("AN limited", "BSCH AAAA ANdl ANfile");

    printf("\nDONE\n");
    share_free(&ix);
    return 0;
}
