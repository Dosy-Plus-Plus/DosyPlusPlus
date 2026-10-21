#!/bin/bash
# A file leaving a browser tab: the transfer gate for the v86 host.
#
# make test-v86 proves DOSy++ boots in v86 and logs in. That is a handshake,
# not a client. This moves a file: the guest shares two demo files from its
# image, a host client searches the hub, queues one and downloads it, and
# the bytes are checked against the original's hash.
#
# The direction is forced by what Wisp can do. It carries outbound streams
# only -- nothing can dial into a browser tab -- so a transfer only works
# when the guest is the side doing the dialling. In ADC that means the guest
# is the UPLOADER answering an active downloader's CTM:
#
#   host (active, listening)  --CTM-->  hub  -->  guest
#   guest  --dials--> 10.77.0.2:port --> relay --> host's listener
#
# 10.77.0.2 is a sentinel: the stub hub is told to advertise it as the host
# client's address (--i4-for), and the relay maps it to the host's real
# port. The guest therefore needs no DNS and no knowledge of the host's
# address, which is the same trick the hub mapping uses.
set -uo pipefail

# mtools asks about a name clash, and asks on stdin: an mmd for a
# directory that exists, or an mcopy without -o, waits for an answer that
# no rig is going to give. Non-interactive runs get EOF and carry on;
# a person running this in a terminal waits for ever, with the question
# usually hidden by a 2>/dev/null. One line removes the whole class.
exec < /dev/null


ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLIENT="$ROOT/build/host/dosypp"
RUN="$ROOT/build/v86xfer"
. "$ROOT/test/host/hublib.sh"

command -v node >/dev/null 2>&1 || { echo "need node (brew install node)" >&2; exit 1; }
command -v mcopy >/dev/null 2>&1 || { echo "need mtools (brew install mtools)" >&2; exit 1; }
[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }

for f in web/vendor/libv86.mjs web/images/boot.img web/images/dosypp.img; do
    [ -f "$ROOT/$f" ] || { echo "missing $f -- run: make images" >&2; exit 1; }
done

RELAY_PORT=$(free_port)
HUB_PORT=$(free_port)
HOST_PEER_PORT=$(free_port)
GUEST_AS=10.77.0.1          # the hub, from inside the guest
HOST_AS=10.77.0.2           # the host client, from inside the guest

export MTOOLS_SKIP_CHECK=1
rm -rf "$RUN"; mkdir -p "$RUN/down"

ok=1
fail() { echo "FAIL: $*" >&2; ok=0; }

cleanup() {
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    [ -n "${RELAY_PID:-}" ] && kill "$RELAY_PID" 2>/dev/null
    [ -n "${DOWN_PID:-}" ] && kill "$DOWN_PID" 2>/dev/null
    [ -n "${GUEST_PID:-}" ] && kill "$GUEST_PID" 2>/dev/null
}
trap cleanup EXIT

# The guest's image carries AUTOXFER.BAT; the boot floppy has to call it
# instead of the login test's AUTOTEST.BAT.
cp "$ROOT/web/images/boot.img" "$RUN/bootxfer.img"
cat > "$RUN/FDAUTO.BAT" <<'EOF'
@echo off
set DOSDIR=A:\FREEDOS
path A:\BIN;C:\
c:
call C:\AUTOXFER.BAT
EOF
mcopy -i "$RUN/bootxfer.img" -o "$RUN/FDAUTO.BAT" ::FDAUTO.BAT

mdir -i "$ROOT/web/images/dosypp.img" ::AUTOXFER.BAT >/dev/null 2>&1 || {
    echo "dosypp.img has no AUTOXFER.BAT -- run: make images" >&2; exit 1; }

# What the guest is sharing, so the download can be checked against it. The
# image's copies are generated deterministically by mkv86img.sh; these are
# the same bytes from the same seeds.
python3 - "$RUN" <<'PY'
import os, random, sys
run = sys.argv[1]
for name, size, seed in [("DEMO1.BIN", 64 * 1024, 1994),
                         ("DEMO2.BIN", 32 * 1024, 1995)]:
    random.seed(seed)
    with open(os.path.join(run, name), "wb") as f:
        f.write(bytes(random.getrandbits(8) for _ in range(size)))
PY

command -v rhash >/dev/null 2>&1 && {
    WANT_TTH=$(rhash --tth "$RUN/DEMO1.BIN" | cut -d' ' -f1 | tr 'a-z' 'A-Z')
    echo "DEMO1.BIN should hash to $WANT_TTH"
}

# The hub, advertising the host client at the sentinel address. Without this
# the guest would be told to dial 127.0.0.1, which inside the guest is the
# guest.
start_stub_hub "$HUB_PORT" "$RUN/hub.log" --no-fake-share \
    --i4-for "HostPeer=$HOST_AS"

"$ROOT/tools/wisprelay.py" --port "$RELAY_PORT" \
    --map "$GUEST_AS=127.0.0.1:$HUB_PORT" \
    --map "$HOST_AS=127.0.0.1:$HOST_PEER_PORT" \
    > "$RUN/relay.log" 2>&1 &
RELAY_PID=$!
for _ in $(seq 40); do
    grep -q "wisprelay: http" "$RUN/relay.log" 2>/dev/null && break
    sleep 0.25
done
kill -0 "$RELAY_PID" 2>/dev/null || {
    echo "the relay did not start:" >&2; cat "$RUN/relay.log" >&2; exit 1; }
echo "relay on $RELAY_PORT: $GUEST_AS -> hub, $HOST_AS -> :$HOST_PEER_PORT"

cat > "$RUN/down.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = HostPeer
sound = no
music = no
active = yes
port.listen = $HOST_PEER_PORT
EOF

# The guest boots first, in the background.
#
# The order matters and the first version of this got it wrong: the
# downloader searches once, a few seconds after it logs in, and the guest
# needs about two minutes to boot FreeDOS, load the driver, log in and hash
# its share. A search sent before any of that found nothing, was never
# repeated, and the rig timed out with everything working.
echo "booting the browser guest..."
node "$ROOT/test/v86/boot.mjs" \
    "wisp://localhost:$RELAY_PORT/" \
    "$RUN/bootxfer.img" \
    "$ROOT/web/images/dosypp.img" \
    "${1:-300}" \
    "upload done: file" \
    "AUTOXFER-DONE" > "$RUN/guest.log" 2>&1 &
GUEST_PID=$!

# Wait for the guest to be a usable source: logged in AND its share
# hashed, which is what SS with a non-zero size in its INF means. Searching
# before that is searching an empty share.
echo "waiting for the guest to log in and hash its share..."
for _ in $(seq 240); do
    grep -qaE "BINF .* SS[1-9][0-9]* SF[1-9]" "$RUN/hub.log" 2>/dev/null &&
        break
    kill -0 "$GUEST_PID" 2>/dev/null || break
    sleep 1
done

if ! grep -qaE "BINF .* SS[1-9][0-9]* SF[1-9]" "$RUN/hub.log" 2>/dev/null; then
    echo "the guest never advertised a hashed share:" >&2
    tail -20 "$RUN/guest.log" >&2
    kill "$GUEST_PID" 2>/dev/null
    exit 1
fi
echo "the guest is sharing: $(grep -aoE 'SS[0-9]+ SF[0-9]+' "$RUN/hub.log" | tail -1)"

# Now the downloader: active, so its CTM tells the guest where to dial.
echo "starting the host downloader (active, listening on $HOST_PEER_PORT)..."
( cd "$RUN/down" && exec "$CLIENT" -c "$RUN/down.ini" --null-display \
    --door 4 --peer-trace --search DEMO1 --queue --run-secs 90 \
    --screenshot "$RUN/down.txt" 127.0.0.1 ) > "$RUN/down.log" 2>&1 &
DOWN_PID=$!

wait "$GUEST_PID"
rc=$?

echo
echo "--- the guest (COM1) ---"
grep -aE "CSND|CGET|CSUP|logged|PHASE|harness\]" "$RUN/guest.log" | tail -12
echo "--- the relay ---";  tail -14 "$RUN/relay.log"
echo "--- the hub ---";    grep -aE "logged in|SS[0-9]|CTM|RCM" "$RUN/hub.log" | tail -8

[ $rc -eq 0 ] || fail "the guest never served a file (harness exit $rc)"

# Give the host side a moment to finish writing what the guest sent.
for _ in $(seq 40); do
    [ -n "$(ls "$RUN/down" 2>/dev/null | grep -i 'DEMO1')" ] && break
    sleep 0.5
done
kill "$DOWN_PID" 2>/dev/null
wait "$DOWN_PID" 2>/dev/null

echo
echo "--- what the host downloaded ---"
ls -l "$RUN/down" | grep -viE 'DOSYPP|\.ini|\.txt' | sed 's/^/  /'

GOT=$(ls "$RUN/down" | grep -i 'DEMO1' | head -1)
if [ -z "$GOT" ]; then
    fail "nothing arrived from the browser guest"
else
    SZ=$(wc -c < "$RUN/down/$GOT" | tr -d ' ')
    echo "got $GOT ($SZ bytes)"
    [ "$SZ" = 65536 ] || fail "$GOT is $SZ bytes, expected 65536"

    if command -v rhash >/dev/null 2>&1; then
        HAVE=$(rhash --tth "$RUN/down/$GOT" | cut -d' ' -f1 | tr 'a-z' 'A-Z')
        if [ "$HAVE" = "${WANT_TTH:-}" ]; then
            echo "  and its hash matches the file in the image"
        else
            fail "the hash does not match: got $HAVE, wanted ${WANT_TTH:-?}"
        fi
    fi
fi

# The guest really did the serving: its own trace, and the hub's record of
# the connection request that sent it there.
grep -qa "logged in as 'WebNode'" "$RUN/hub.log" ||
    fail "the guest never logged in"
grep -qaE "CTM|RCM" "$RUN/hub.log" ||
    fail "no connection request crossed the hub"

echo
if [ "$ok" = 1 ]; then
    echo "V86 TRANSFER OK: a file left a browser tab, through the Wisp relay,"
    echo "and arrived with the right hash."
    exit 0
fi
echo "V86 TRANSFER FAILED"
exit 1
