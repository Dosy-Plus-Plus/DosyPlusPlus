#!/bin/bash
# A share bigger than 4GB, counted and advertised and read back.
#
# What this does NOT test, despite appearances: the 32-bit print that was
# the original bug. `unsigned long` is 64 bits on the machine this runs on,
# so the broken code prints the right answer here -- verified by putting it
# back and watching this rig pass. The bug needed a 32-bit long, which
# means DOS: see dos-bigshare-check, where the same sabotage fails
# immediately with SS1073741824.
#
# What it does test, none of which is platform-specific: that a share over
# 4GB is counted correctly by the index, hashed to completion, advertised
# as one number rather than two, shown as 5.0G by the door's formatter, and
# read back correctly by a second client -- which has its own copy of that
# arithmetic and had the same bug.
#
# This builds a 5GB share, hashes it, and reads the number off the wire.
# Two things make it affordable:
#
#   - The files are sparse (tools/mkbigshare.py): a unique 512-byte header
#     and then a hole, so 5GB costs 180KB of disk and reads back at memory
#     speed. The header is what stops all the files sharing one hash.
#   - The check is the hub's own log, so there is no second client to wait
#     for -- though one joins at the end, because the receiving side parses
#     that number too and it had the same bug.
#
# Hashing 5GB still takes minutes: the hash queue spends a fixed budget per
# main-loop pass on purpose, so this is as fast as the client is.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLIENT="$ROOT/build/host/dosypp"
. "$ROOT/test/host/hublib.sh"

SIZE=${BIGSHARE_SIZE:-5G}
SECS=${BIGSHARE_SECS:-420}
RUN="$(mktemp -d)"
HUB_PORT=$(free_port)

ok=1
fail() { echo "FAIL: $*" >&2; ok=0; }

cleanup() {
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    [ -n "${CLIENT_PID:-}" ] && kill "$CLIENT_PID" 2>/dev/null
    if [ -n "${BIGSHARE_KEEP:-}" ] || [ "$ok" != 1 ]; then
        echo "run dir kept: $RUN"
        return
    fi
    rm -rf "$RUN"
}
trap cleanup EXIT

[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }

# Real software at the top of each area when it has been fetched, filler
# behind it. The test does not need the real files -- the arithmetic is the
# same either way -- but a rig whose share door is worth looking at gets
# looked at.
# An array, not a string: this repository's own path has a space in it, so
# an unquoted "$REAL" splits into two arguments and the generator gets a
# stray positional. That is the third time a space in the path has cost
# half an hour; see run-dosbox-tests.sh for the second.
REAL=()
[ -d "$ROOT/build/atmos" ] && REAL=(--real "$ROOT/build/atmos")

echo "building a $SIZE share$([ ${#REAL[@]} -gt 0 ] && \
      echo " with real software")..."
TOTAL=$("$ROOT/tools/mkbigshare.py" "$RUN/DL" "$SIZE" ${REAL+"${REAL[@]}"} \
        --manifest "$RUN/manifest.txt" | tail -1)
FILES=$(grep -c "" "$RUN/manifest.txt")
echo "$FILES files, $TOTAL bytes, $(du -sh "$RUN/DL" | cut -f1) on disk"

# Above 4GB or this test proves nothing: the bug was a 32-bit print, and a
# 3GB share would have printed correctly all along.
if [ "$TOTAL" -le 4294967295 ]; then
    echo "a $SIZE share is $TOTAL bytes, which is under 4GB -- this test" >&2
    echo "needs more than that to mean anything" >&2
    exit 1
fi

# What the broken code would have said, worked out here so the check can
# look for it by name rather than merely hoping for the right answer.
TRUNCATED=$(python3 -c "print($TOTAL % (2**32))")
echo "a 32-bit print would have said $TRUNCATED"

start_stub_hub "$HUB_PORT" "$RUN/hub.log" --no-fake-share

# Written fresh, never edited: the client rewrites its config on first run
# (it stores the identity there) and pads the keys, so a sed that looks for
# "port = " silently stops matching and the next run dials the old port.
cat > "$RUN/node.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = BigShare
active = no
sound = no
music = no
share = $RUN/DL
EOF

echo "hashing (this is the slow part -- a fixed budget per loop pass)..."
( cd "$RUN" && exec "$CLIENT" -c "$RUN/node.ini" --null-display --door 5 \
    --screenshot "$RUN/share.txt" --run-secs "$SECS" 127.0.0.1 ) \
    > "$RUN/run.log" 2>&1 &
CLIENT_PID=$!

# Wait for the whole share to be advertised rather than for the deadline:
# the number appearing in the hub's log IS the result, so there is nothing
# to gain by sitting out the rest of the run.
START=$(date +%s)
for _ in $(seq $((SECS / 2))); do
    SS=$(grep -aoE "SS[0-9]+" "$RUN/hub.log" 2>/dev/null | tail -1 |
         tr -d 'S')
    [ "${SS:-0}" = "$TOTAL" ] && break
    kill -0 "$CLIENT_PID" 2>/dev/null || break
    sleep 2
    printf '  %s\r' "$(grep -aoE 'SS[0-9]+ SF[0-9]+' "$RUN/hub.log" |
                        tail -1)"
done
echo
echo "hashed in $(( $(date +%s) - START ))s"

# ---- the number on the wire, while the sharer is still on the hub
LAST=$(grep -aoE "SS[0-9]+ SF[0-9]+" "$RUN/hub.log" | tail -1)
echo "last INF: $LAST"

SS=$(printf '%s' "$LAST" | sed -n 's/SS\([0-9]*\).*/\1/p')
SF=$(printf '%s' "$LAST" | sed -n 's/.*SF\([0-9]*\)/\1/p')

if [ -z "$SS" ]; then
    fail "the hub never saw a share size"
elif [ "$SS" = "$TRUNCATED" ]; then
    fail "the share was advertised as $SS -- the total modulo 4GB. On this \
machine that takes more than a 32-bit print to achieve, so something is \
truncating deliberately"
elif [ "$SS" != "$TOTAL" ]; then
    fail "the share was advertised as $SS, not $TOTAL"
else
    echo "advertised $SS bytes, which is the whole share"
fi

[ "$SF" = "$FILES" ] || fail "advertised $SF files, not $FILES"

# ---- the receiving side, which has its own copy of that arithmetic
#
# Run while the sharer is still logged in, or the who door has nothing to
# show: the first version of this test started the peer afterwards and
# passed on an empty list.
echo
echo "a second client, reading that INF..."
cat > "$RUN/peer.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = Nosey
active = no
sound = no
music = no
EOF

( cd "$RUN" && exec "$CLIENT" -c "$RUN/peer.ini" --null-display --door 2 \
    --screenshot "$RUN/who.txt" --run-secs 6 127.0.0.1 ) \
    > "$RUN/peer.log" 2>&1 < /dev/null

echo "--- the who door, as the second client drew it ---"
sed -n '2,8p' "$RUN/who.txt" 2>/dev/null
echo "-------------------------------------------------"

grep -q "BigShare" "$RUN/who.txt" ||
    fail "the second client does not see the sharer at all"

# 5.0G, not 1.0G and not 0B: the parse and the formatter on the reading
# side, which is where the same bug lived a second time.
if grep -qE "BigShare.*5\.0G" "$RUN/who.txt"; then
    echo "it shows the share as 5.0G"
else
    fail "the second client shows $(grep -oE 'BigShare.*' "$RUN/who.txt" |
          head -1 | tr -s ' ')"
fi

kill "$CLIENT_PID" 2>/dev/null
wait "$CLIENT_PID" 2>/dev/null

echo
echo "--- the sharer's own share door ---"
sed -n '4,10p' "$RUN/share.txt" 2>/dev/null
echo "-----------------------------------"

grep -qE "FOUND +$FILES files" "$RUN/share.txt" 2>/dev/null ||
    echo "note: no screenshot from the sharer (it was still running)"

echo
if [ "$ok" = 1 ]; then
    echo "BIGSHARE OK: $TOTAL bytes counted, hashed, advertised and read"
    echo "back. (The 32-bit print itself is tested by dos-bigshare-check:"
    echo "on this machine long is 64 bits and the old code passes here.)"
    exit 0
fi
echo "BIGSHARE FAILED"
exit 1
