#!/bin/bash
# Run the BBS interface on real DOS and bring back what it drew.
#
# Uses --screenshot, which fills the cell buffer exactly as normal but never
# touches video memory, so the result can be read on the host. That checks
# the layout, the CP437 output and the whole program running under DJGPP --
# though not the VGA writes themselves, which need eyes on a screen.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNDIR="$ROOT/build/dos/uirun"
SRC="$ROOT/dosbox-x/c"
DOOR="${1:-0}"
MODE="${2:-shot}"       # shot = screenshot via null display
                        # vga  = draw for real, then read 0xB8000 back
ROWS="${3:-25}"         # 25, or 50 for the 8x8 font

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
fi

if [ "$MODE" = "vga" ]; then
    EXTRA="--vgacheck"
else
    EXTRA="--screenshot SHOT.TXT --null-display"
fi
EXTRA="$EXTRA --rows $ROWS"

cat > "$RUNDIR/dosbox-ui.conf" <<EOF
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
DOSYPP.EXE -n DOSNode $EXTRA --door $DOOR 10.0.2.2 > RUN.TXT
echo DONE >> RUN.TXT
exit
EOF

echo "running the UI on DOS (door $DOOR, $ROWS rows)..."
SDL_VIDEODRIVER=dummy perl -e 'alarm 240; exec @ARGV' \
    dosbox-x -conf "$RUNDIR/dosbox-ui.conf" -nogui \
    > "$RUNDIR/dosbox.log" 2>&1

if [ "$MODE" = "vga" ]; then
    echo "--- DOS output ---"
    cat "$RUNDIR/RUN.TXT"
    grep -q "VGACHECK OK" "$RUNDIR/RUN.TXT" || {
        echo "VGA check failed" >&2; exit 1; }

    # At 50 rows this is the only check that the mode switch really
    # happened: --null-display would report fifty rows whatever the BIOS
    # did, because nothing asked the hardware. The readback compares 4,000
    # cells of actual video memory.
    if ! grep -q "(80x$ROWS)" "$RUNDIR/RUN.TXT"; then
        echo "FAILED: the display is not 80x$ROWS" >&2
        grep -a VGACHECK "$RUNDIR/RUN.TXT" >&2
        exit 1
    fi
    echo "VGA renderer verified against video memory at 80x$ROWS"
    exit 0
fi

if [ ! -f "$RUNDIR/SHOT.TXT" ]; then
    echo "no screenshot produced. DOS output:" >&2
    cat "$RUNDIR/RUN.TXT" 2>/dev/null >&2
    exit 1
fi

echo "--- drawn by DOSy++ running on DOS ---"
cat "$RUNDIR/SHOT.TXT"
echo "--------------------------------------"
grep -q DONE "$RUNDIR/RUN.TXT" || { echo "did not finish" >&2; exit 1; }

# The screenshot is the cell buffer, so its line count is the height the
# program laid out for -- which is the thing worth checking about a mode
# switch: asking for 50 and getting 25 is a silent failure otherwise.
LINES=$(grep -c "" "$RUNDIR/SHOT.TXT")
echo "the DOS client laid out $LINES rows (asked for $ROWS)"
if [ "$LINES" != "$ROWS" ]; then
    echo "FAILED: asked for $ROWS rows and got $LINES" >&2
    exit 1
fi

# And the bottom row is the hotkey bar: at 50 rows an off-by-one in the
# layout puts it on the status line or off the screen entirely.
if ! sed -n "${ROWS}p" "$RUNDIR/SHOT.TXT" | grep -q "1-MSGS"; then
    echo "FAILED: row $ROWS is not the hotkey bar" >&2
    sed -n "${ROWS}p" "$RUNDIR/SHOT.TXT" >&2
    exit 1
fi

echo "UI OK on DOS at $ROWS rows"
