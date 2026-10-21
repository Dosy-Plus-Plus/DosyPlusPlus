#!/bin/bash
# Download a file on real DOS, over a packet driver, and verify it by hash.
#
# The DOS client is the PASSIVE downloader and the host client is the ACTIVE
# uploader. That split is forced: DOSBox-X's slirp backend does NAT with no
# inbound mapping, so nothing can dial into the guest. A passive client never
# needs to be dialled -- it sends RCM, the uploader answers with CTM, and the
# guest dials out. (Active mode on DOS needs the pcap backend and a real
# network; that is the hardware pass, not this.)
#
# One subtlety that makes or breaks the whole rig: the address a peer is
# dialled at comes from the I4 field the hub observed, not from the CTM. The
# host uploader connects over loopback, so the hub would record 127.0.0.1 --
# which inside the guest means the guest itself. The stub hub is told to
# advertise 10.0.2.2 instead, which is how slirp presents the host. Pointing
# the uploader at the host's LAN address instead would be more lifelike but
# needs the macOS firewall to allow inbound connections, which it does not
# by default.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNDIR="$ROOT/build/dos/xferrun"
SRC="$ROOT/dosbox-x/c"
CLIENT="$ROOT/build/host/dosypp"
source "$ROOT/test/host/hublib.sh"
# A free port, not 1511: a rig that reuses whatever is already listening
# spends its run testing somebody else's hub. See hublib.sh.
HUB_PORT=${XFER_HUB_PORT:-$(free_port)}
PEER_PORT=${XFER_PEER_PORT:-14141}
SIZE=${XFER_SIZE:-120000}
# A big transfer needs a longer run and a longer alarm. The sustained
# gate (XFER_SIZE=100000000) is about stalls, which only show up over
# minutes -- see docs/AO486.md.
RUN_SECS=${XFER_RUN_SECS:-90}
ALARM=${XFER_ALARM:-400}

command -v dosbox-x >/dev/null 2>&1 || { echo "need dosbox-x (brew install dosbox-x)" >&2; exit 1; }
command -v rhash >/dev/null 2>&1 || { echo "need rhash (brew install rhash)" >&2; exit 1; }
[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }
for f in DOSYPP.EXE CWSDPMI.EXE NE2000.COM; do
    [ -f "$SRC/$f" ] || { echo "missing $SRC/$f (run: make dosfiles)" >&2; exit 1; }
done

# How the host appears from inside slirp.
HOST_AS_GUEST_SEES_IT=10.0.2.2

rm -rf "$RUNDIR"; mkdir -p "$RUNDIR" "$RUNDIR/up/warez"

python3 - "$RUNDIR/up/warez/MAZERUN2.ZIP" "$SIZE" <<'PY'
import random, sys
random.seed(1994)
with open(sys.argv[1], "wb") as f:
    f.write(bytes(random.getrandbits(8) for _ in range(int(sys.argv[2]))))
PY

WANT=$(rhash --tth "$RUNDIR/up/warez/MAZERUN2.ZIP" | cut -d' ' -f1 | tr 'a-z' 'A-Z')
echo "file to move: $SIZE bytes, TTH $WANT"
echo "the guest will dial the host as $HOST_AS_GUEST_SEES_IT:$PEER_PORT"

cp "$SRC"/DOSYPP.EXE "$SRC"/CWSDPMI.EXE "$SRC"/NE2000.COM "$RUNDIR/"

cat > "$RUNDIR/DOSYPP.INI" <<EOF
hub = 10.0.2.2
port = $HUB_PORT
nick = DOSLeech
sound = no
active = no
EOF

cat > "$RUNDIR/up.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = HostSeed
sound = no
active = yes
port.listen = $PEER_PORT
share = $RUNDIR/up
EOF

cleanup() {
    [ -n "${HUB:-}" ] && kill "$HUB" 2>/dev/null
    [ -n "${UP:-}" ] && kill "$UP" 2>/dev/null
}
trap cleanup EXIT

# --no-fake-share so the only answer to the search is the real file.
start_stub_hub "$HUB_PORT" "$RUNDIR/hub.log" --no-fake-share \
    --loopback-as "$HOST_AS_GUEST_SEES_IT"
HUB=$HUB_PID

echo "starting the host uploader (active, sharing)..."
# In its own directory: the share's FILES.XML and FILES.BZ2 are written to
# the working directory, and without this they land wherever the script was
# run from.
( cd "$RUNDIR/up" && exec "$CLIENT" -c "$RUNDIR/up.ini" --null-display --door 5 \
    --peer-trace --run-secs $((RUN_SECS * 2)) 127.0.0.1 ) \
    > "$RUNDIR/up.log" 2>&1 &
UP=$!
for _ in $(seq 120); do
    grep -qE "BINF .* SS[1-9]" "$RUNDIR/hub.log" 2>/dev/null && break
    sleep 0.5
done
grep -q "listening on port" "$RUNDIR/up.log" || {
    echo "the uploader is not listening:" >&2; cat "$RUNDIR/up.log" >&2
    exit 1; }

cat > "$RUNDIR/dosbox-xfer.conf" <<EOF
[sdl]
output = surface
[dosbox]
machine = svga_s3
memsize = 16
[cpu]
core = dynamic
cputype = 486
cycles = max
[dos]
ver = 7.1
lfn = false
[ne2000]
ne2000 = true
nicbase = 300
nicirq = 10
backend = slirp
[autoexec]
mount c "$RUNDIR"
c:
NE2000 0x60 10 0x300 > NUL
DOSYPP.EXE --screenshot SHOT.TXT --null-display --door 4 --peer-trace --search MAZERUN2 --queue --run-secs RUNSECS > RUN.TXT
echo DONE >> RUN.TXT
exit
EOF

echo "downloading on DOS..."
sed -i.bak "s/--run-secs RUNSECS/--run-secs $RUN_SECS/" \
    "$RUNDIR/dosbox-xfer.conf" && rm -f "$RUNDIR/dosbox-xfer.conf.bak"

START=$(date +%s)
SDL_VIDEODRIVER=dummy perl -e "alarm $ALARM; exec @ARGV" \
    dosbox-x -conf "$RUNDIR/dosbox-xfer.conf" -nogui \
    > "$RUNDIR/dosbox.log" 2>&1

echo "--- the transfer door, on DOS ---"
sed -n '2,10p' "$RUNDIR/SHOT.TXT" 2>/dev/null || echo "(no screenshot)"
echo "---------------------------------"

echo "--- connection setup, as the hub saw it ---"
grep -E "RCM|CTM|routed" "$RUNDIR/hub.log" | head -8 || echo "(none)"

GOT=$(ls "$RUNDIR" | grep -iE '\.ZIP$' | head -1)
if [ -z "$GOT" ]; then
    echo "NO FILE WAS DOWNLOADED on DOS" >&2
    echo "--- DOS output ---" >&2
    cat "$RUNDIR/RUN.TXT" 2>/dev/null >&2
    echo "--- uploader ---" >&2
    tail -12 "$RUNDIR/up.log" >&2
    exit 1
fi

HAVE=$(rhash --tth "$RUNDIR/$GOT" | cut -d' ' -f1 | tr 'a-z' 'A-Z')
SZ=$(wc -c < "$RUNDIR/$GOT" | tr -d ' ')
echo "downloaded on DOS as: $GOT ($SZ bytes)"
echo "  expected TTH: $WANT"
echo "  actual   TTH: $HAVE"

# Throughput, measured rather than assumed: a sustained transfer that
# stalls in bursts -- ao486's known PIO weakness -- shows up here as a
# number, where a hang would otherwise just look like a timeout.
ELAPSED=$(( $(date +%s) - START ))
[ "$ELAPSED" -gt 0 ] || ELAPSED=1
if [ -n "${SZ:-}" ] && [ "${SZ:-0}" -gt 0 ]; then
    echo "moved $SZ bytes in ${ELAPSED}s -- $(( SZ / ELAPSED / 1024 )) KB/s \
including boot, login and hashing"
fi

if [ "$HAVE" = "$WANT" ] && [ "$SZ" = "$SIZE" ]; then
    echo "TRANSFER OK on DOS: the file arrived intact over the packet driver."
    exit 0
fi

echo "TRANSFER FAILED on DOS: content does not match" >&2
exit 1
