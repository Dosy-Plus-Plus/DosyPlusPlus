#!/bin/bash
# The rescan timer: does a file added after startup get picked up?
#
# This takes a little over a minute of real time, because the shortest
# setting is one minute and faking the clock would mean testing the fake.
# It is the only way to tell "the timer works" from "the timer is wired to
# nothing", which is a mistake that would never show up in a short run.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLIENT="$ROOT/build/host/dosypp"
RUN="$(mktemp -d)"
HUB_PORT=${RESCAN_HUB_PORT:-24931}

cleanup() {
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    pkill -f "dosypp -c $RUN/" 2>/dev/null
    [ -n "${RESCAN_KEEP:-}" ] && { echo "run dir kept: $RUN"; return; }
    rm -rf "$RUN"
}
trap cleanup EXIT

[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }

mkdir -p "$RUN/share/warez" "$RUN/node"

head -c 40000 /dev/urandom > "$RUN/share/warez/FIRST.ZIP"

cat > "$RUN/node/dosypp.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = Watcher
sound = no
rescan = 1
share = $RUN/share
EOF

"$ROOT/tools/stubhub.py" --port "$HUB_PORT" --no-fake-share \
    > "$RUN/hub.log" 2>&1 &
HUB_PID=$!
for _ in $(seq 20); do
    nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null && break
    sleep 0.25
done

echo "starting a client sharing one file, rescan = 1 minute..."
( cd "$RUN/node" && exec "$CLIENT" -c "$RUN/node/dosypp.ini" \
    --screenshot "$RUN/shot.txt" --null-display --door 5 \
    --run-secs 100 127.0.0.1 ) > "$RUN/run.log" 2>&1 < /dev/null &
NODE_PID=$!

# Wait for the first pass to finish, so the file added next is genuinely
# new rather than something the initial scan would have found anyway.
for _ in $(seq 60); do
    grep -qE "BINF .* SS[1-9]" "$RUN/hub.log" 2>/dev/null && break
    sleep 0.5
done
FIRST=$(grep -oE "SS[0-9]+ SF[0-9]+" "$RUN/hub.log" | tail -1)
echo "after the first scan, the hub was told: $FIRST"

echo "adding a second file..."
head -c 70000 /dev/urandom > "$RUN/share/warez/SECOND.ZIP"

# One minute for the timer, plus slack for the scan and the hashing.
echo "waiting for the rescan (about 75 seconds)..."
for _ in $(seq 90); do
    grep -qE "BINF .* SF2" "$RUN/hub.log" 2>/dev/null && break
    sleep 1
done

wait "$NODE_PID" 2>/dev/null

echo
echo "--- the share door ---"
sed -n '2,14p' "$RUN/shot.txt" 2>/dev/null || echo "(no screenshot)"
echo "----------------------"

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

LAST=$(grep -oE "SS[0-9]+ SF[0-9]+" "$RUN/hub.log" | tail -1)
echo "the hub was last told: $LAST"

grep -qE "BINF .* SF2" "$RUN/hub.log" ||
    fail "the second file was never picked up -- the rescan did not happen"

grep -q "2 files" "$RUN/shot.txt" ||
    fail "the share door does not show two files"

# It must have rescanned, not merely restarted: the status line and the
# log both say so.
grep -q "rescanning" "$RUN/run.log" ||
    echo "note: the rescan status line is not in stdout (it is a UI status)"

echo
if [ "$ok" = 1 ]; then
    echo "RESCAN OK: a file added after startup was found and published."
    exit 0
fi
echo "--- client output ---" >&2
tail -10 "$RUN/run.log" >&2
echo "--- what failed ---$FAILURES" >&2
echo "RESCAN FAILED" >&2
exit 1
