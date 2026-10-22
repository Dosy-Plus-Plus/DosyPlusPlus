#!/bin/bash
# The setup door: does editing settings on screen actually change the file?
#
# The screen showing "yes" proves nothing on its own -- what matters is that
# DOSYPP.INI says so afterwards, and that the client reads it back the same
# way on the next run. So this drives the door with keystrokes, saves, and
# then reads the file and restarts the client to see what it loaded.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$ROOT/test/host/hublib.sh"   # await_line
CLIENT="$ROOT/build/host/dosypp"
RUN="$(mktemp -d)"
HUB_PORT=${SETUP_HUB_PORT:-24991}

ok=1
fail() { echo "FAIL: $*" >&2; ok=0; }

cleanup() {
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    pkill -f "dosypp -c $RUN/" 2>/dev/null
    if [ -n "${SETUP_KEEP:-}" ] || [ "$ok" != 1 ]; then
        echo "run dir kept: $RUN"
        return
    fi
    rm -rf "$RUN"
}
trap cleanup EXIT

[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }

cat > "$RUN/node.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = Before
sound = yes
active = no
theme = renegade
EOF

"$ROOT/tools/stubhub.py" --port "$HUB_PORT" > "$RUN/hub.log" 2>&1 &
HUB_PID=$!
for _ in $(seq 20); do
    nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null && break
    sleep 0.25
done

DOWN=$'\033[B'

# Field order in the door: hub, port, handle, password, description, slots,
# active, listen port, hashing, rescan, sound, theme.
#
#   DOWN x2, ENTER, "After", ENTER   -> change the handle
#   DOWN x4, ENTER                   -> toggle ACTIVE MODE
#   DOWN x5, ENTER                   -> cycle the THEME
#   S                                -> write the file
echo "driving the setup door..."
(
    # Anchored on the connection rather than on process start, then given
    # the same settle the door always needed: "connected" is logged within
    # a second, and the first keys used to arrive at five. Pressing at one
    # second saved nothing and edited nothing -- the door had not drawn.
    await_line "$RUN/run.log" "connected" || true
    sleep 4
    printf '%s%s' "$DOWN" "$DOWN"; sleep 1
    printf '\r';                    sleep 1
    printf 'After\r';               sleep 1
    printf '%s%s%s%s' "$DOWN" "$DOWN" "$DOWN" "$DOWN"; sleep 1
    printf '\r';                    sleep 1
    printf '%s%s%s%s%s' "$DOWN" "$DOWN" "$DOWN" "$DOWN" "$DOWN"; sleep 1
    printf '\r';                    sleep 1
    printf 'S';                     sleep 3
) | ( cd "$RUN" && exec "$CLIENT" -c "$RUN/node.ini" \
        --screenshot "$RUN/shot.txt" --null-display --door 9 \
        --run-secs 20 127.0.0.1 ) > "$RUN/run.log" 2>&1

echo
echo "--- the setup door, after editing ---"
sed -n '2,20p' "$RUN/shot.txt" 2>/dev/null || echo "(no screenshot)"
echo "-------------------------------------"

echo "--- what was written ---"
grep -vE "^;|^$" "$RUN/node.ini" | head -20
echo "------------------------"

# The file, which is the only thing that outlives the run.
grep -qE "^nick *= *After" "$RUN/node.ini" ||
    fail "the handle was not saved (still $(grep -E '^nick' "$RUN/node.ini" | head -1))"
grep -qE "^active *= *yes" "$RUN/node.ini" ||
    fail "active mode was not saved"
grep -qE "^theme *= *pcboard" "$RUN/node.ini" ||
    fail "the theme was not saved (got $(grep -E '^theme' "$RUN/node.ini" | head -1))"

# Nothing else should have changed. A settings screen that quietly rewrites
# unrelated fields is worse than one that does not work.
grep -qE "^port *= *$HUB_PORT" "$RUN/node.ini" ||
    fail "the port changed when nothing touched it"
grep -qE "^sound *= *yes" "$RUN/node.ini" ||
    fail "sound changed when nothing touched it"

# And the client reads its own file back the same way: a save that produces
# a file the loader reads differently is the real failure mode here.
echo "--- reading it back ---"
( cd "$RUN" && exec "$CLIENT" -c "$RUN/node.ini" \
    --screenshot "$RUN/shot2.txt" --null-display --door 9 \
    --run-secs 4 127.0.0.1 ) > "$RUN/run2.log" 2>&1 < /dev/null

grep -q "After" "$RUN/shot2.txt" 2>/dev/null ||
    fail "the saved handle did not come back"
grep -qE "ACTIVE MODE +yes" "$RUN/shot2.txt" 2>/dev/null ||
    fail "the saved active mode did not come back"
grep -qE "THEME +pcboard" "$RUN/shot2.txt" 2>/dev/null ||
    fail "the saved theme did not come back"
grep -q "listening on port" "$RUN/run2.log" ||
    fail "active mode was saved but the client did not listen"

echo
if [ "$ok" = 1 ]; then
    echo "SETUP OK: edits reach the file and come back on the next run."
    exit 0
fi
echo "SETUP FAILED" >&2
exit 1
