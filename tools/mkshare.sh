#!/bin/bash
# Build the share the demo hub serves, with its hashes precomputed.
#
#   tools/mkshare.sh              -> build/warez + build/warez/SHARE.TXT
#
# Why precomputed: the client verifies every download against the TTH the
# hub advertised, so the hub has to know it. Python has no Tiger in its
# standard library, and the demo hub is meant to run on a visitor's machine
# with nothing but python3 -- so the hashing happens here, at build time,
# with our own implementation (build/host/tthof), and the hub just reads the
# answers. When rhash is installed every hash is cross-checked against it:
# a hub advertising a hash our client then disagrees with would fail the
# download in a way that looks like a transfer bug.
#
# The files are the free DOS packages tools/fetchatmos.sh fetches. Nothing
# here is invented filler: the point of the demo is downloading something
# real and running it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ATMOS="${ATMOS_DIR:-$ROOT/build/atmos}"
OUT="${OUT:-$ROOT/build/warez}"
TTHOF="$ROOT/build/host/tthof"

# Per-file and total caps. Everything here is published to GitHub Pages and
# downloaded by whoever runs the demo hub, so this is a budget and not an
# invitation.
#
# Sized to fit the game with music and not much more. BOOM is a complete
# free Doom -- the GPL engine, its own CWSDPMI and a 17.6MB WAD, all in one
# 7.8MB zip -- and it drives an MPU-401, which is the point of the browser
# demo's MIDI support.
#
# FreeDoom's own 27MB package is left out by default, and this is why:
# BOOM already contains a playable free Doom, so FreeDoom adds no game, and
# 27MB of incompressible zip is the largest single thing in what gets
# published. SHARE_FULL=1 puts it (and everything else that fits under
# 30MB) back:
#
#     SHARE_FULL=1 make warez
#
# A 486 in a browser tab really does unzip 19MB of Doom in a few seconds,
# which was the other reason to doubt any of this.
if [ "${SHARE_FULL:-0}" = "1" ]; then
    MAX_FILE="${MAX_FILE:-30000000}"
    BUDGET="${BUDGET:-60000000}"
else
    MAX_FILE="${MAX_FILE:-9000000}"
    BUDGET="${BUDGET:-25000000}"
fi

die() { echo "error: $*" >&2; exit 1; }

[ -d "$ATMOS" ] || die "no packages in $ATMOS
       run: make atmos      (tools/fetchatmos.sh)"
[ -x "$TTHOF" ] || die "no $TTHOF
       run: make tthof"

RHASH="$(command -v rhash || true)"

rm -rf "$OUT"
mkdir -p "$OUT"

budget=$BUDGET
added=0
skipped=0

# Deterministic order, so two builds of the same packages produce the same
# share and SHARE.TXT diffs mean something.
while IFS= read -r src; do
    rel="${src#"$ATMOS"/}"
    case "$rel" in
        README.TXT) continue ;;        # about the fetch, not part of a share
    esac

    sz=$(wc -c < "$src" | tr -d ' ')
    if [ "$sz" -gt "$MAX_FILE" ] || [ "$sz" -gt "$budget" ]; then
        skipped=$((skipped + 1))
        printf '  - %-28s %s (too big for the demo share)\n' "$rel" \
            "$(echo "$sz" | awk '{printf "%.1fM", $1/1048576}')"
        continue
    fi

    # Directories lower case, file names upper: what a 1994 file area
    # looked like, and what the invented listing in stubhub.py already uses.
    dir="$(dirname "$rel" | tr 'A-Z' 'a-z')"
    base="$(basename "$rel")"
    [ "$dir" = "." ] && dest="$base" || dest="$dir/$base"

    mkdir -p "$OUT/$(dirname "$dest")"
    cp "$src" "$OUT/$dest"

    budget=$((budget - sz))
    added=$((added + 1))
done <<EOF
$(find "$ATMOS" -type f | LC_ALL=C sort)
EOF

[ "$added" -gt 0 ] || die "nothing small enough to share from $ATMOS"

# ---------------------------------------------------------------- the hashes

{
    echo "# The demo hub's share: what it advertises, and what the client"
    echo "# checks each download against."
    echo "#"
    echo "# <tth(base32)>  <bytes>  <name>"
    echo "#"
    echo "# Written by tools/mkshare.sh using build/host/tthof -- our own"
    echo "# TTH, the same code the DOS client verifies with."
} > "$OUT/SHARE.TXT"

bad=0
while IFS= read -r f; do
    rel="${f#"$OUT"/}"
    line="$("$TTHOF" "$f")" || die "tthof failed on $rel"
    tth="${line%%  *}"
    sz="$(wc -c < "$f" | tr -d ' ')"

    if [ -n "$RHASH" ]; then
        want="$("$RHASH" --tth "$f" | cut -d' ' -f1 | tr 'a-z' 'A-Z')"
        if [ "$tth" != "$want" ]; then
            echo "  !! $rel: our TTH $tth, rhash says $want" >&2
            bad=$((bad + 1))
            continue
        fi
    fi

    printf '%s  %s  %s\n' "$tth" "$sz" "$rel" >> "$OUT/SHARE.TXT"
    printf '  + %-28s %7sK  %s\n' "$rel" "$((sz / 1024))" "${tth:0:12}..."
done <<EOF
$(find "$OUT" -type f ! -name SHARE.TXT | LC_ALL=C sort)
EOF

[ "$bad" -eq 0 ] || die "$bad file(s) hashed differently by rhash -- the hub
       would advertise a hash the client then rejects. Not shipping that."

echo
echo "share:   $OUT  ($added file(s), $skipped too big)"
if [ -n "$RHASH" ]; then
    echo "hashes:  ours, each cross-checked against rhash"
else
    echo "hashes:  ours (rhash not installed, so nothing cross-checked them)"
fi
