#!/bin/bash
# Assemble the static bundle that gets published to GitHub Pages.
#
# Everything the browser needs and nothing it does not: the page, the v86
# engine, the two disk images, and -- because a browser cannot open a TCP
# socket -- the two Python scripts that make up the other half of the demo.
#
#   tools/mkpages.sh            -> build/pages
#   OUT=/tmp/pages tools/mkpages.sh
#
# This exists rather than publishing web/ directly so that what CI uploads
# can be built and served locally, byte for byte, before it is published.
# A demo that only works in CI is not a demo anybody can fix.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-$ROOT/build/pages}"

die() { echo "error: $*" >&2; exit 1; }

need() {
    [ -f "$1" ] || die "missing $1
       $2"
}

need "$ROOT/web/index.html"    "the page itself is part of the repo"
need "$ROOT/web/relayurl.mjs" "the page imports it"
need "$ROOT/web/midi.mjs"     "the page imports it"
need "$ROOT/web/synth.mjs"    "the MIDI worklet imports it"
need "$ROOT/web/midiworklet.mjs" "the page loads it as an AudioWorklet"
# Vendored and fetched rather than committed, so fetch them: telling a CI
# runner to "run tools/fetchsynth.sh" is advice nobody is there to take,
# and a bundle is not publishable without them. Both fetchers are
# sha256-pinned and exit early if the files are already there.
[ -f "$ROOT/web/vendor/synth/spessasynth_core.js" ] || "$ROOT/tools/fetchsynth.sh"
[ -f "$ROOT/web/vendor/v86.wasm" ] || "$ROOT/tools/fetchv86.sh"

need "$ROOT/web/vendor/synth/spessasynth_core.js" "tools/fetchsynth.sh ran and did not produce it"
need "$ROOT/web/vendor/v86.wasm" "tools/fetchv86.sh ran and did not produce it"
need "$ROOT/web/images/dosypp.img" "run: make images"
need "$ROOT/web/images/boot.img"   "run: make images"
need "$ROOT/build/warez/SHARE.TXT" "run: make warez"

# Where the published page will live. demo.sh has to name it absolutely --
# a script arriving down a pipe does not know where it came from -- so a
# bundle built without this says so rather than shipping a broken URL.
BASE="${PAGES_BASE:-}"
if [ -z "$BASE" ]; then
    BASE="http://127.0.0.1:8000/"
    echo "note: PAGES_BASE not set, so relay/demo.sh will fetch from"
    echo "      $BASE (fine for a local check, wrong for publishing)"
fi
case "$BASE" in */) ;; *) BASE="$BASE/" ;; esac

rm -rf "$OUT"
mkdir -p "$OUT/vendor" "$OUT/images" "$OUT/relay" "$OUT/attic"

cp "$ROOT/web/index.html" "$OUT/index.html"

# The icon, which the page links and every browser asks for anyway: a
# bundle without it answers /favicon.ico with a 404 for every visitor.
# Drawn by tools/mkfavicon.py, so it is generated rather than committed --
# the same arrangement as art/DOSYPP.MOD.
[ -f "$ROOT/web/favicon.ico" ] || "$ROOT/tools/mkfavicon.py" >/dev/null
need "$ROOT/web/favicon.ico" "tools/mkfavicon.py ran and produced nothing"
cp "$ROOT/web/favicon.ico" "$ROOT/web/favicon.png" "$OUT/"
cp "$ROOT/web/relayurl.mjs" "$ROOT/web/midi.mjs" "$ROOT/web/synth.mjs" \
   "$ROOT/web/midiworklet.mjs" "$ROOT/web/webmidi.mjs" \
   "$ROOT/web/wisp.mjs" "$ROOT/web/adchub.mjs" "$ROOT/web/disk.mjs" \
   "$ROOT/web/fitscreen.mjs" "$OUT/"

# The SoundFont synthesiser that plays game MIDI, and the SoundFont it
# plays through. The SoundFont is 40MB and is fetched by the page only when
# somebody presses Enable MIDI, so it costs nothing to the visitors who
# never do.
mkdir -p "$OUT/vendor/synth" "$OUT/sf2"
cp "$ROOT/web/vendor/synth/"*.js "$OUT/vendor/synth/"
[ -f "$ROOT/web/vendor/synth/VERSIONS.txt" ] &&
    cp "$ROOT/web/vendor/synth/VERSIONS.txt" "$OUT/vendor/synth/"

# The SoundFont is 40MB and incompressible, and it is the single biggest
# thing here. It is fetched by the page only if somebody presses "play it
# here" -- the other two ways to hear game MIDI (your own SoundFont file,
# or Web MIDI out to a synth you own) cost nothing. PAGES_SF=none leaves it
# out, and the page then offers only those two.
SF="${DOSYPP_SF2:-$ROOT/dosbox-x/sf2/MuseScore_General.sf3}"
if [ "${PAGES_SF:-include}" = "none" ]; then
    echo "note: PAGES_SF=none, so the page will offer Web MIDI and your own"
    echo "      SoundFont, and no 40MB download."
    SF=""
fi
if [ -n "$SF" ] && [ -f "$SF" ]; then
    cp "$SF" "$OUT/sf2/MuseScore_General.sf3"
    [ -f "$(dirname "$SF")/README.txt" ] &&
        cp "$(dirname "$SF")/README.txt" "$OUT/sf2/"
else
    echo "note: no SoundFont at $SF, so Enable MIDI in the page will have"
    echo "      nothing to fetch (visitors can still pick their own file)."
    echo "      run: make sf2"
fi
cp "$ROOT/web/vendor/"libv86.mjs "$ROOT/web/vendor/"v86.wasm \
   "$ROOT/web/vendor/"seabios.bin "$ROOT/web/vendor/"vgabios.bin "$OUT/vendor/"
[ -f "$ROOT/web/vendor/VERSIONS.txt" ] &&
    cp "$ROOT/web/vendor/VERSIONS.txt" "$OUT/vendor/"
# The boot floppy goes whole: 1.4MB, and v86 wants it in one piece.
cp "$ROOT/web/images/boot.img" "$OUT/images/"

# The hard disk goes in zstd-compressed pieces. 128MB of mostly-empty
# FAT16 publishes as about 9MB, and the browser fetches whole small files
# instead of byte ranges -- so the demo no longer needs the host to honour
# Range requests either. v86 reads this layout natively (`use_parts`); see
# tools/splitimg.mjs for the naming, which is v86's and not ours.
#
# PAGES_WHOLE_IMAGE=1 publishes the single 128MB file instead, for a host
# that would rather serve one object than two hundred.
if [ "${PAGES_WHOLE_IMAGE:-0}" = "1" ]; then
    cp "$ROOT/web/images/dosypp.img" "$OUT/images/"
    echo "image:   one file, $(du -h "$OUT/images/dosypp.img" | cut -f1)"
else
    node "$ROOT/tools/splitimg.mjs" "$ROOT/web/images/dosypp.img" \
        "$OUT/images" "${PAGES_CHUNK:-1048576}"
fi

# The network half. A page served from Pages can open a WebSocket and
# nothing else, so the TCP end has to run on the visitor's own machine --
# which means it has to be downloadable from the same place as the page.
# Both are single files with no dependencies beyond python3 on purpose.
cp "$ROOT/tools/wisprelay.py" "$ROOT/tools/stubhub.py" "$OUT/relay/"
chmod +x "$OUT/relay/wisprelay.py" "$OUT/relay/stubhub.py"

# The share, unpacked. Both routes read it from here: the hub in the page
# fetches a file (by range) only when the client asks for it, and
# relay/demo.sh downloads the lot for the Python hub using SHARE.TXT as
# its manifest.
#
# It used to be published twice -- unpacked for nothing and zipped for
# demo.sh -- which was 22MB of duplication for the convenience of one
# curl. SHARE.TXT already lists every file, so the zip is gone.
cp -R "$ROOT/build/warez/." "$OUT/attic/"
echo "share:   $(find "$OUT/attic" -type f ! -name SHARE.TXT | wc -l | tr -d ' ') file(s) unpacked for both routes"

cat > "$OUT/relay/demo.sh" <<SH
#!/bin/sh
# The other half of the DOSy++ browser demo: a hub with a real share, and
# the Wisp relay that gives a browser tab a TCP network.
#
# Everything it downloads comes from $BASE and nothing else. It writes to
# ./dosypp-demo, binds two ports on 127.0.0.1, and stops when you do.
#
#   curl -fsSL ${BASE}relay/demo.sh | sh
#
# Or read it first, which is the better habit:
#   curl -fsSLO ${BASE}relay/demo.sh && less demo.sh && sh demo.sh
set -eu

BASE="\${BASE:-$BASE}"
DIR="\${DIR:-\$PWD/dosypp-demo}"
HUB_PORT="\${HUB_PORT:-1511}"
ATTIC_PORT="\${ATTIC_PORT:-1512}"
RELAY_PORT="\${RELAY_PORT:-8086}"

command -v python3 >/dev/null 2>&1 || {
    echo "This needs python3 (3.8 or newer)." >&2; exit 1; }


mkdir -p "\$DIR"
cd "\$DIR"
echo "working in \$DIR"

get() {
    [ -f "\$2" ] && { echo "  have \$2"; return 0; }
    echo "  get  \$2"
    curl -fsSL "\${BASE}relay/\$1" -o "\$2.part"
    mv "\$2.part" "\$2"
}

get wisprelay.py wisprelay.py
get stubhub.py stubhub.py

# The share, from its own manifest. No archive: the files are published
# unpacked because the hub in the page fetches them one at a time, and
# SHARE.TXT lists every one with the hash the client will check.
mkdir -p attic
if [ ! -f attic/SHARE.TXT ]; then
    echo "  get  attic/SHARE.TXT"
    curl -fsSL "\${BASE}attic/SHARE.TXT" -o attic/SHARE.TXT.part
    mv attic/SHARE.TXT.part attic/SHARE.TXT
fi

while read -r tth size name; do
    case "\$tth" in ""|\#*) continue ;; esac
    [ -f "attic/\$name" ] && continue
    echo "  get  attic/\$name  (\$((size / 1024))K)"
    mkdir -p "attic/\$(dirname "\$name")"
    curl -fsSL "\${BASE}attic/\$name" -o "attic/\$name.part" || {
        echo "could not fetch attic/\$name" >&2; exit 1; }
    mv "attic/\$name.part" "attic/\$name"
done < attic/SHARE.TXT

echo
echo "starting the hub on 127.0.0.1:\$HUB_PORT"
python3 stubhub.py --port "\$HUB_PORT" --motd-ansi \\
    --serve attic --serve-ip 10.77.0.2 --serve-port "\$ATTIC_PORT" \\
    > hub.log 2>&1 &
HUB=\$!
trap 'kill \$HUB 2>/dev/null || true' EXIT INT TERM
sleep 1

kill -0 "\$HUB" 2>/dev/null || {
    echo "the hub did not start:" >&2; cat hub.log >&2; exit 1; }

cat <<MSG

  Now put this in the page's relay box and press Boot:

      wisp://127.0.0.1:\$RELAY_PORT/

  Then: press 3, type BLKDROP, ENTER, ENTER on the result, 4 to watch it
  arrive. /quit and PLAY BLKDROP to play it.

  Hub log: \$DIR/hub.log      Ctrl-C here to stop both.

MSG

exec python3 wisprelay.py --port "\$RELAY_PORT" \\
    --map "10.77.0.1=127.0.0.1:\$HUB_PORT" \\
    --map "10.77.0.2=127.0.0.1:\$ATTIC_PORT"
SH
chmod +x "$OUT/relay/demo.sh"

# Pages runs Jekyll over the upload otherwise, which eats anything that
# looks like a template and every directory beginning with an underscore.
touch "$OUT/.nojekyll"

# What the page shows in its footer, so a visitor can tell a fresh build
# from a cached tab, and so a bug report names a build.
V86VER="$(sed -n 's/^v86 engine: *//p' "$ROOT/web/vendor/VERSIONS.txt" \
          2>/dev/null | head -1)"
# The size of the emulated drive, which is not the size of what was
# published: in pieces, a 128MB disk is about 9MB of files.
IMGBYTES="$(wc -c < "$ROOT/web/images/dosypp.img" | tr -d ' ')"
PUBBYTES="$(du -sk "$OUT" | cut -f1)"
GITREV="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"

# Where to send somebody who wants the source. From the environment in CI,
# from the checkout otherwise, and left empty rather than guessed -- the
# page omits the link when it does not know.
REPO="${PAGES_REPO:-}"
if [ -z "$REPO" ] && [ -n "${GITHUB_REPOSITORY:-}" ]; then
    REPO="${GITHUB_SERVER_URL:-https://github.com}/$GITHUB_REPOSITORY"
fi
if [ -z "$REPO" ]; then
    REPO="$(git -C "$ROOT" remote get-url origin 2>/dev/null || true)"
    # git@github.com:owner/repo.git is not a URL a browser can follow.
    case "$REPO" in
        git@*:*) REPO="https://${REPO#git@}"
                 REPO="${REPO/:/\/}" ;;
    esac
    REPO="${REPO%.git}"
fi

cat > "$OUT/build.json" <<JSON
{
  "built": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "rev": "$GITREV",
  "v86": "${V86VER:-unknown}",
  "disk_bytes": $IMGBYTES,
  "published_kb": $PUBBYTES,
  "repo": "$REPO"
}
JSON

# This project's own terms, beside the page that is published under them.
if [ -f "$ROOT/LICENSE" ]; then
    cp "$ROOT/LICENSE" "$OUT/LICENSE.txt"
else
    echo "error: no LICENSE in the repository -- refusing to publish a" >&2
    echo "       bundle whose own terms are unstated." >&2
    exit 1
fi

# What is bundled and under what licence, read out of the packages
# themselves. The page links it, and so does the hub the page runs.
"$ROOT/tools/mkcredits.sh" "$OUT/CREDITS.txt" || {
    echo "error: refusing to publish a share that cannot be accounted for" >&2
    exit 1
}

cat > "$OUT/relay/README.txt" <<'TXT'
The other half of the DOSy++ browser demo
=========================================

The page in your browser emulates a 486 running the real DOS binary. What a
browser cannot do is open a TCP connection, so the DOS guest's network comes
out as one WebSocket (the Wisp protocol) and something on your machine has to
turn that back into TCP. That something is wisprelay.py.

Python 3.8 or newer. Nothing to install: the two Python files here use
only the standard library.

The quick way
-------------

    sh demo.sh

It fetches these files and the share, starts both, and tells you what to put
in the page's relay box.

By hand
-------

    python3 stubhub.py --port 1511 --motd-ansi \
            --serve attic --serve-ip 10.77.0.2 --serve-port 1512 &
    python3 wisprelay.py --port 8086 \
            --map 10.77.0.1=127.0.0.1:1511 \
            --map 10.77.0.2=127.0.0.1:1512

Then put wisp://127.0.0.1:8086/ in the page's relay box and press Boot.
(The page accepts ws:// too and converts it: v86 selects its network
adapter from the scheme, and only wisp:// and wisps:// mean Wisp.)

stubhub.py is a small ADC hub: enough of the protocol for a client to log in,
chat, search and transfer. --serve makes one node on it real -- it serves the
files in attic/ over the client-to-client half of ADC, with the hashes from
attic/SHARE.TXT, which is what the client verifies each download against.

Why the odd address: v86 hands the relay the dotted quad the guest dialled,
as a hostname. So the guest dials 10.77.0.1, and --map says what that
really means. The DOS side never learns the hub's real address, and needs no
DNS.

Your own hub, including a TLS one
---------------------------------

    python3 wisprelay.py --port 8086 --map 10.77.0.1=hub.example.net:411

ADCS hubs work the same way, with the relay doing the TLS -- that is what
the :tls on the end of a mapping means:

    python3 wisprelay.py --port 8086 \
            --map 10.77.0.1=hub.example.net:2780:tls

There is no crypto in the DOS binary. The relay is the TLS endpoint, which
means the relay sees your traffic in the clear -- run it yourself and that is
your own machine, which is the point of it being here rather than hosted.

What the relay can reach
------------------------

Only what --map names. There is a --allow-direct switch that lets the guest
dial anything; it is off by default, because a WebSocket server that will
connect anywhere on behalf of whoever opens a page is an open proxy.
TXT

echo "bundle: $OUT  ($(du -sh "$OUT" | cut -f1))"
# The disk pieces are summarised rather than listed: there are over a
# hundred of them and printing every one buries everything else, including
# the line that says how to run it.
( cd "$OUT" && find . -type f ! -name '*.img.zst' | sort | while read -r f; do
    printf '  %-28s %8s\n' "${f#./}" \
        "$(wc -c < "$f" | awk '{printf "%.0fK", $1/1024}')"
  done
  n=$(find . -name '*.img.zst' | wc -l | tr -d ' ')
  if [ "$n" -gt 0 ]; then
      k=$(find . -name '*.img.zst' -exec cat {} + | wc -c |
          awk '{printf "%.0fK", $1/1024}')
      printf '  %-28s %8s\n' "images/dosypp-*.img.zst ($n pieces)" "$k"
  fi )
echo
echo "serve it: python3 -m http.server -d $OUT 8000"
