#!/bin/bash
# The browser host's session persistence, as far as it can be tested without
# a browser.
#
# The page keeps the whole machine state in IndexedDB so a reload does not
# cost you a new CID, a re-hashed share and your favourites. IndexedDB
# itself is browser-only; v86's save_state/restore_state with DOS running
# inside is the half that can go wrong, and that runs in Node.
set -uo pipefail

# mtools asks about a name clash, and asks on stdin: an mmd for a
# directory that exists, or an mcopy without -o, waits for an answer that
# no rig is going to give. Non-interactive runs get EOF and carry on;
# a person running this in a terminal waits for ever, with the question
# usually hidden by a 2>/dev/null. One line removes the whole class.
exec < /dev/null


ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN="$ROOT/build/v86state"
. "$ROOT/test/host/hublib.sh"

command -v node >/dev/null 2>&1 || { echo "need node (brew install node)" >&2; exit 1; }
for f in web/vendor/libv86.mjs web/images/boot.img web/images/dosypp.img; do
    [ -f "$ROOT/$f" ] || { echo "missing $f -- run: make images" >&2; exit 1; }
done

RELAY_PORT=$(free_port)
HUB_PORT=$(free_port)

rm -rf "$RUN"; mkdir -p "$RUN"

cleanup() {
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    [ -n "${RELAY_PID:-}" ] && kill "$RELAY_PID" 2>/dev/null
}
trap cleanup EXIT

command -v mcopy >/dev/null 2>&1 || { echo "need mtools (brew install mtools)" >&2; exit 1; }
export MTOOLS_SKIP_CHECK=1

# The shipped boot floppy runs GO.BAT, which talks to the screen. The
# harness reads COM1, so it needs the same non-interactive floppy the login
# test builds -- without this the test sees nothing and blames the state
# machinery for it.
cp "$ROOT/web/images/boot.img" "$RUN/bootstate.img"
cat > "$RUN/FDAUTO.BAT" <<'EOF'
@echo off
set DOSDIR=A:\FREEDOS
path A:\BIN;C:\
c:
call C:\AUTOTEST.BAT
EOF
mcopy -i "$RUN/bootstate.img" -o "$RUN/FDAUTO.BAT" ::FDAUTO.BAT

start_stub_hub "$HUB_PORT" "$RUN/hub.log"

"$ROOT/tools/wisprelay.py" --port "$RELAY_PORT" \
    --map "10.77.0.1=127.0.0.1:$HUB_PORT" > "$RUN/relay.log" 2>&1 &
RELAY_PID=$!
for _ in $(seq 40); do
    grep -q "wisprelay: http" "$RUN/relay.log" 2>/dev/null && break
    sleep 0.25
done

node "$ROOT/test/v86/state.mjs" \
    "wisp://localhost:$RELAY_PORT/" \
    "$RUN/bootstate.img" \
    "$ROOT/web/images/dosypp.img" \
    "${1:-300}"
rc=$?

if [ $rc -eq 0 ]; then
    echo
    echo "V86 STATE OK: a session was saved, restored into a fresh emulator,"
    echo "and carried on with the same identity."
fi
exit $rc
