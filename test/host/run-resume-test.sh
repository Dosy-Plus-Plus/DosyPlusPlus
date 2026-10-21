#!/bin/bash
# Resume a download that a previous run left half finished.
#
# The interrupted state is built by hand rather than by killing a client
# mid-transfer: over loopback a transfer finishes in milliseconds, so trying
# to interrupt one would be a race, and what needs testing is the recovery,
# not the interruption. So this writes what a crashed run would have left --
# a partial file and a queue file naming it -- and starts a client.
#
# What has to happen, none of which the client is told directly:
#
#   1. Read DOSYPP.QUE and notice the partial file on disk.
#   2. Search for the hash, because the SID of whoever had it last time
#      means nothing in a new session.
#   3. Ask that source for the parts it is missing, and not for the parts
#      it already has.
#   4. End up with a file that matches its TTH.
#
# Since segmented downloading landed, the recovery is per 64KB part rather
# than "everything from byte N": the parts already on disk are recognised by
# their leaf hashes and skipped. That re-fetches at most one part's worth of
# bytes the old scheme would have kept -- and in exchange the parts it keeps
# are verified rather than assumed.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLIENT="$ROOT/build/host/dosypp"
RUN="$(mktemp -d)"
HUB_PORT=${RESUME_HUB_PORT:-24921}
PEER_PORT=${RESUME_PEER_PORT:-24922}
SIZE=${RESUME_SIZE:-200000}
HAVE=${RESUME_HAVE:-80000}

cleanup() {
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    [ -n "${UP_PID:-}" ] && kill "$UP_PID" 2>/dev/null
    pkill -f "dosypp -c $RUN/" 2>/dev/null
    [ -n "${RESUME_KEEP:-}" ] && { echo "run dir kept: $RUN"; return; }
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

TTH=$(rhash --tth "$RUN/up/warez/MAZERUN2.ZIP" | cut -d' ' -f1 | tr 'a-z' 'A-Z')
echo "the file: $SIZE bytes, TTH $TTH"

# What the interrupted run left behind: the first $HAVE bytes, and a queue
# file naming them. The local path is relative, which is what the client
# writes (downloads land beside the program).
head -c "$HAVE" "$RUN/up/warez/MAZERUN2.ZIP" > "$RUN/down/MAZERUN2.ZIP"
echo "already on disk: $(wc -c < "$RUN/down/MAZERUN2.ZIP" | tr -d ' ') bytes"

cat > "$RUN/down/DOSYPP.QUE" <<EOF
; DOSy++ download queue -- rewritten as it changes.
; <tth> <size> <local path> <name>
$TTH $SIZE ./MAZERUN2.ZIP MAZERUN2.ZIP
EOF

cat > "$RUN/up.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = Seeder
sound = no
active = yes
port.listen = $PEER_PORT
share = $RUN/up
EOF

cat > "$RUN/dn.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = Resumer
sound = no
active = no
EOF

"$ROOT/tools/stubhub.py" --port "$HUB_PORT" --no-fake-share \
    > "$RUN/hub.log" 2>&1 &
HUB_PID=$!
for _ in $(seq 20); do
    nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null && break
    sleep 0.25
done

echo "starting the seeder (active, sharing)..."
( cd "$RUN/up" && exec "$CLIENT" -c "$RUN/up.ini" --null-display \
    --door 5 --peer-trace --run-secs 90 127.0.0.1 ) > "$RUN/up.log" 2>&1 &
UP_PID=$!

for _ in $(seq 60); do
    grep -qE "BINF .* SS[1-9]" "$RUN/hub.log" 2>/dev/null && break
    sleep 0.5
done
grep -q "listening on port" "$RUN/up.log" || {
    echo "the seeder is not listening:" >&2; cat "$RUN/up.log" >&2; exit 1; }

echo "starting the client that has half the file..."
( cd "$RUN/down" && exec "$CLIENT" -c "$RUN/dn.ini" \
    --screenshot "$RUN/dn.txt" --null-display --door 4 --peer-trace \
    --run-secs 40 127.0.0.1 ) > "$RUN/dn.log" 2>&1 < /dev/null

echo
echo "--- the transfer door ---"
sed -n '2,8p' "$RUN/dn.txt" 2>/dev/null || echo "(no screenshot)"
echo "-------------------------"

ok=1
fail() { echo "FAIL: $*" >&2; ok=0; }

grep -q "unfinished download" "$RUN/dn.log" ||
    grep -q "restored" "$RUN/dn.log" ||
    echo "note: the restore message is not in the log (it goes to the chat pane)"

# It must have gone looking for a source rather than giving up: the SID from
# the last session is meaningless now.
grep -qE "BSCH .*TR$TTH" "$RUN/hub.log" ||
    fail "the client never searched for the hash it was resuming"

# It must have asked for the leaf hashes: that is how it tells which parts
# of the file on disk are good.
grep -q "CGET tthl TTH/$TTH" "$RUN/dn.log" ||
    fail "it never asked for leaf hashes, so it cannot have checked what it had"

# The distinguishing assertion: it must NOT have asked for byte 0. The first
# 64KB were already on disk and correct, so re-fetching them would mean the
# resume did nothing.
if grep -qE "CGET file TTH/$TTH 0 " "$RUN/dn.log"; then
    fail "it asked for the start of the file again -- nothing was resumed"
else
    echo "it did not ask for the part it already had"
fi

# And it must have fetched less than the whole file.
MOVED=$(grep -oE "CSND file TTH/$TTH [0-9]+ [0-9]+" "$RUN/up.log" |
        awk '{ total += $5 } END { print total+0 }')
echo "bytes the seeder actually sent: $MOVED of $SIZE"
[ "${MOVED:-0}" -gt 0 ] || fail "the seeder sent nothing"
[ "${MOVED:-0}" -lt "$SIZE" ] ||
    fail "it fetched the whole file: $MOVED bytes for a $HAVE-byte head start"

GOT_SIZE=$(wc -c < "$RUN/down/MAZERUN2.ZIP" | tr -d ' ')
GOT_TTH=$(rhash --tth "$RUN/down/MAZERUN2.ZIP" | cut -d' ' -f1 | tr 'a-z' 'A-Z')

echo "result: $GOT_SIZE bytes, TTH $GOT_TTH"
[ "$GOT_SIZE" = "$SIZE" ] || fail "the file is $GOT_SIZE bytes, expected $SIZE"
[ "$GOT_TTH" = "$TTH" ] || fail "the hash does not match the original"

# A completed download must leave no queue entry behind, or the next start
# would truncate the finished file and fetch it again.
if [ -f "$RUN/down/DOSYPP.QUE" ]; then
    if grep -q "$TTH" "$RUN/down/DOSYPP.QUE"; then
        fail "the finished download is still in the queue file"
    fi
fi

echo
if [ "$ok" = 1 ]; then
    echo "RESUME OK: it fetched only the missing part and the hash still matches."
    exit 0
fi

echo "--- client output ---" >&2
tail -20 "$RUN/dn.log" >&2
echo "RESUME FAILED" >&2
exit 1
