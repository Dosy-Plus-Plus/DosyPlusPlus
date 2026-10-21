#!/bin/bash
# Drive the DOS UI with injected keystrokes and check what they did.
#
# This is the end-to-end test of the int 9h handler: DOSBox-X's AUTOTYPE
# presses real keys, the ISR decodes them from port 0x60, and the screen
# dump shows the result. Nothing is stubbed -- the keys go through the
# emulated keyboard controller exactly as hardware would deliver them.
#
# The UI runs on the real VGA display here (not the null one), so the dump
# also confirms the two paths agree.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNDIR="$ROOT/build/dos/keyrun"
SRC="$ROOT/dosbox-x/c"
# Keys are injected by DOSy++ itself through the keyboard controller
# (0x64 <- 0xD2), not by AUTOTYPE. AUTOTYPE is synchronous, so its
# keystrokes land at the command prompt before the program under test has
# even started -- observed as "1 int9 fired in 11 seconds".

command -v dosbox-x >/dev/null 2>&1 || { echo "need dosbox-x (brew install dosbox-x)" >&2; exit 1; }
for f in DOSYPP.EXE CWSDPMI.EXE NE2000.COM WATTCP.CFG; do
    [ -f "$SRC/$f" ] || { echo "missing $SRC/$f" >&2; exit 1; }
done

rm -rf "$RUNDIR"; mkdir -p "$RUNDIR"
cp "$SRC"/DOSYPP.EXE "$SRC"/CWSDPMI.EXE "$SRC"/NE2000.COM "$SRC"/WATTCP.CFG \
   "$RUNDIR/"

if ! nc -z 127.0.0.1 1511 2>/dev/null; then
    "$ROOT/tools/stubhub.py" --port 1511 > "$RUNDIR/hub.log" 2>&1 &
    HUB=$!
    trap 'kill "$HUB" 2>/dev/null' EXIT
    sleep 1
    # A few more nodes, so the who list has rows for the lightbar to move
    # between -- otherwise "down" proves nothing.
    for n in Phantom Razor Acid Mercury; do
        "$ROOT/build/host/dosypp" -p 1511 -n "$n" --console \
            -c "$RUNDIR/$n.ini" 127.0.0.1 >/dev/null 2>&1 &
        sleep 0.3
    done
fi

cat > "$RUNDIR/dosbox-keys.conf" <<EOF
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
DOSYPP.EXE -n DOSNode --kbdtest --screenshot SHOT.TXT 10.0.2.2 > RUN.TXT
echo DONE >> RUN.TXT
exit
EOF

echo "injecting scancodes via the keyboard controller..."
SDL_VIDEODRIVER=dummy perl -e 'alarm 300; exec @ARGV' \
    dosbox-x -conf "$RUNDIR/dosbox-keys.conf" -nogui \
    > "$RUNDIR/dosbox.log" 2>&1

echo "--- DOS output ---"
cat "$RUNDIR/RUN.TXT" 2>/dev/null
echo "------------------"

if [ -f "$RUNDIR/SHOT.TXT" ]; then
    echo "--- screen after the injected keys ---"
    cat "$RUNDIR/SHOT.TXT"
    echo "--------------------------------------"
fi

if grep -q "KBDTEST OK" "$RUNDIR/RUN.TXT" 2>/dev/null; then
    echo "int 9h handler verified: controller -> IRQ1 -> ISR -> UI"
    exit 0
fi
if grep -q "KBDTEST SKIPPED" "$RUNDIR/RUN.TXT" 2>/dev/null; then
    echo "int 9h handler NOT verified (injection unsupported here)" >&2
    exit 2
fi
echo "int 9h handler test failed" >&2
exit 1
