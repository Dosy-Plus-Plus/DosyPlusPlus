/* hashbench.c - how fast is TTH on this machine?
 *
 * The design assumed roughly 0.3-1 MB/s on a 486 and built scheduling
 * decisions on it: overnight hashing, a resumable hash queue, a progress
 * gauge that has to be honest. That number deserves measuring rather than
 * assuming.
 *
 * Integer arithmetic only -- the whole project is built -msoft-float, so a
 * printf("%f") here would not even link.
 *
 * Builds for host and DOS. On DOS, run it under a cycle-limited DOSBox-X
 * profile; at cycles=max the answer is flattering and useless.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../../src/hash/tth.h"

#define BUF_KB    256
#define MIN_TICKS (CLOCKS_PER_SEC * 2)   /* measure for at least ~2s */

static unsigned char buf[BUF_KB * 1024];

int main(void)
{
    unsigned char root[TTH_HASH_LEN];
    clock_t start, elapsed = 0;
    unsigned long rounds = 0;
    unsigned long kb, ms, kbps;
    size_t i;

    for (i = 0; i < sizeof buf; i++)
        buf[i] = (unsigned char)((i * 37 + 7) % 256);

    printf("DOSy++ TTH benchmark\n");
    printf("hashing %d KB per round, for at least 2 seconds...\n", BUF_KB);
    fflush(stdout);

    start = clock();
    do {
        tth(buf, sizeof buf, root);
        rounds++;
        elapsed = clock() - start;
    } while (elapsed < MIN_TICKS);

    kb = rounds * BUF_KB;
    ms = (unsigned long)elapsed * 1000UL / (unsigned long)CLOCKS_PER_SEC;
    if (ms == 0)
        ms = 1;
    kbps = kb * 1000UL / ms;

    printf("\n");
    printf("rounds      : %lu\n", rounds);
    printf("hashed      : %lu KB\n", kb);
    printf("elapsed     : %lu ms\n", ms);
    printf("throughput  : %lu KB/s (%lu.%02lu MB/s)\n",
           kbps, kbps / 1024, ((kbps % 1024) * 100) / 1024);
    printf("\n");

    /* The number that actually matters for the UI and the share workflow. */
    if (kbps > 0) {
        unsigned long secs_per_gb = 1024UL * 1024UL / kbps;
        printf("1 GB share  : %lu s (%lu min)\n",
               secs_per_gb, secs_per_gb / 60);
    }

    printf("DONE\n");
    return 0;
}
