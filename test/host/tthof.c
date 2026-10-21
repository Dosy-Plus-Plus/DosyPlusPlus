/* tthof.c - print the ADC TTH of each file named, with its size.
 *
 * Our own implementation rather than rhash, on purpose: the hashes this
 * prints end up in SHARE.TXT, which the demo hub advertises and the DOS
 * client verifies after downloading. Using the same code for both ends
 * would hide a bug in it, so tools/mkshare.sh cross-checks against rhash
 * when rhash is installed -- and the cross-check is the thing that would
 * catch it, not the choice of implementation here.
 *
 *   build/host/tthof FILE...      ->  <tth>  <size>  <path>
 *
 * Not in TEST_SRC: it is a tool, not a test, and `make test` should not
 * try to run it.
 */
#include <stdio.h>

#include "hash/tth.h"

int main(int argc, char **argv)
{
    int i, bad = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: tthof FILE...\n");
        return 2;
    }

    for (i = 1; i < argc; i++) {
        unsigned char buf[65536], root[TTH_HASH_LEN];
        char b32[BASE32_TIGER_LEN + 1];
        tth_ctx ctx;
        tth_u64 total = 0;
        size_t n;
        FILE *f = fopen(argv[i], "rb");

        if (!f) {
            fprintf(stderr, "tthof: cannot open %s\n", argv[i]);
            bad = 1;
            continue;
        }

        tth_init(&ctx, NULL, NULL, -1);
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
            tth_update(&ctx, buf, n);
            total += (tth_u64)n;
        }
        if (ferror(f)) {
            fprintf(stderr, "tthof: read error on %s\n", argv[i]);
            bad = 1;
        }
        fclose(f);

        tth_final(&ctx, root);
        tth_to_base32(root, b32);
        printf("%s  %lu  %s\n", b32, (unsigned long)total, argv[i]);
    }

    return bad;
}
