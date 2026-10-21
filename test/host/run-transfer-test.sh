#!/bin/bash
# Transfer a file between two DOSy++ clients and verify it by hash.
#
# Self-contained because doing this by hand repeatedly produced more harness
# mistakes than real findings: stale screenshots, wrong ports, leftover
# clients from a demo. Everything here is per-run and torn down.
#
# One client shares a file and runs ACTIVE (listening); the other is PASSIVE
# and downloads it. That is the only combination that can work for a DOS
# client: two passive clients can never connect.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLIENT="$ROOT/build/host/dosypp"
RUN="$(mktemp -d)"
HUB_PORT=${TRANSFER_HUB_PORT:-24601}
PEER_PORT=${TRANSFER_PEER_PORT:-24602}
SIZE=${TRANSFER_SIZE:-180000}
# Run lengths scale with the size: a 100MB transfer is a different test
# from a 180KB one and finding size-dependent bugs needs it to be runnable.
RUN_SECS=${TRANSFER_RUN_SECS:-40}

cleanup() {
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    [ -n "${UP_PID:-}" ] && kill "$UP_PID" 2>/dev/null
    pkill -f "dosypp -c $RUN/" 2>/dev/null
    [ -n "${TRANSFER_KEEP:-}" ] && { echo "run dir kept: $RUN"; return; }
    rm -rf "$RUN"
}
trap cleanup EXIT

[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }
command -v rhash >/dev/null 2>&1 || { echo "need rhash (brew install rhash)" >&2; exit 1; }

mkdir -p "$RUN/up/warez" "$RUN/down"

python3 - "$RUN/up/warez/MAZERUN2.ZIP" "$SIZE" <<'PY'
import random, sys
random.seed(1994)
with open(sys.argv[1], "wb") as f:
    f.write(bytes(random.getrandbits(8) for _ in range(int(sys.argv[2]))))
PY

WANT=$(rhash --tth "$RUN/up/warez/MAZERUN2.ZIP" | cut -d' ' -f1 | tr 'a-z' 'A-Z')
echo "file to move: $SIZE bytes, TTH $WANT"

cat > "$RUN/up.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = Uploader
sound = no
active = yes
port.listen = $PEER_PORT
share = $RUN/up
EOF

cat > "$RUN/dn.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = Downloader
sound = no
active = no
EOF

# --no-fake-share: otherwise the hub answers searches for files that do not
# exist, and there is no telling which result got queued.
"$ROOT/tools/stubhub.py" --port "$HUB_PORT" --no-fake-share \
    > "$RUN/hub.log" 2>&1 &
HUB_PID=$!
for _ in $(seq 20); do
    nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null && break
    sleep 0.25
done

echo "starting the uploader (active, sharing)..."
( cd "$RUN/up" && exec "$CLIENT" -c "$RUN/up.ini" --screenshot "$RUN/up.txt" \
    --null-display --door 5 --peer-trace --run-secs $((RUN_SECS * 2)) \
    127.0.0.1 ) > "$RUN/up.log" 2>&1 &
UP_PID=$!

# Wait for it to finish hashing: until it has, it answers no searches.
for _ in $(seq 60); do
    grep -q "SS[1-9]" "$RUN/hub.log" 2>/dev/null && break
    sleep 0.5
done
grep -q "listening on port" "$RUN/up.log" || {
    echo "the uploader is not listening:" >&2
    cat "$RUN/up.log" >&2
    exit 1; }
echo "uploader ready: $(grep -oE 'SS[0-9]+ SF[0-9]+' "$RUN/hub.log" | tail -1)"

echo "starting the downloader (passive)..."
( cd "$RUN/down" && exec "$CLIENT" -c "$RUN/dn.ini" \
    --screenshot "$RUN/dn.txt" --null-display --door ${TRANSFER_DOOR:-4} --peer-trace \
    --search MAZERUN2 --queue --run-secs $RUN_SECS 127.0.0.1 ) \
    > "$RUN/dn.log" 2>&1

echo
echo "--- the downloader's door ---"
sed -n '2,12p' "$RUN/dn.txt" 2>/dev/null
echo "-------------------------"

echo "--- the INF the downloader was told about the uploader ---"
grep -E "BINF" "$RUN/hub.log" | head -4
echo "--- connection setup, as the hub saw it ---"
grep -E "RCM|CTM|routed" "$RUN/hub.log" | head -6 || echo "(none)"
echo

GOT_FILE=$(ls "$RUN/down" | grep -viE 'DOSYPP|\.ini' | head -1)
if [ -z "$GOT_FILE" ]; then
    echo "NO FILE WAS DOWNLOADED" >&2
    echo "--- downloader output ---" >&2
    tail -12 "$RUN/dn.log" >&2
    exit 1
fi

GOT=$(rhash --tth "$RUN/down/$GOT_FILE" | cut -d' ' -f1 | tr 'a-z' 'A-Z')
SZ=$(wc -c < "$RUN/down/$GOT_FILE" | tr -d ' ')

echo "downloaded as: $GOT_FILE ($SZ bytes)"
echo "  expected TTH: $WANT"
echo "  actual   TTH: $GOT"

if [ -f "$RUN/down/DOSYPP.NAM" ]; then
    echo "name map:"
    sed 's/^/  /' "$RUN/down/DOSYPP.NAM"
fi

# What the door said while it was moving, which is where size-dependent
# display bugs show: a percentage that overflowed, a rate of zero, a size
# shown as 0B. Checked rather than merely printed.
if grep -qE "COMPLETE" "$RUN/dn.txt" 2>/dev/null; then
    echo "the transfer door shows it complete"
else
    echo "WARNING: the door does not say COMPLETE:" >&2
    sed -n '2,8p' "$RUN/dn.txt" >&2
fi

if [ "$GOT" = "$WANT" ] && [ "$SZ" = "$SIZE" ]; then
    echo
    echo "TRANSFER OK: the file arrived intact and the hash matches."
    exit 0
fi

echo "TRANSFER FAILED: content does not match" >&2
exit 1
