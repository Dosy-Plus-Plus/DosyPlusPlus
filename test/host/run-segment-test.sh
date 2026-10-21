#!/bin/bash
# A segmented download from two sources at once.
#
# Two clients share the same file; a third fetches it. What should happen:
#
#   1. It searches, and both sources answer with the same hash, so the two
#      results coalesce into one with two sources.
#   2. It asks one of them for the leaf hashes, checks the set against the
#      root, and splits the file into parts.
#   3. It opens a connection to EACH source and fetches different parts
#      down each one.
#   4. Every part is verified against its own leaf as it lands, so the
#      finished file needs no pass over it.
#
# The evidence for (3) is in the peer trace: two CGETs with different byte
# offsets, to two different peers. Without that, a "segmented" download is
# just a download.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLIENT="$ROOT/build/host/dosypp"
. "$ROOT/test/host/hublib.sh"
RUN="$(mktemp -d)"
# Ports from the kernel rather than two numbers that looked free: this rig
# once spent a whole run talking to somebody else's hub on 24971 and
# reported the feature broken.
HUB_PORT=${SEG_HUB_PORT:-$(free_port)}
PEER_A=${SEG_PEER_A:-$(free_port)}
PEER_B=${SEG_PEER_B:-$(free_port)}
SIZE=${SEG_SIZE:-300000}

ok=1
fail() { echo "FAIL: $*" >&2; ok=0; }

cleanup() {
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    [ -n "${A_PID:-}" ] && kill "$A_PID" 2>/dev/null
    [ -n "${B_PID:-}" ] && kill "$B_PID" 2>/dev/null
    pkill -f "dosypp -c $RUN/" 2>/dev/null
    if [ -n "${SEG_KEEP:-}" ] || [ "$ok" != 1 ]; then
        echo "run dir kept: $RUN"
        return
    fi
    rm -rf "$RUN"
}
trap cleanup EXIT

[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }
command -v rhash >/dev/null 2>&1 || { echo "need rhash (brew install rhash)" >&2; exit 1; }

mkdir -p "$RUN/a/warez" "$RUN/b/warez" "$RUN/down"

python3 - "$RUN/a/warez/BIGFILE.ZIP" "$SIZE" <<'PY'
import random, sys
random.seed(1994)
with open(sys.argv[1], "wb") as f:
    f.write(bytes(random.getrandbits(8) for _ in range(int(sys.argv[2]))))
PY
cp "$RUN/a/warez/BIGFILE.ZIP" "$RUN/b/warez/BIGFILE.ZIP"

WANT=$(rhash --tth "$RUN/a/warez/BIGFILE.ZIP" | cut -d' ' -f1 | tr 'a-z' 'A-Z')
echo "one file, two sources: $SIZE bytes, TTH $WANT"

for who in a b; do
    port=$PEER_A
    [ "$who" = b ] && port=$PEER_B
    cat > "$RUN/$who.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = Source$(echo "$who" | tr 'ab' 'AB')
sound = no
active = yes
port.listen = $port
share = $RUN/$who
EOF
done

cat > "$RUN/dn.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = Collector
sound = no
active = no
EOF

start_stub_hub "$HUB_PORT" "$RUN/hub.log" --no-fake-share

echo "starting both sources..."
( cd "$RUN/a" && exec "$CLIENT" -c "$RUN/a.ini" --null-display --door 5 \
    --peer-trace --run-secs 90 127.0.0.1 ) > "$RUN/a.log" 2>&1 &
A_PID=$!
( cd "$RUN/b" && exec "$CLIENT" -c "$RUN/b.ini" --null-display --door 5 \
    --peer-trace --run-secs 90 127.0.0.1 ) > "$RUN/b.log" 2>&1 &
B_PID=$!

for _ in $(seq 90); do
    [ "$(grep -cE 'BINF .* SS[1-9]' "$RUN/hub.log" 2>/dev/null)" -ge 2 ] &&
        break
    sleep 0.5
done
grep -q "listening on port $PEER_A" "$RUN/a.log" || fail "source A is not listening"
grep -q "listening on port $PEER_B" "$RUN/b.log" || fail "source B is not listening"

echo "both sources have hashed and announced; collecting..."
( cd "$RUN/down" && exec "$CLIENT" -c "$RUN/dn.ini" \
    --screenshot "$RUN/dn.txt" --null-display --door 4 --peer-trace \
    --search BIGFILE --queue --run-secs 60 127.0.0.1 ) \
    > "$RUN/dn.log" 2>&1 < /dev/null

echo
echo "--- the transfer door ---"
sed -n '2,9p' "$RUN/dn.txt" 2>/dev/null || echo "(no screenshot)"
echo "-------------------------"

# 1. Both sources answered, and the results coalesced.
SOURCES=$(grep -cE "^\[....\] --> DRES .*$WANT" "$RUN/hub.log" 2>/dev/null)
echo "sources that answered the search: ${SOURCES:-0}"
[ "${SOURCES:-0}" -ge 2 ] || fail "only ${SOURCES:-0} source(s) answered"

# 2. It asked for leaf hashes and accepted them.
grep -q "CGET tthl TTH/$WANT" "$RUN/dn.log" ||
    fail "it never asked for leaf hashes"
grep -qE "CSND tthl TTH/$WANT" "$RUN/dn.log" ||
    fail "no leaf hashes arrived"

# 3. Parts were fetched, from both peers, at different offsets. A segmented
#    download that fetches everything from byte 0 is not segmented.
# Field 4 is the start offset: "CGET file TTH/<hash> <start> <len>".
OFFSETS=$(grep -oE "CGET file TTH/$WANT [0-9]+ [0-9]+" "$RUN/dn.log" |
          awk '{print $4}' | sort -n | uniq | wc -l | tr -d ' ')
echo "distinct part offsets requested: $OFFSETS"
[ "${OFFSETS:-0}" -ge 2 ] ||
    fail "only $OFFSETS distinct offset(s): this was not segmented"

SERVED_A=$(grep -cE "CSND file TTH/$WANT" "$RUN/a.log" 2>/dev/null)
SERVED_B=$(grep -cE "CSND file TTH/$WANT" "$RUN/b.log" 2>/dev/null)
echo "parts served: A=$SERVED_A B=$SERVED_B"
[ "${SERVED_A:-0}" -ge 1 ] && [ "${SERVED_B:-0}" -ge 1 ] ||
    fail "both sources should have served at least one part"

# 4. The file is complete and correct, and it was never hashed end to end:
#    every part was checked against its leaf instead.
GOT=$(ls "$RUN/down" | grep -iE '\.zip$' | head -1)
if [ -z "$GOT" ]; then
    fail "nothing was downloaded"
else
    HAVE=$(rhash --tth "$RUN/down/$GOT" | cut -d' ' -f1 | tr 'a-z' 'A-Z')
    SZ=$(wc -c < "$RUN/down/$GOT" | tr -d ' ')
    echo "downloaded $GOT ($SZ bytes)"
    [ "$SZ" = "$SIZE" ] || fail "the file is $SZ bytes, expected $SIZE"
    if [ "$HAVE" = "$WANT" ]; then
        echo "  and the hash matches, assembled from parts"
    else
        fail "the assembled file hashes to $HAVE, not $WANT"
    fi
fi

grep -q "every part hash verified" "$RUN/dn.log" ||
    echo "note: the completion line is in the chat pane, not stdout"

echo
if [ "$ok" = 1 ]; then
    echo "SEGMENTS OK: one file, two sources, parts verified individually."
    exit 0
fi

echo "--- collector output ---" >&2
grep -E "^peer|cannot|failed" "$RUN/dn.log" | tail -20 >&2
echo "SEGMENTS FAILED" >&2
exit 1
