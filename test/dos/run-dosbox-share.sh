#!/bin/bash
# Scan and hash a share on real DOS, then check what it produced.
#
# This is the half of Phase 5 the host build cannot exercise: FINDFIRST and
# FINDNEXT, DOS 8.3 names, packed DOS timestamps in the hash cache, and the
# whole thing inside 16MB.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNDIR="$ROOT/build/dos/sharerun"
SRC="$ROOT/dosbox-x/c"
source "$ROOT/test/host/hublib.sh"
# A free port, not 1511. This rig used to reuse whatever was already
# listening there, which meant it read an empty log and reported the share
# size broken whenever anything else on the machine held the port. That is
# the failure hublib.sh exists for.
HUB_PORT=${SHARE_HUB_PORT:-$(free_port)}

command -v dosbox-x >/dev/null 2>&1 || { echo "need dosbox-x (brew install dosbox-x)" >&2; exit 1; }
for f in DOSYPP.EXE CWSDPMI.EXE NE2000.COM; do
    [ -f "$SRC/$f" ] || { echo "missing $SRC/$f" >&2; exit 1; }
done

rm -rf "$RUNDIR"; mkdir -p "$RUNDIR/DL/WAREZ" "$RUNDIR/DL/MODS"
cp "$SRC"/DOSYPP.EXE "$SRC"/CWSDPMI.EXE "$SRC"/NE2000.COM "$RUNDIR/"

# A share with 8.3 names, which is all DOS can offer.
python3 - "$RUNDIR" <<'PY'
import os, sys, random
root = sys.argv[1]
random.seed(1994)
for path, size in [("DL/WAREZ/MAZERUN2.ZIP", 120_000),
                   ("DL/WAREZ/ARCPACK2.EXE",  48_000),
                   ("DL/MODS/TUNE0001.MOD",    64_000),
                   ("DL/README.TXT",          4_000)]:
    full = os.path.join(root, path)
    with open(full, "wb") as f:
        f.write(bytes(random.getrandbits(8) for _ in range(size)))
print("made the share")
PY

cat > "$RUNDIR/DOSYPP.INI" <<EOF
hub = 10.0.2.2
port = $HUB_PORT
nick = DOSShare
sound = no
share = C:\\DL
EOF

start_stub_hub "$HUB_PORT" "$RUNDIR/hub.log"
HUB=$HUB_PID
trap 'kill "$HUB" 2>/dev/null' EXIT

cat > "$RUNDIR/dosbox-share.conf" <<EOF
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
DOSYPP.EXE --screenshot SHOT.TXT --null-display --door 3 --run-secs 25 > RUN.TXT
echo DONE >> RUN.TXT
exit
EOF

echo "scanning and hashing a share on DOS..."
SDL_VIDEODRIVER=dummy perl -e 'alarm 300; exec @ARGV' \
    dosbox-x -conf "$RUNDIR/dosbox-share.conf" -nogui \
    > "$RUNDIR/dosbox.log" 2>&1

[ -f "$RUNDIR/SHOT.TXT" ] || {
    echo "no screenshot. DOS output:" >&2
    cat "$RUNDIR/RUN.TXT" 2>/dev/null >&2
    exit 1; }

echo "--- the share door, on DOS ---"
sed -n '2,14p' "$RUNDIR/SHOT.TXT"
echo "------------------------------"

ok=1
if [ -f "$RUNDIR/DOSYPP.TTH" ]; then
    echo "hash cache written:"
    grep -v '^;' "$RUNDIR/DOSYPP.TTH" | sed 's/^/  /'
else
    echo "NO hash cache was written" >&2; ok=0
fi

if [ -f "$RUNDIR/FILES.XML" ]; then
    echo "files.xml written ($(wc -c < "$RUNDIR/FILES.XML") bytes)"
else
    echo "NO files.xml was written" >&2; ok=0
fi

grep -q "logged in as 'DOSShare'" "$RUNDIR/hub.log" 2>/dev/null && \
    echo "hub saw the login"

# The share size reported to the hub is the point: a hub with a minimum
# only reconsiders when the INF changes after hashing.
if grep -qE "BINF .* SS[1-9]" "$RUNDIR/hub.log" 2>/dev/null; then
    echo "share size reported to the hub:"
    grep -oE "SS[0-9]+ SF[0-9]+" "$RUNDIR/hub.log" | tail -1 | sed 's/^/  /'
else
    echo "the hub was never told a non-zero share size" >&2; ok=0
fi

[ "$ok" = 1 ] && echo "SHARE OK on DOS" || { echo "SHARE FAILED on DOS" >&2; exit 1; }
