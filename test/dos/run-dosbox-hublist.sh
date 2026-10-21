#!/bin/bash
# Fetch a public hub list on real DOS, over a packet driver.
#
# HTTP is new code on this target: Watt-32 sockets, a blocking GET, and a
# bzip2 stream decompressed inside 16MB. The host build says nothing about
# whether any of that works on DOS.
#
# A plain Python web server on the host serves the list; slirp lets the guest
# reach the host at 10.0.2.2, which is the same route the hub uses.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNDIR="$ROOT/build/dos/hublistrun"
SRC="$ROOT/dosbox-x/c"
HUB_PORT=${HUBLIST_HUB_PORT:-1511}
WEB_PORT=${HUBLIST_WEB_PORT:-14180}

command -v dosbox-x >/dev/null 2>&1 || { echo "need dosbox-x (brew install dosbox-x)" >&2; exit 1; }
for f in DOSYPP.EXE CWSDPMI.EXE NE2000.COM; do
    [ -f "$SRC/$f" ] || { echo "missing $SRC/$f (run: make dosfiles)" >&2; exit 1; }
done

rm -rf "$RUNDIR"
mkdir -p "$RUNDIR" "$RUNDIR/web"

# A list naming the hub the guest is already on, plus entries it must
# refuse. Compressed by Python, so the decompressor on DOS is being fed
# somebody else's bzip2 rather than its own.
python3 - "$RUNDIR/web/hublist.xml.bz2" "$HUB_PORT" <<'MAKELIST'
import bz2, sys
out, port = sys.argv[1], int(sys.argv[2])
xml = (
    '<?xml version="1.0" encoding="utf-8"?>\n'
    '<Hublist><Hubs>\n'
    '  <Hub Name="Slirp Test Hub" Address="adc://10.0.2.2:%d"\n'
    '       Description="the one the guest is on" Users="17"\n'
    '       Shared="5497558138880"/>\n'
    '  <Hub Name="Old Protocol" Address="dchub://nmdc.example" Users="900"/>\n'
    '  <Hub Name="Encrypted" Address="adcs://secure.example:2781" Users="800"/>\n'
    '</Hubs></Hublist>\n' % port
)
with open(out, "wb") as f:
    f.write(bz2.compress(xml.encode("utf-8")))
print("wrote a bzip2 hub list")
MAKELIST

cp "$SRC"/DOSYPP.EXE "$SRC"/CWSDPMI.EXE "$SRC"/NE2000.COM "$RUNDIR/"
[ -f "$SRC/WATTCP.CFG" ] && cp "$SRC/WATTCP.CFG" "$RUNDIR/"

cat > "$RUNDIR/DOSYPP.INI" <<EOF
hub = 10.0.2.2
port = $HUB_PORT
nick = DOSWanderer
sound = no
active = no
hublist = http://10.0.2.2:$WEB_PORT/hublist.xml.bz2
EOF

cleanup() {
    [ -n "${HUB:-}" ] && kill "$HUB" 2>/dev/null
    [ -n "${WEB:-}" ] && kill "$WEB" 2>/dev/null
}
trap cleanup EXIT

"$ROOT/tools/stubhub.py" --port "$HUB_PORT" > "$RUNDIR/hub.log" 2>&1 &
HUB=$!
( cd "$RUNDIR/web" && exec python3 -m http.server "$WEB_PORT" ) \
    > "$RUNDIR/web.log" 2>&1 &
WEB=$!

for _ in $(seq 20); do
    nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null &&
        nc -z 127.0.0.1 "$WEB_PORT" 2>/dev/null && break
    sleep 0.25
done
nc -z 127.0.0.1 "$WEB_PORT" 2>/dev/null || {
    echo "the web server did not start" >&2
    cat "$RUNDIR/web.log" >&2
    exit 1; }

echo "hub on $HUB_PORT, list served on $WEB_PORT"

cat > "$RUNDIR/dosbox-hublist.conf" <<EOF
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
DOSYPP.EXE --screenshot SHOT.TXT --null-display --door 10 --fetch-hublist --run-secs 40 > RUN.TXT
echo DONE >> RUN.TXT
exit
EOF

echo "fetching the hub list on DOS..."
SDL_VIDEODRIVER=dummy perl -e 'alarm 400; exec @ARGV' \
    dosbox-x -conf "$RUNDIR/dosbox-hublist.conf" -nogui \
    > "$RUNDIR/dosbox.log" 2>&1

echo "--- the hub list door, on DOS ---"
sed -n '2,12p' "$RUNDIR/SHOT.TXT" 2>/dev/null || echo "(no screenshot)"
echo "---------------------------------"

ok=1
fail() { echo "FAIL: $*" >&2; ok=0; }

grep -qE '"GET /hublist.xml.bz2 HTTP/1.[01]" 200' "$RUNDIR/web.log" ||
    fail "DOS never fetched the list (the web server saw nothing)"

[ -f "$RUNDIR/SHOT.TXT" ] || fail "no screenshot came back from DOS"
grep -q "PUBLIC HUB LIST" "$RUNDIR/SHOT.TXT" 2>/dev/null ||
    fail "the hub list door is not showing"
grep -q "Slirp Test Hub" "$RUNDIR/SHOT.TXT" 2>/dev/null ||
    fail "the list did not parse on DOS"
grep -q "Old Protocol" "$RUNDIR/SHOT.TXT" 2>/dev/null &&
    fail "an NMDC hub was offered on DOS"
grep -qi "could not be fetched" "$RUNDIR/SHOT.TXT" 2>/dev/null &&
    fail "DOS could not fetch the list"

# The download is a temporary and must not be left behind.
[ -f "$RUNDIR/HUBLIST.TMP" ] &&
    fail "the downloaded list was left on the DOS disk"

echo
if [ "$ok" = 1 ]; then
    echo "HUBLIST OK on DOS: HTTP and bzip2 work over the packet driver."
    exit 0
fi

echo "--- DOS output ---" >&2
cat "$RUNDIR/RUN.TXT" 2>/dev/null >&2
echo "HUBLIST FAILED on DOS" >&2
exit 1
