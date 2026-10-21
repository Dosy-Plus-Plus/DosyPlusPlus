#!/bin/bash
# Fetch the SoundFont synthesiser the browser demo plays game MIDI through.
#
#   tools/fetchsynth.sh          -> web/vendor/synth/
#
# v86 emulates an MPU-401 that answers a game's probe and discards every
# note (see web/midi.mjs), so a game in the browser detects a Roland and
# plays silence. The page takes the bytes off the port itself; this is what
# turns them into sound.
#
# spessasynth_core: a SoundFont 2/3 synthesiser in plain JavaScript,
# Apache-2.0, no WebAudio dependency -- it renders into Float32Arrays, which
# is why the same code can run in the page's AudioWorklet and in a test rig
# with no audio hardware at all. Its one dependency, stb-vorbis (also
# Apache-2.0), decodes the compressed samples in an .sf3.
#
# Both are hash-pinned. A synthesiser that changed under us would be heard
# rather than reported, and probably not by whoever changed it.
#
# Fetched, not committed, for the same reasons as Watt-32, bzip2, the v86
# engine and the SoundFont itself.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/web/vendor/synth"
CACHE="${DOSYPP_CACHE:-$ROOT/.cache}"

CORE_VERSION="${SYNTH_VERSION:-4.3.22}"
VORBIS_VERSION="${VORBIS_VERSION:-0.0.6}"

CORE_SHA="a94f0bfcc43339d9d3a80a67eaa16178ed26d30283892e836fcd472c2845975b"
VORBIS_SHA="f2c9612be9718c3e93cbee86a5b7c25a901b786ba5542e82e9cb8dad3b11637b"

CDN="https://cdn.jsdelivr.net/npm"

die() { echo "error: $*" >&2; exit 1; }

sha_of() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        sha256sum "$1" | cut -d' ' -f1
    fi
}

get() {
    local url="$1" out="$2" want="$3" what="$4"

    if [ -f "$out" ] && [ "$(sha_of "$out")" = "$want" ]; then
        echo "  have $(basename "$out")  (sha256 verified)"
        return 0
    fi

    local tmp="$CACHE/$(basename "$out")"
    if [ ! -f "$tmp" ] || [ "$(sha_of "$tmp")" != "$want" ]; then
        echo "  get  $(basename "$out")  ($what)"
        curl -fsSL -m 300 -o "$tmp.part" "$url" || die "could not fetch $url"
        mv "$tmp.part" "$tmp"
    fi

    local got
    got="$(sha_of "$tmp")"
    if [ "$got" != "$want" ]; then
        rm -f "$tmp"
        die "$(basename "$out") does not match the hash this script pins.
       wanted $want
       got    $got
       Nothing is installed."
    fi

    cp "$tmp" "$out"
}

mkdir -p "$DEST" "$CACHE"

echo "synthesiser:"
get "$CDN/spessasynth_core@$CORE_VERSION/dist/index.js" \
    "$DEST/spessasynth_core.js" "$CORE_SHA" "SoundFont synth, Apache-2.0"
get "$CDN/stb-vorbis@$VORBIS_VERSION/dist/index.js" \
    "$DEST/stb-vorbis.js" "$VORBIS_SHA" "ogg decoder for .sf3 samples"

# The published bundle has no package manager and no import map, and Node
# resolves bare specifiers from node_modules, which a test rig should not
# need either. One bare import stands in the way, so it is rewritten to a
# relative path -- after the hash check, so what is verified is what npm
# published and what is served is a one-line edit of it.
if grep -q 'from *"stb-vorbis"' "$DEST/spessasynth_core.js"; then
    perl -pi -e 's{from *"stb-vorbis"}{from"./stb-vorbis.js"}g' \
        "$DEST/spessasynth_core.js"
    echo "  + rewrote the bare 'stb-vorbis' import to ./stb-vorbis.js"
elif grep -q 'from"\./stb-vorbis\.js"' "$DEST/spessasynth_core.js"; then
    echo "  (import already relative)"
else
    die "spessasynth_core.js does not import stb-vorbis the way this script
       expects. Check the package before trusting the rewrite."
fi

cat > "$DEST/VERSIONS.txt" <<EOF
spessasynth_core: $CORE_VERSION (npm, Apache-2.0)
stb-vorbis:       $VORBIS_VERSION (npm, Apache-2.0)
fetched:          $(date -u +%Y-%m-%dT%H:%M:%SZ)

The bare "stb-vorbis" import in spessasynth_core.js is rewritten to
./stb-vorbis.js by tools/fetchsynth.sh, so both files work unchanged in a
browser and in Node. Hashes are pinned in that script.
EOF

echo
echo "next: make test-v86-midi   (renders a guest's MIDI and measures it)"
