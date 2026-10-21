#!/bin/bash
# Boot the v86 browser host headlessly and verify a live ADC handshake.
#
# Starts the stub hub and the Wisp relay, builds a test boot floppy whose
# startup runs AUTOTEST.BAT (output to COM1), then drives v86 from Node.
set -uo pipefail

# mtools asks about a name clash, and asks on stdin: an mmd for a
# directory that exists, or an mcopy without -o, waits for an answer that
# no rig is going to give. Non-interactive runs get EOF and carry on;
# a person running this in a terminal waits for ever, with the question
# usually hidden by a 2>/dev/null. One line removes the whole class.
exec < /dev/null


ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN="$ROOT/build/v86"
RELAY_PORT=18087
HUB_PORT=11512

command -v node >/dev/null 2>&1 || { echo "need node (brew install node)" >&2; exit 1; }
command -v mcopy >/dev/null 2>&1 || { echo "need mtools (brew install mtools)" >&2; exit 1; }

for f in web/vendor/libv86.mjs web/vendor/v86.wasm web/vendor/seabios.bin \
         web/vendor/vgabios.bin web/images/boot.img web/images/dosypp.img; do
    [ -f "$ROOT/$f" ] || { echo "missing $f -- run: make images" >&2; exit 1; }
done

export MTOOLS_SKIP_CHECK=1
rm -rf "$RUN"; mkdir -p "$RUN"

# A boot floppy that runs the non-interactive test instead of GO.BAT.
cp "$ROOT/web/images/boot.img" "$RUN/boottest.img"
cat > "$RUN/FDAUTO.BAT" <<'EOF'
@echo off
set DOSDIR=A:\FREEDOS
path A:\BIN;C:\
c:
call C:\AUTOTEST.BAT
EOF
mcopy -i "$RUN/boottest.img" -o "$RUN/FDAUTO.BAT" ::FDAUTO.BAT

mdir -i "$ROOT/web/images/dosypp.img" ::AUTOTEST.BAT >/dev/null 2>&1 || {
    echo "dosypp.img has no AUTOTEST.BAT -- rebuild with ./tools/mkv86img.sh" >&2
    exit 1; }

cleanup() {
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    [ -n "${RELAY_PID:-}" ] && kill "$RELAY_PID" 2>/dev/null
}
trap cleanup EXIT

"$ROOT/tools/stubhub.py" --port "$HUB_PORT" > "$RUN/hub.log" 2>&1 &
HUB_PID=$!
"$ROOT/tools/wisprelay.py" --port "$RELAY_PORT" \
    --map "10.77.0.1=127.0.0.1:$HUB_PORT" > "$RUN/relay.log" 2>&1 &
RELAY_PID=$!
sleep 1.5

node "$ROOT/test/v86/boot.mjs" \
    "wisp://localhost:$RELAY_PORT/" \
    "$RUN/boottest.img" \
    "$ROOT/web/images/dosypp.img" \
    "${1:-240}"
rc=$?

echo
echo "--- relay ---"; tail -12 "$RUN/relay.log"
echo "--- hub ---";   tail -12 "$RUN/hub.log"

if [ $rc -eq 0 ]; then
    echo
    echo "V86 OK: DOSy++ ran in v86 and reached the hub through the Wisp relay."
fi
exit $rc
