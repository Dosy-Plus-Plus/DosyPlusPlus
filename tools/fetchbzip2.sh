#!/bin/bash
# Fetch libbzip2's sources into third_party/bzip2/.
#
# Why bzip2 at all: an ADC file list is `files.xml.bz2`, and the name is not
# decoration -- every other client compresses and expects compression. Browse
# does not work without it in either direction.
#
# Why vendor rather than write one: bzip2 is a Burrows-Wheeler transform, a
# move-to-front coder and multi-table Huffman. Reimplementing that correctly
# is weeks of work with a high chance of producing something subtly wrong
# that only fails against other clients. libbzip2 is plain C89 with no
# dynamic allocation of its own (it calls a supplied allocator), which is
# about as good a fit for DJGPP as third-party code gets.
#
# Fetched, not committed: the project does the same for Watt-32 and the
# packet drivers. The licence is the bzip2 BSD-style licence -- see
# third_party/bzip2/LICENSE once this has run.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/third_party/bzip2"
CACHE="${DOSYPP_CACHE:-$ROOT/.cache}"

VER=1.0.8
TARBALL="bzip2-$VER.tar.gz"
URL="${BZIP2_URL:-https://sourceware.org/pub/bzip2/$TARBALL}"
SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269

# Only the library. bzip2.c (the command line tool), bzip2recover.c and the
# test files are not wanted: we link the library into DOSYPP.EXE.
WANT=(
    bzlib.h bzlib_private.h
    bzlib.c blocksort.c compress.c decompress.c huffman.c
    crctable.c randtable.c
    LICENSE
)

have_all() {
    local f
    for f in "${WANT[@]}"; do
        [ -f "$DEST/$f" ] || return 1
    done
    return 0
}

if have_all; then
    echo "bzip2 $VER: already in third_party/bzip2"
    exit 0
fi

mkdir -p "$CACHE" "$DEST"

if [ ! -f "$CACHE/$TARBALL" ]; then
    echo "bzip2 $VER: fetching $TARBALL"
    curl -fsSL "$URL" -o "$CACHE/$TARBALL.part"
    mv "$CACHE/$TARBALL.part" "$CACHE/$TARBALL"
fi

# Checked, because this is source code that gets compiled into the client.
if command -v shasum >/dev/null 2>&1; then
    got=$(shasum -a 256 "$CACHE/$TARBALL" | cut -d' ' -f1)
elif command -v sha256sum >/dev/null 2>&1; then
    got=$(sha256sum "$CACHE/$TARBALL" | cut -d' ' -f1)
else
    got="$SHA256"
    echo "  WARNING: no shasum or sha256sum; cannot verify the tarball" >&2
fi

if [ "$got" != "$SHA256" ]; then
    echo "bzip2 tarball does not match its expected hash:" >&2
    echo "  got      $got" >&2
    echo "  expected $SHA256" >&2
    echo "refusing to unpack it" >&2
    rm -f "$CACHE/$TARBALL"
    exit 1
fi

echo "bzip2 $VER: sha256 verified; unpacking the library sources"
for f in "${WANT[@]}"; do
    tar -xzf "$CACHE/$TARBALL" -O "bzip2-$VER/$f" > "$DEST/$f.part"
    mv "$DEST/$f.part" "$DEST/$f"
    echo "  +    $f"
done

cat > "$DEST/README.dosypp" <<EOF
libbzip2 $VER, fetched by tools/fetchbzip2.sh -- not part of DOSy++.

Only the library sources are here; the command line tools are not needed.
See LICENSE for the terms (BSD-style). Compiled with warnings off, because
patching upstream warnings is how you end up maintaining a fork.
EOF

echo "bzip2 $VER: ready"
