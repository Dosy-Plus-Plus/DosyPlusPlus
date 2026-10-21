#!/bin/bash
# Download from a peer that refuses leaf hashes.
#
# Plenty of uploaders have no leaves to give: clients older than segmented
# downloading, hub-side proxies, and the demo hub in tools/stubhub.py, which
# cannot compute Tiger in Python. The client asks for leaves first, so a
# refusal is on the path to every one of those downloads.
#
# It used to be fatal. The refusal failed the connection with the request's
# only source already counted against it, so the file went to FAILED and
# could not be downloaded at all -- and in the other direction, leaves that
# arrived but yielded no plan closed the connection cleanly, which the queue
# settled as a success: DONE, with nothing fetched. Both are one fallback in
# settle() now.
#
# This is the host-side gate for it. make test-v86-dl does the same thing
# the slow way, through the emulator.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN="$ROOT/build/attic"
CLIENT="$ROOT/build/host/dosypp"
HUB_PORT=11531
ATTIC_PORT=11532
WAREZ="${WAREZ_DIR:-$ROOT/build/warez}"

[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }
[ -f "$WAREZ/SHARE.TXT" ] || {
    echo "no share to download from -- run: make warez" >&2; exit 1; }

WANT=$(awk '/BLKDROP.ZIP/ {print $1; exit}' "$WAREZ/SHARE.TXT")
SIZE=$(awk '/BLKDROP.ZIP/ {print $2; exit}' "$WAREZ/SHARE.TXT")
SRC=$(awk '/BLKDROP.ZIP/ {print $3; exit}' "$WAREZ/SHARE.TXT")
[ -n "$WANT" ] || { echo "the share has no BLKDROP.ZIP" >&2; exit 1; }

rm -rf "$RUN"; mkdir -p "$RUN/down"

cleanup() {
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
}
trap cleanup EXIT

"$ROOT/tools/stubhub.py" --port "$HUB_PORT" --no-fake-share \
    --serve "$WAREZ" --serve-ip 127.0.0.1 --serve-port "$ATTIC_PORT" \
    > "$RUN/hub.log" 2>&1 &
HUB_PID=$!

for _ in $(seq 40); do
    grep -q "ctrl-c to stop" "$RUN/hub.log" 2>/dev/null && break
    sleep 0.25
done
grep -q "attic" "$RUN/hub.log" || {
    echo "the hub did not start its serving node:" >&2
    cat "$RUN/hub.log" >&2; exit 1; }

# Passive, like the browser guest: the peer is the one that can be dialled.
cat > "$RUN/dn.ini" <<EOF
hub    = 127.0.0.1
port   = $HUB_PORT
nick   = ATTICTEST
active = no
music  = no
sound  = no
EOF

( cd "$RUN/down" && exec "$CLIENT" -c "$RUN/dn.ini" \
    --screenshot "$RUN/dn.txt" --null-display --door 4 --peer-trace \
    --search BLKDROP --queue --run-secs "${1:-45}" 127.0.0.1 ) \
    > "$RUN/dn.log" 2>&1

FAILED=0
fail() { echo "FAIL: $*"; FAILED=1; }

echo "--- what the peer was asked ---"
grep -E "CGET|CSND|CSTA" "$RUN/dn.log" | sed 's/^/  /'

echo
grep -q "CGET tthl" "$RUN/dn.log" ||
    fail "the client never asked for leaf hashes -- this rig is then
       testing nothing, because the refusal is what it is about"
grep -q "CSTA 151 Leaf" "$RUN/dn.log" ||
    fail "the peer never refused the leaves"
grep -q "CGET file TTH/$WANT" "$RUN/dn.log" ||
    fail "the client did not fall back to fetching the whole file"
grep -q "download done: BLKDROP.ZIP" "$RUN/dn.log" ||
    fail "the download never completed"

GOT="$RUN/down/BLKDROP.ZIP"
if [ ! -f "$GOT" ]; then
    fail "no BLKDROP.ZIP on disk"
else
    HAVE=$(wc -c < "$GOT" | tr -d ' ')
    [ "$HAVE" = "$SIZE" ] || fail "$HAVE bytes on disk, $SIZE advertised"
    if ! cmp -s "$GOT" "$WAREZ/$SRC"; then
        fail "the bytes differ from the original"
    else
        echo "$HAVE bytes, identical to $SRC"
    fi
fi

# The client reports a completed download only after the TTH matches, so
# the above is already a hash check -- but say so with the hash in hand,
# because "download done" does not read like one.
if command -v rhash >/dev/null 2>&1 && [ -f "$GOT" ]; then
    HASH=$(rhash --tth "$GOT" | cut -d' ' -f1 | tr 'a-z' 'A-Z')
    [ "$HASH" = "$WANT" ] ||
        fail "rhash says $HASH, the hub advertised $WANT"
    echo "rhash agrees with what the hub advertised: $HASH"
fi

if [ "$FAILED" -eq 0 ]; then
    echo
    echo "ATTIC TRANSFER OK: a peer that refuses leaf hashes is still a"
    echo "source -- the client fell back to the whole file and verified it."
    exit 0
fi

echo
echo "ATTIC TRANSFER FAILED (logs in $RUN)"
exit 1
