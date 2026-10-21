#!/bin/bash
# Randomised cross-check of our TTH against RHash, an independent
# implementation. The embedded vectors in test_hash.c lock in behaviour;
# this hunts for sizes those vectors do not cover.
#
#   ./test/host/crosscheck_rhash.sh [count]
#   ./test/host/crosscheck_rhash.sh --print-vectors   # regenerate the table
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

command -v rhash >/dev/null 2>&1 || {
    echo "rhash not found (brew install rhash)" >&2; exit 1; }

HELPER="$(mktemp -d)/tthof"
trap 'rm -rf "$(dirname "$HELPER")" /tmp/dosypp-cc.bin' EXIT

# Small driver around our implementation: prints the base32 TTH of a file.
cat > "$HELPER.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include "hash/tth.h"
int main(int argc, char **argv)
{
    unsigned char buf[65536], root[TTH_HASH_LEN];
    char b32[BASE32_TIGER_LEN + 1];
    tth_ctx ctx;
    size_t n;
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : stdin;
    if (!f) return 2;
    tth_init(&ctx, NULL, NULL, -1);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        tth_update(&ctx, buf, n);
    tth_final(&ctx, root);
    tth_to_base32(root, b32);
    printf("%s\n", b32);
    return 0;
}
EOF
${CC:-cc} -O2 -I src -o "$HELPER" "$HELPER.c" \
    src/hash/tth.c src/hash/tiger.c src/hash/base32.c || exit 1

# The filler MUST match fill(buf, n, 37) in test_hash.c.
gen() { python3 -c "import sys;sys.stdout.buffer.write(bytes((i*37+7)%256 for i in range($1)))"; }

if [ "${1:-}" = "--print-vectors" ]; then
    for sz in 0 1 1023 1024 1025 2048 2049 3072 4096 5000; do
        gen "$sz" > /tmp/dosypp-cc.bin
        printf '        { %5d, "%s" },\n' "$sz" \
            "$(rhash --tth /tmp/dosypp-cc.bin | cut -d' ' -f1 | tr 'a-z' 'A-Z')"
    done
    exit 0
fi

COUNT="${1:-40}"
fail=0

check() {
    local sz="$1"
    gen "$sz" > /tmp/dosypp-cc.bin
    local ours want
    ours="$("$HELPER" /tmp/dosypp-cc.bin)"
    want="$(rhash --tth /tmp/dosypp-cc.bin | cut -d' ' -f1 | tr 'a-z' 'A-Z')"
    if [ "$ours" = "$want" ]; then
        printf 'ok    %-9s %s\n' "$sz" "$ours"
    else
        printf 'FAIL  %-9s ours=%s rhash=%s\n' "$sz" "$ours" "$want"
        fail=1
    fi
}

echo "--- leaf-boundary sizes"
for sz in 0 1 1024 1025 2047 2048 2049 4095 4096 4097 8192 8193; do
    check "$sz"
done

echo "--- random sizes"
for _ in $(seq "$COUNT"); do
    check "$((RANDOM * 32 % 200000))"
done

echo
if [ "$fail" -eq 0 ]; then
    echo "ok: our TTH agrees with rhash everywhere"
else
    echo "FAILED: divergence from rhash"
fi
exit $fail
