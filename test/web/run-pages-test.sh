#!/bin/bash
# Check the bundle that gets published to GitHub Pages, as published.
#
# Three claims, each of which has a way of being wrong that nobody notices
# until a stranger opens the page:
#
#   1. It is complete and servable by a plain static host. GitHub Pages
#      sets no special headers, runs no code and will not be fixed by a
#      local tweak, so the bundle is fetched over HTTP here -- including a
#      Range request, which is how the 64MB disk image is read in blocks.
#
#   2. It needs no cross-origin isolation. v86 builds with shared memory
#      require SharedArrayBuffer, which requires COOP/COEP headers, which
#      Pages cannot send. This checks the engine in the bundle is not one
#      of those -- the single fact the whole plan rests on.
#
#   3. The demo in it actually runs. relay/demo.sh is fetched from the
#      served bundle and run, and then the bundle's own disk image is
#      booted against it and told to download a game. Streamed, because
#      that is how a visitor's browser will read it.
set -uo pipefail

# mtools asks about a name clash, and asks on stdin: an mmd for a
# directory that exists, or an mcopy without -o, waits for an answer that
# no rig is going to give. Non-interactive runs get EOF and carry on;
# a person running this in a terminal waits for ever, with the question
# usually hidden by a 2>/dev/null. One line removes the whole class.
exec < /dev/null


ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUNDLE="${BUNDLE:-$ROOT/build/pages}"
RUN="$ROOT/build/pagestest"

# Not the defaults: a visitor's demo uses 1511/1512/8086 and this must not
# land on top of one somebody is running.
WEB_PORT=18100
HUB_PORT=11521
ATTIC_PORT=11522
RELAY_PORT=18101

command -v node >/dev/null 2>&1 || { echo "need node" >&2; exit 1; }
command -v mcopy >/dev/null 2>&1 || { echo "need mtools" >&2; exit 1; }
command -v curl >/dev/null 2>&1 || { echo "need curl" >&2; exit 1; }

[ -d "$BUNDLE" ] || { echo "no bundle at $BUNDLE -- run: make pages" >&2; exit 1; }

export MTOOLS_SKIP_CHECK=1
rm -rf "$RUN"; mkdir -p "$RUN"

FAILED=0
fail() { echo "FAIL: $*"; FAILED=1; }

cleanup() {
    for pid in ${WEB_PID:-} ${DEMO_PID:-}; do
        # wait after kill, or bash announces each one as Terminated and
        # the last thing on screen is noise rather than the verdict.
        [ -n "$pid" ] && { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; }
    done
    pkill -f "stubhub.py --port $HUB_PORT" 2>/dev/null
    pkill -f "wisprelay.py --port $RELAY_PORT" 2>/dev/null
}
trap cleanup EXIT

# The page's own modules, which no amount of booting an emulator would
# exercise: the relay URL scheme (a ws:// URL silently selects a network
# adapter that is not Wisp), the MIDI stream reassembly, and the Web MIDI
# sink. Cheap, so they run first.
for t in test-relayurl test-midistream test-webmidi test-disk \
         test-fitscreen; do
    node "$ROOT/test/web/$t.mjs" > "$RUN/$t.log" 2>&1 &&
        printf '  %-18s %s\n' "$t" "$(tail -1 "$RUN/$t.log")" ||
        { fail "$t failed"; sed 's/^/    /' "$RUN/$t.log"; }
done

# --------------------------------------------------- 1. served by a dumb host

# test/web/rangeserver.py rather than python3 -m http.server: the stock
# one answers a Range request with the whole file and a 200, which is the
# one behaviour this rig has to be able to tell apart. See its docstring
# for how to check the real host.
python3 "$ROOT/test/web/rangeserver.py" "$BUNDLE" "$WEB_PORT" \
    > "$RUN/web.log" 2>&1 &
WEB_PID=$!
for _ in $(seq 20); do
    curl -fsS -o /dev/null "http://127.0.0.1:$WEB_PORT/index.html" \
        2>/dev/null && break
    sleep 0.25
done

BASE="http://127.0.0.1:$WEB_PORT/"

echo "--- the bundle, over plain HTTP ---"

# Everything the page asks for by a relative path. Taken from the page
# rather than from a list here, so a new asset cannot be forgotten.
REFS=$(grep -oE '(url|src|href|wasm_path)["'"'"']?[:=] *["'"'"']\.?/?[A-Za-z0-9_./-]+' \
       "$BUNDLE/index.html" |
       grep -oE '[A-Za-z0-9_./-]+$' |
       grep -vE '^(https?|#)' | sort -u)
# ES module imports are not src= attributes, and the page itself and the
# engine are the two files whose absence is hardest to misread. Added by
# name rather than by pattern.
REFS="$REFS build.json index.html vendor/libv86.mjs"
# Loaded by name from JavaScript rather than referenced by an attribute:
# the AudioWorklet module, the SoundFont the MIDI panel fetches, and the
# synthesiser they both need.
# synth.mjs is imported by the worklet, not by the page, so scanning the
# page cannot find it -- and a bundle missing it would publish a MIDI
# button that fails only once somebody presses it.
# The share the hub in the page serves, and its manifest: fetched by name
# at runtime, so scanning the page cannot find them.
REFS="$REFS attic/SHARE.TXT attic/warez/games/BLKDROP.ZIP CREDITS.txt"
REFS="$REFS LICENSE.txt"
REFS="$REFS midiworklet.mjs synth.mjs vendor/synth/spessasynth_core.js"
REFS="$REFS vendor/synth/stb-vorbis.js"
[ -f "$BUNDLE/sf2/MuseScore_General.sf3" ] &&
    REFS="$REFS sf2/MuseScore_General.sf3"

# The disk, in whichever form this bundle publishes. The manifest is what
# the page asks for first; the pieces are named by v86's own convention,
# so the first and last are checked by name -- a naming mistake 404s every
# fetch with nothing to say why.
if [ -f "$BUNDLE/images/dosypp.img.json" ]; then
    REFS="$REFS images/dosypp.img.json"
    CHUNK=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["chunk"])' \
            "$BUNDLE/images/dosypp.img.json")
    PARTS=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["parts"])' \
            "$BUNDLE/images/dosypp.img.json")
    LAST=$(( (PARTS - 1) * CHUNK ))
    REFS="$REFS images/dosypp-0-$CHUNK.img.zst"
    REFS="$REFS images/dosypp-$LAST-$((LAST + CHUNK)).img.zst"
else
    REFS="$REFS images/dosypp.img"
fi
REFS="$REFS $(grep -oE 'from +"\./[A-Za-z0-9_./-]+"' "$BUNDLE/index.html" |
              sed -E 's/^from +"\.\///; s/"$//' | sort -u)"
REFS=$(echo "$REFS" | tr ' ' '\n' | sed 's|^\./||' | sort -u)

# Two kinds of thing the scan finds that are not files to check:
#
#   - a bare directory, from a path the page builds at runtime
#     ("images/" + whatever the manifest names)
#   - images/dosypp.img, which the page uses only if there is no manifest.
#     A bundle that publishes pieces does not have it, and should not.
REFS=$(echo "$REFS" | grep -v '/$')
if [ -f "$BUNDLE/images/dosypp.img.json" ]; then
    REFS=$(echo "$REFS" | grep -vx 'images/dosypp.img')
fi

for ref in $REFS; do
    case "$ref" in
        ""|"#"*|http*) continue ;;
        # A mailto: link is not a file; the address itself is checked below.
        mailto*) continue ;;
    esac
    code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE$ref")
    if [ "$code" = "200" ]; then
        printf '  %-24s %s\n' "$ref" "ok"
    else
        fail "$ref: HTTP $code from a plain static server"
    fi
done

# What form of the disk would the page ask this bundle for? The page's own
# chooser, against the bundle over HTTP, because the failure this guards
# against was invisible to every other check here: the bundle was complete
# and correct, and the page asked it for a file it does not publish.
# The module path is passed in: a script read from stdin has no useful
# import.meta.url to resolve a relative import against, which made the
# first version of this check fail for a reason that had nothing to do
# with the bundle.
node - "$BASE" "$ROOT/web/disk.mjs" <<'JS'
const base = process.argv[2];
const { chooseDisk, diskManifest } = await import(
    "file://" + encodeURI(process.argv[3]));

const m = await diskManifest(base + "images/");
const disk = chooseDisk(m, false);        /* false: the localhost default */

console.log(`  the page would ask for: ${disk.form} -- ${disk.describe}`);

if (!m) {
    console.log("FAIL: this bundle publishes no disk manifest, so the page "
                + "would ask for a single image file");
    process.exit(1);
}
if (!disk.use_parts || !disk.url.endsWith(".zst")) {
    console.log(`FAIL: the page would ask for ${disk.url}, which this `
                + "bundle does not publish");
    process.exit(1);
}

const r = await fetch(base + disk.url.replace(/\.img\.zst$/, "-0-")
                      + disk.fixed_chunk_size + ".img.zst");
if (!r.ok) {
    console.log(`FAIL: the first piece is not served (HTTP ${r.status})`);
    process.exit(1);
}
console.log(`  and the first piece answers ${r.status} `
            + `(${(await r.arrayBuffer()).byteLength} bytes)`);
JS
[ $? -eq 0 ] || fail "the page would ask this bundle for a disk it does not have"

# Every file the hub will serve has to be accounted for in CREDITS.txt.
# A bundle that serves software nobody can name the licence of is the one
# thing here that could cost somebody their site.
python3 - "$BUNDLE" <<'PY'
import os, re, sys

bundle = sys.argv[1]
credits_path = os.path.join(bundle, "CREDITS.txt")

if not os.path.exists(credits_path):
    print("FAIL: the bundle has no CREDITS.txt")
    sys.exit(1)

credits = open(credits_path, encoding="utf-8", errors="replace").read()
share = os.path.join(bundle, "attic")
missing = []

for root, _, names in os.walk(share):
    for n in names:
        if not n.lower().endswith(".zip"):
            continue
        if n not in credits:
            missing.append(n)

if missing:
    print("FAIL: served but not credited: " + ", ".join(sorted(missing)))
    sys.exit(1)

# And the claims that matter are the ones a complaint would test.
for want in ("free software", "Freedoom", "not id Software", "MIT"):
    if want not in credits:
        print(f"FAIL: CREDITS.txt no longer says anything about {want!r}")
        sys.exit(1)

n = len(re.findall(r"licence: ", credits))
print(f"  credits:                 {n} packages, every served file named")

# Somebody who wants a file taken down has to be able to find out where to
# write without cloning the repository. The address lives in one place in
# the build (mkcredits.sh); the page has to carry the same one.
page = open(os.path.join(bundle, "index.html"), encoding="utf-8",
            errors="replace").read()
addrs = set(re.findall(r"mailto:([^\"'<>\s]+@[^\"'<>\s]+)", page))
if not addrs:
    print("FAIL: the page gives no address to report a file to")
    sys.exit(1)
cred_addrs = set(re.findall(r"[\w.+-]+@[\w.-]+\.\w+", credits))
if not (addrs & cred_addrs):
    print(f"FAIL: page says {sorted(addrs)}, CREDITS.txt says "
          f"{sorted(cred_addrs) or 'nothing'}")
    sys.exit(1)
print(f"  report address:          {sorted(addrs & cred_addrs)[0]}, "
      "page and CREDITS.txt agree")
PY
[ $? -eq 0 ] || fail "the published share is not fully accounted for"

# Do the pieces reconstruct the disk? This is the claim the whole layout
# rests on: v86 reads piece N as the bytes at N * chunk, so a splitter
# that dropped, padded or reordered anything would produce a filesystem
# that mounts and then goes wrong somewhere nobody is looking.
if [ -f "$BUNDLE/images/dosypp.img.json" ]; then
    node - "$BUNDLE/images" "$ROOT/web/images/dosypp.img" <<'JS'
import { readFileSync, readdirSync } from "node:fs";
import { createHash } from "node:crypto";
import { zstdDecompressSync } from "node:zlib";
import { join } from "node:path";

const [dir, original] = process.argv.slice(2);
const man = JSON.parse(readFileSync(join(dir, "dosypp.img.json"), "utf8"));
const stem = man.image.replace(/\.img\.zst$/, "");

const h = createHash("sha256");
let total = 0;

for (let i = 0; i < man.parts; i++) {
    const start = i * man.chunk;
    const name = `${stem}-${start}-${start + man.chunk}.img.zst`;
    const raw = zstdDecompressSync(readFileSync(join(dir, name)));

    if (raw.length !== man.chunk && i !== man.parts - 1) {
        console.log(`FAIL: ${name} decompresses to ${raw.length} bytes, not `
                    + `${man.chunk} -- v86 reads a whole chunk at a time, so `
                    + "a short piece shifts the whole disk");
        process.exit(1);
    }
    h.update(raw);
    total += raw.length;
}

if (total !== man.size) {
    console.log(`FAIL: the pieces hold ${total} bytes, the manifest says `
                + `${man.size}`);
    process.exit(1);
}

const want = createHash("sha256").update(readFileSync(original)).digest("hex");
const got = h.digest("hex");

if (got !== want) {
    console.log(`FAIL: reassembled ${total} bytes hash ${got.slice(0, 16)}, `
                + `the image itself ${want.slice(0, 16)}`);
    process.exit(1);
}

console.log(`  pieces:                  ${man.parts} reassemble to the `
            + `${(man.size / 1048576).toFixed(0)}MB image exactly `
            + `(${got.slice(0, 12)}...)`);
JS
    [ $? -eq 0 ] || fail "the published pieces are not the disk image"

    # Range is no longer part of this: whole small files work on any host.
    # Reported anyway, because the SoundFont and the boot floppy are still
    # single files and a host that refuses Range affects nothing else here.
    CODE=$(curl -s -o /dev/null -w '%{http_code}' -r 0-15 \
           "${BASE}images/boot.img")
    echo "  Range (not needed now):  HTTP $CODE on boot.img"
else
    RANGE=$(curl -s -r 1048576-1048591 "${BASE}images/dosypp.img" | xxd -p)
    WANT=$(dd if="$BUNDLE/images/dosypp.img" bs=1 skip=1048576 count=16 \
              2>/dev/null | xxd -p)
    if [ -z "$RANGE" ] || [ "$RANGE" != "$WANT" ]; then
        fail "a Range request for 16 bytes at 1MB returned
       ${RANGE:0:64}...
       and the file itself has
       ${WANT:0:64}...
       (a server that ignores Range sends the whole file)"
    else
        echo "  Range request:           ok ($RANGE)"
    fi
fi

# ------------------------------------------- 2. no cross-origin isolation

python3 - "$BUNDLE/vendor/v86.wasm" <<'PY'
import sys

data = open(sys.argv[1], "rb").read()
assert data[:4] == b"\0asm", "not a wasm module"

i = 8
def uleb(i):
    r = s = 0
    while True:
        b = data[i]; i += 1
        r |= (b & 0x7F) << s; s += 7
        if not b & 0x80:
            return r, i

while i < len(data):
    sid = data[i]; i += 1
    size, i = uleb(i)
    if sid == 5:                      # the memory section
        _, j = uleb(i)
        flags = data[j]
        if flags & 0x02:
            print("  wasm memory:             SHARED -- needs COOP/COEP, "
                  "which GitHub Pages cannot send")
            sys.exit(1)
        print("  wasm memory:             unshared, so no COOP/COEP needed")
        sys.exit(0)
    i += size

print("  wasm memory:             no memory section found")
sys.exit(1)
PY
[ $? -eq 0 ] || fail "the engine in the bundle needs cross-origin isolation"

if grep -q "SharedArrayBuffer" "$BUNDLE/vendor/libv86.mjs"; then
    fail "libv86.mjs mentions SharedArrayBuffer -- check it still works "\
"without COOP/COEP before publishing"
else
    echo "  libv86.mjs:              no SharedArrayBuffer"
fi

# ------------------------------------------------ 3. the demo in it runs

echo
echo "--- relay/demo.sh, fetched from the served bundle ---"

( cd "$RUN" && BASE="$BASE" DIR="$RUN/demo" HUB_PORT=$HUB_PORT \
  ATTIC_PORT=$ATTIC_PORT RELAY_PORT=$RELAY_PORT \
  sh -c "curl -fsSL '$BASE/relay/demo.sh' | sh" ) > "$RUN/demo.log" 2>&1 &
DEMO_PID=$!

for _ in $(seq 40); do
    grep -q "wisprelay: listening\|serving" "$RUN/demo.log" 2>/dev/null && break
    sleep 0.5
done
sed -n '1,14p' "$RUN/demo.log" | sed 's/^/  /'

if ! grep -qa "attic" "$RUN/demo/hub.log" 2>/dev/null; then
    fail "demo.sh did not bring up a hub with a share"
    echo "--- demo.log ---"; cat "$RUN/demo.log"
    echo; echo "PAGES BUNDLE FAILED"; exit 1
fi

# A boot floppy that runs the download batch instead of the interactive
# startup. The hard disk is the bundle's own, untouched and streamed.
cp "$BUNDLE/images/boot.img" "$RUN/boottest.img"
cat > "$RUN/FDAUTO.BAT" <<'EOF'
@echo off
set DOSDIR=A:\FREEDOS
path A:\BIN;C:\
c:
call C:\AUTODL.BAT
EOF
mcopy -i "$RUN/boottest.img" -o "$RUN/FDAUTO.BAT" ::FDAUTO.BAT

echo
echo "--- booting the bundle's own disk, as published ---"
if [ -f "$BUNDLE/images/dosypp.img.json" ]; then
    DISK="$BUNDLE/images/dosypp.img.json"      # the compressed pieces
else
    DISK="$BUNDLE/images/dosypp.img"
fi
V86_ASYNC_HDA=1 node "$ROOT/test/v86/boot.mjs" \
    "wisp://localhost:$RELAY_PORT/" \
    "$RUN/boottest.img" \
    "$DISK" \
    "${1:-300}" "AUTODL-DONE" "AUTODL-DONE" | tee "$RUN/guest.log" | tail -24
rc=${PIPESTATUS[1]}

echo
echo "-------------------------------"
grep -qa "download done: BLKDROP.ZIP" "$RUN/guest.log" ||
    fail "the guest did not download the game from the bundled share"
grep -qa "BLKDROP  EXE" "$RUN/guest.log" ||
    fail "the guest did not unpack the game"
[ "$rc" -eq 0 ] || fail "the guest did not finish its run (exit $rc)"

if [ "$FAILED" -eq 0 ]; then
    echo
    echo "PAGES BUNDLE OK: a plain static host served it, the compressed"
    echo "pieces reassemble to the disk exactly, the engine needs no"
    echo "cross-origin isolation, and the demo downloaded a game into the"
    echo "bundle's own disk -- booted in the form that gets published."
    echo
    echo "Not checked here: that a browser renders it. There is no browser"
    echo "in this rig -- the same engine runs under Node instead."
    exit 0
fi

echo
echo "PAGES BUNDLE FAILED"
echo "  logs: $RUN"
exit 1
