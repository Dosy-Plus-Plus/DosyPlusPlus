#!/bin/bash
# Boot DOSy++ on DOS for real: load the packet driver, bring up Watt-32 via
# DHCP, and run a live ADC handshake against a hub on the host machine.
#
# This is the first test that exercises the whole stack as it will ship --
# packet driver, Watt-32, sockets, the ADC tokenizer -- rather than any
# host-side stand-in.
#
#   ./test/dos/run-dosbox-net.sh [hub-ip] [port]
#
# With no arguments it starts tools/stubhub.py on the host and points DOS at
# it. Under DOSBox-X's slirp backend the host is always 10.0.2.2.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNDIR="$ROOT/build/dos/netrun"
SRC="$ROOT/dosbox-x/c"

HUB_IP="${1:-10.0.2.2}"
HUB_PORT="${2:-1511}"
OWN_HUB=0

command -v dosbox-x >/dev/null 2>&1 || {
    echo "dosbox-x not found (brew install dosbox-x)" >&2; exit 1; }

need() {
    [ -f "$SRC/$1" ] || { echo "missing $SRC/$1 -- $2" >&2; exit 1; }
}
need DOSYPP.EXE  "run: make dos-docker && cp build/dos/DOSYPP.EXE dosbox-x/c/"
need CWSDPMI.EXE "run: make dosfiles"
need NE2000.COM  "run: make pktdrv"
need WATTCP.CFG  "should be in the repo"

rm -rf "$RUNDIR"; mkdir -p "$RUNDIR"
cp "$SRC/DOSYPP.EXE" "$SRC/CWSDPMI.EXE" "$SRC/NE2000.COM" "$SRC/WATTCP.CFG" "$RUNDIR/"

if [ "$HUB_PORT" = "1511" ] && [ "$HUB_IP" = "10.0.2.2" ]; then
    if ! nc -z 127.0.0.1 1511 2>/dev/null; then
        echo "starting stub hub on the host..."
        "$ROOT/tools/stubhub.py" --port 1511 > "$RUNDIR/hub.log" 2>&1 &
        OWN_HUB=$!
        sleep 1
    else
        echo "something is already listening on 1511 -- using it"
    fi
fi
trap '[ "$OWN_HUB" != 0 ] && kill "$OWN_HUB" 2>/dev/null' EXIT

# slirp: user-mode NAT, no privileges, guest gets 10.0.2.15 by DHCP and the
# host is 10.0.2.2. Outbound only -- fine, DOSy++ is passive-mode here.
cat > "$RUNDIR/dosbox-net.conf" <<EOF
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
macaddr = AC:DE:48:88:99:AA
backend = slirp
[autoexec]
mount c "$RUNDIR"
c:
echo === loading packet driver >> RESULT.TXT
NE2000 0x60 10 0x300 >> RESULT.TXT
echo === running dosypp >> RESULT.TXT
DOSYPP.EXE --batch -p $HUB_PORT $HUB_IP >> RESULT.TXT
echo === exit code %ERRORLEVEL% >> RESULT.TXT
echo DONE >> RESULT.TXT
exit
EOF

echo "booting DOSBox-X: NE2000 + slirp, hub at $HUB_IP:$HUB_PORT"
SDL_VIDEODRIVER=dummy perl -e 'alarm 240; exec @ARGV' \
    dosbox-x -conf "$RUNDIR/dosbox-net.conf" -nogui \
    > "$RUNDIR/dosbox.log" 2>&1
rc=$?

RESULT="$RUNDIR/RESULT.TXT"
if [ ! -f "$RESULT" ]; then
    echo "no RESULT.TXT -- DOSBox-X exited $rc" >&2
    grep -iE "ne2000|slirp|error" "$RUNDIR/dosbox.log" | tail -10 >&2
    exit 1
fi

echo "--- output from DOS ---"
cat "$RESULT"
echo "-----------------------"

if [ -s "$RUNDIR/hub.log" ]; then
    echo "--- what the hub saw ---"
    cat "$RUNDIR/hub.log"
    echo "------------------------"
fi

grep -q "^DONE" "$RESULT" || { echo "FAILED: did not run to completion" >&2; exit 1; }

if grep -q "PHASE 2 OK" "$RESULT"; then
    echo "LIVE OK: DOSy++ logged in from DOS over a packet driver and has the user list."
    exit 0
fi

echo "FAILED: no successful handshake. See the output above." >&2
exit 1
