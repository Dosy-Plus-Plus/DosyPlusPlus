#!/bin/bash
# The /-commands: do the ones that change something actually change it?
#
# /help printing a list proves itself. These do not:
#
#   /desc    has to reach the hub as an INF update, not just the config
#   /slots   likewise, and the number has to come back in the INF
#   /connect has to move the session to another hub
#   /save    has to write the file
#   /rows    has to be in the file and NOT take effect in this run
#
# So there are two stub hubs here, and the check is what each of them saw.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLIENT="$ROOT/build/host/dosypp"
RUN="$(mktemp -d)"
# Ports the kernel says are free, rather than two numbers that looked
# unused: the first version of this test hard-coded a port that something
# else was already listening on, and `nc -z` happily confirmed that
# *something* was there. It then checked a log file that the hub it meant to
# start had never written to.
free_port() {
    python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}
PORT_A=${CMD_PORT_A:-$(free_port)}
PORT_B=${CMD_PORT_B:-$(free_port)}

ok=1
# Collected as well as printed: the sweep shows only the tail of a log,
# and these lines are often a screen or two above it. The list is
# reprinted immediately before the verdict, so a red run in CI says which
# check failed instead of only that something did.
FAILURES=""
fail() {
    echo "FAIL: $*" >&2
    FAILURES="$FAILURES
  - $*"
    ok=0
}

cleanup() {
    [ -n "${HUB_A:-}" ] && kill "$HUB_A" 2>/dev/null
    [ -n "${HUB_B:-}" ] && kill "$HUB_B" 2>/dev/null
    if [ -n "${CMD_KEEP:-}" ] || [ "$ok" != 1 ]; then
        echo "run dir kept: $RUN"
        return
    fi
    rm -rf "$RUN"
}
trap cleanup EXIT

[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }

cat > "$RUN/node.ini" <<EOF
hub = 127.0.0.1
port = $PORT_A
nick = Warlord
active = no
sound = no
music = no
rows = 25
slots = 1
EOF

"$ROOT/tools/stubhub.py" --port "$PORT_A" > "$RUN/hubA.log" 2>&1 &
HUB_A=$!
"$ROOT/tools/stubhub.py" --port "$PORT_B" > "$RUN/hubB.log" 2>&1 &
HUB_B=$!
# And the hub we started has to be the one answering: its own log line,
# plus the process still being alive. Either of those failing means the
# rest of this script would be testing somebody else's hub.
for _ in $(seq 40); do
    grep -q "stubhub: adc://" "$RUN/hubA.log" 2>/dev/null &&
    grep -q "stubhub: adc://" "$RUN/hubB.log" 2>/dev/null && break
    sleep 0.25
done
for which in A B; do
    eval "pid=\$HUB_$which; port=\$PORT_$which"
    kill -0 "$pid" 2>/dev/null || {
        echo "stub hub $which died on port $port:" >&2
        cat "$RUN/hub$which.log" >&2
        exit 1; }
    grep -q "stubhub: adc://" "$RUN/hub$which.log" || {
        echo "stub hub $which never bound port $port:" >&2
        cat "$RUN/hub$which.log" >&2
        exit 1; }
done
echo "hub A on $PORT_A, hub B on $PORT_B"

echo "driving the commands..."
(
    sleep 4
    printf '/desc Dialling from 1994\r';      sleep 2
    printf '/slots 4\r';                      sleep 2
    printf '/rows 50\r';                      sleep 1
    printf '/save\r';                         sleep 2
    printf '/connect 127.0.0.1:%s\r' "$PORT_B"; sleep 6
    printf '/stats\r';                        sleep 2
) | ( cd "$RUN" && exec "$CLIENT" -c "$RUN/node.ini" \
        --null-display --screenshot "$RUN/shot.txt" --door 1 \
        --run-secs 24 127.0.0.1 ) > "$RUN/run.log" 2>&1

echo "--- the message pane ---"
sed -n '2,20p' "$RUN/shot.txt" 2>/dev/null || echo "(no screenshot)"
echo "------------------------"

# ---- what hub A was told
grep -qE "BINF .* DEDialling\\\\sfrom\\\\s1994" "$RUN/hubA.log" ||
    fail "/desc did not reach the hub as an INF update ($(grep -oE 'BINF [^ ]+ DE[^ ]*' "$RUN/hubA.log" | tail -1))"
grep -qE "BINF .* SL4" "$RUN/hubA.log" ||
    fail "/slots 4 did not reach the hub ($(grep -oE 'BINF [^ ]+ SL[0-9]+' "$RUN/hubA.log" | tail -1))"

# ---- the file
grep -qE "^rows *= *50" "$RUN/node.ini" ||
    fail "/rows 50 then /save did not write the file"
grep -qE "^slots *= *4" "$RUN/node.ini" ||
    fail "/slots did not reach the file"
grep -q "Dialling from 1994" "$RUN/node.ini" ||
    fail "/desc did not reach the file"

# The screenshot is the proof /rows did NOT take effect this run: the cell
# buffer is still 25 rows, which is what the row said would happen.
LINES=$(grep -c "" "$RUN/shot.txt" 2>/dev/null || echo 0)
[ "$LINES" = 25 ] ||
    fail "/rows changed the screen in this run ($LINES rows), which it says it does not"

# ---- the move
grep -q "logged in as 'Warlord'" "$RUN/hubB.log" ||
    fail "/connect did not move the session to the other hub"

# ---- a dial that does not answer must not stop the client's clock
#
# /connect used to be a blocking call with a 20-second timeout: the main
# loop stopped, the sound card's ring drained, and the screen froze. The
# dial is polled now, and the measurement is the mixer's own counter --
# frames fed to the device while the dial is in progress. 192.0.2.1 is
# RFC 5737 test-net space, which black-holes rather than refusing, so the
# connect really does stay pending.
echo
echo "dialling a black hole with the music on..."
cp "$ROOT/art/DOSYPP.MOD" "$RUN/DOSYPP.MOD" 2>/dev/null

cat > "$RUN/music.ini" <<EOF
hub = 127.0.0.1
port = $PORT_A
nick = Warlord
active = no
sound = no
music = yes
music.file = DOSYPP.MOD
music.rate = 11025
music.wav = dial.wav
EOF

(
    sleep 5
    printf '/connect 192.0.2.1:1511\r'
    sleep 20
) | ( cd "$RUN" && exec "$CLIENT" -c "$RUN/music.ini" --null-display \
        --door 1 --music-report --run-secs 30 \
        --screenshot "$RUN/dial.txt" 127.0.0.1 ) > "$RUN/dial.log" 2>&1

echo "--- what the client reported while dialling ---"
grep -a "MUSIC:\|connecting to\|did not answer" "$RUN/dial.log" | tail -8

grep -qa "connecting to" "$RUN/dial.log" ||
    fail "/connect to a black hole did not start a dial"

# The frames fed after the dial began, against before. A blocking connect
# shows as a flat line here; a polled one keeps mixing.
DIAL_FED=$(grep -a "MUSIC:" "$RUN/dial.log" |
           sed -n 's/.*fed=\([0-9]*\).*/\1/p' |
           python3 -c '
import sys
n = [int(x) for x in sys.stdin.read().split()]
# The dial starts five seconds in; compare the last reading with the one
# from about then, which is the fifth or so.
if len(n) < 8:
    print("short")
else:
    print(n[-1] - n[5])
')
echo "frames mixed after the dial began: $DIAL_FED"

if [ "$DIAL_FED" = "short" ]; then
    fail "not enough reports to tell whether the dial blocked the loop"
elif [ "${DIAL_FED:-0}" -lt 22050 ]; then
    fail "only $DIAL_FED frames were mixed while dialling -- two seconds' \
worth is 22050, so the loop was blocked"
fi

if [ "$ok" = 1 ]; then
    echo "COMMANDS OK: /desc and /slots reach the hub, /save writes the"
    echo "file, /rows waits for the next run, /connect moves hubs, and a"
    echo "dial that does not answer keeps the music playing."
    exit 0
fi
echo "--- what failed ---$FAILURES" >&2
echo "COMMANDS FAILED"
exit 1
