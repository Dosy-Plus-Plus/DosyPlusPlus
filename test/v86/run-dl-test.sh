#!/bin/bash
# The browser demo's headline claim, with a rig behind it.
#
# A DOS binary in a browser engine, with no TCP of its own, searches a hub
# for a real game, downloads it from a peer it had to dial through a
# WebSocket, verifies it against the hash the hub advertised, and unpacks
# the executable.
#
# Nothing here is simulated except the hardware: the hub is tools/stubhub.py
# with --serve, so the file really is served over the client-to-client half
# of ADC, and the bytes are a FreeDOS package fetched by tools/fetchatmos.sh.
set -uo pipefail

# mtools asks about a name clash, and asks on stdin: an mmd for a
# directory that exists, or an mcopy without -o, waits for an answer that
# no rig is going to give. Non-interactive runs get EOF and carry on;
# a person running this in a terminal waits for ever, with the question
# usually hidden by a 2>/dev/null. One line removes the whole class.
exec < /dev/null


ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN="$ROOT/build/v86dl"
RELAY_PORT=18089
HUB_PORT=11514
ATTIC_PORT=11515
WAREZ="${WAREZ_DIR:-$ROOT/build/warez}"

command -v node >/dev/null 2>&1 || { echo "need node" >&2; exit 1; }
command -v mcopy >/dev/null 2>&1 || { echo "need mtools" >&2; exit 1; }

[ -f "$WAREZ/SHARE.TXT" ] || {
    echo "no share to download from: $WAREZ/SHARE.TXT" >&2
    echo "  run: make warez      (needs make atmos first)" >&2
    exit 1; }

grep -q "BLKDROP.ZIP" "$WAREZ/SHARE.TXT" || {
    echo "the share has no BLKDROP.ZIP -- this rig downloads that one" >&2
    exit 1; }

WANT_TTH=$(awk '/BLKDROP.ZIP/ {print $1}' "$WAREZ/SHARE.TXT" | head -1)
WANT_SIZE=$(awk '/BLKDROP.ZIP/ {print $2}' "$WAREZ/SHARE.TXT" | head -1)

export MTOOLS_SKIP_CHECK=1
rm -rf "$RUN"; mkdir -p "$RUN"

# A boot floppy that runs the download batch instead of GO.BAT.
cp "$ROOT/web/images/boot.img" "$RUN/boottest.img"
cat > "$RUN/FDAUTO.BAT" <<'EOF'
@echo off
set DOSDIR=A:\FREEDOS
path A:\BIN;C:\
c:
call C:\AUTODL.BAT
EOF
mcopy -i "$RUN/boottest.img" -o "$RUN/FDAUTO.BAT" ::FDAUTO.BAT

mdir -i "$ROOT/web/images/dosypp.img" ::AUTODL.BAT >/dev/null 2>&1 || {
    echo "dosypp.img has no AUTODL.BAT -- rebuild: ./tools/mkv86img.sh" >&2
    exit 1; }

# The guest needs UNZIP.EXE to unpack what it downloads, and that comes
# from the fetched packages. Without it the download still proves itself;
# the unpacking half would silently not happen, which is the kind of quiet
# pass this project does not want.
mdir -i "$ROOT/web/images/dosypp.img" ::UNZIP.EXE >/dev/null 2>&1 || {
    echo "dosypp.img has no UNZIP.EXE -- run: make atmos && make images" >&2
    exit 1; }

cleanup() {
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    [ -n "${RELAY_PID:-}" ] && kill "$RELAY_PID" 2>/dev/null
}
trap cleanup EXIT

# The hub, with one node that really has the files.
"$ROOT/tools/stubhub.py" --port "$HUB_PORT" --no-fake-share \
    --serve "$WAREZ" --serve-ip 10.77.0.2 --serve-port "$ATTIC_PORT" \
    > "$RUN/hub.log" 2>&1 &
HUB_PID=$!

# Two mappings: the hub and the serving node. The guest dials both by their
# sentinel addresses and never learns a real one.
"$ROOT/tools/wisprelay.py" --port "$RELAY_PORT" \
    --map "10.77.0.1=127.0.0.1:$HUB_PORT" \
    --map "10.77.0.2=127.0.0.1:$ATTIC_PORT" > "$RUN/relay.log" 2>&1 &
RELAY_PID=$!
sleep 1.5

grep -q "attic" "$RUN/hub.log" || {
    echo "the hub did not start its serving node:" >&2
    cat "$RUN/hub.log" >&2
    exit 1; }

node "$ROOT/test/v86/boot.mjs" \
    "wisp://localhost:$RELAY_PORT/" \
    "$RUN/boottest.img" \
    "$ROOT/web/images/dosypp.img" \
    "${1:-300}" \
    "AUTODL-DONE" \
    "AUTODL-DONE" | tee "$RUN/guest.log"
rc=$?

echo
echo "--- the serving node ---"
grep -E "attic|sending|sent " "$RUN/hub.log" | tail -8

fail() { echo "FAIL: $*"; FAILED=1; }
FAILED=0

echo
echo "-------------------------------"

grep -qa "download done: BLKDROP.ZIP" "$RUN/guest.log" ||
    fail "the client never reported BLKDROP.ZIP as downloaded"

# The client prints that line only after the file matches the TTH the hub
# advertised, so this is a hash check, not a byte count. Said explicitly
# because "download done" reads like it could be either.
grep -qa "CSND file TTH/$WANT_TTH" "$RUN/guest.log" ||
    fail "the peer never sent the file we asked for by hash"

# DOS's own dir output, so the file is on the guest's disk rather than
# merely reported.
grep -qa "BLKDROP  ZIP" "$RUN/guest.log" ||
    fail "DIR does not show BLKDROP.ZIP on the guest's disk"

SHOWN=$(grep -oa "BLKDROP  ZIP *[0-9,]*" "$RUN/guest.log" | head -1 |
        awk '{print $3}' | tr -d ',')
if [ -n "$SHOWN" ] && [ "$SHOWN" != "$WANT_SIZE" ]; then
    fail "DIR shows $SHOWN bytes, the hub advertised $WANT_SIZE"
fi

# And that it is a real archive: UNZIP unpacked the game's executable.
grep -qa "BLKDROP  EXE" "$RUN/guest.log" ||
    fail "UNZIP did not produce BLKDROP.EXE -- nothing to play"

[ $rc -eq 0 ] || fail "the guest did not finish its run (harness exit $rc)"

echo "wanted: BLKDROP.ZIP, $WANT_SIZE bytes, TTH $WANT_TTH"
echo "guest:  $(grep -oa 'BLKDROP  ZIP *[0-9,]*' "$RUN/guest.log" | head -1)"
echo "unzip:  $(grep -oa 'BLKDROP  EXE *[0-9,]*' "$RUN/guest.log" | head -1)"

if [ "$FAILED" -eq 0 ]; then
    echo
    echo "V86 DOWNLOAD OK: a DOS client in a browser engine searched a hub,"
    echo "dialled a peer through a WebSocket, downloaded a real game,"
    echo "verified it against the hub's hash and unpacked its executable."
    exit 0
fi

echo
echo "V86 DOWNLOAD FAILED"
echo "  guest log: $RUN/guest.log"
echo "  hub log:   $RUN/hub.log"
exit 1
