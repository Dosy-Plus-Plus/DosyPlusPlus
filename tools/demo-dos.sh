#!/bin/bash
# The same thing, but running on actual DOS in a DOSBox-X window.
#
# This is the real article: DJGPP binary, CWSDPMI, a packet driver, Watt-32,
# and the int 9h keyboard handler, drawing into real VGA text memory.
#
#   ./tools/demo-dos.sh
#   ./tools/demo-dos.sh --stop     stop a previous run and exit
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "${1:-}" = "--stop" ]; then
    exec "$ROOT/tools/demo-stop.sh" 1511
fi

RUNDIR="$ROOT/build/dos/demo"
SRC="$ROOT/dosbox-x/c"
PORT=1511

cleanup() {
    rm -f "$RUNDIR/running" 2>/dev/null
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    for p in ${NODE_PIDS:-}; do kill "$p" 2>/dev/null; done
    # The feeding subshells are not in NODE_PIDS ($! for a pipeline names
    # the last command), so kill our children directly, then sweep by port.
    pkill -P $$ 2>/dev/null
    pkill -f "dosypp -p $PORT" 2>/dev/null
    pkill -f "stubhub.py --port $PORT" 2>/dev/null
}
trap cleanup EXIT INT TERM

command -v dosbox-x >/dev/null 2>&1 || {
    echo "need dosbox-x (brew install dosbox-x)" >&2; exit 1; }
for f in DOSYPP.EXE CWSDPMI.EXE NE2000.COM WATTCP.CFG; do
    [ -f "$SRC/$f" ] || {
        echo "missing $SRC/$f" >&2
        echo "run: make dos-docker && make dosfiles && make pktdrv" >&2
        echo "then: cp build/dos/DOSYPP.EXE dosbox-x/c/" >&2
        exit 1; }
done

rm -rf "$RUNDIR"; mkdir -p "$RUNDIR"
cp "$SRC"/DOSYPP.EXE "$SRC"/CWSDPMI.EXE "$SRC"/NE2000.COM "$SRC"/WATTCP.CFG \
   "$RUNDIR/"

# Clear our own leftovers rather than refusing to start.
if pgrep -f "stubhub.py --port $PORT" >/dev/null 2>&1 ||
   pgrep -f "dosypp -p $PORT" >/dev/null 2>&1; then
    echo "cleaning up a previous demo..."
    "$ROOT/tools/demo-stop.sh" "$PORT" || exit 1
fi

if nc -z 127.0.0.1 "$PORT" 2>/dev/null; then
    echo "port $PORT is held by something we did not start:" >&2
    lsof -nP -iTCP:"$PORT" -sTCP:LISTEN 2>/dev/null >&2
    exit 1
fi

echo "starting the hub and a few other nodes on the host..."
"$ROOT/tools/stubhub.py" --port "$PORT" --motd-ansi >"$RUNDIR/hub.log" 2>&1 &
HUB_PID=$!

# Do not launch DOSBox against a hub that never came up: the failure would
# then surface inside DOS as a bare "failed", which is the least useful
# place to discover it.
for _ in $(seq 20); do
    nc -z 127.0.0.1 "$PORT" 2>/dev/null && break
    sleep 0.25
done
if ! nc -z 127.0.0.1 "$PORT" 2>/dev/null; then
    echo "the hub did not start. Its log:" >&2
    cat "$RUNDIR/hub.log" >&2
    exit 1
fi
echo "hub is up on 127.0.0.1:$PORT"

touch "$RUNDIR/running"

NODE_PIDS=""
for n in Phantom Razor Acid; do
    ( while [ -f "$RUNDIR/running" ]; do echo "greetz from $n"; sleep 9; done ) | \
        "$ROOT/build/host/dosypp" -p "$PORT" -n "$n" --console \
            -c "$RUNDIR/$n.ini" 127.0.0.1 >/dev/null 2>&1 &
    NODE_PIDS="$NODE_PIDS $!"
    sleep 0.3
done

# slirp: the guest is 10.0.2.15 and the host is always 10.0.2.2.
cat > "$RUNDIR/dosbox-demo.conf" <<EOF
[sdl]
fullscreen = false
output = opengl
autolock = false
[dosbox]
title = DOSy++ on DOS
machine = svga_s3
memsize = 16
[render]
aspect = true
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
[speaker]
pcspeaker = true
[autoexec]
mount c "$RUNDIR"
c:
@echo off
cls
echo Loading packet driver...
NE2000 0x60 10 0x300
if errorlevel 1 goto nodrv
echo.
rem Probe with --batch first. If the network is not working, the reason
rem lands in PROBE.TXT where the host can read it -- rather than flashing
rem past inside a full-screen UI that then exits.
echo Checking the connection to the host...
DOSYPP.EXE -n DOSProbe --batch 10.0.2.2 > PROBE.TXT
if errorlevel 1 goto nonet
echo.
echo Starting DOSy++ -- quit with /quit or G, then type EXIT to close DOSBox.
echo.
DOSYPP.EXE -n DOSNode 10.0.2.2
goto done
:nodrv
echo.
echo The packet driver did not load. Check NE2000.COM is present and that
echo the ne2000 settings match (io 0x300, irq 10).
goto done
:nonet
echo.
echo Could not reach the hub on the host at 10.0.2.2 port $PORT.
echo The reason is in PROBE.TXT:
echo.
type PROBE.TXT
echo.
echo Leaving you at a DOS prompt. Type EXIT to close DOSBox.
:done
EOF

cat <<MSG

A DOSBox-X window is opening. Inside it:

  1 / 2 / 3 / 4   doors        TAB  cycle
  PGUP / PGDN     scroll       UP / DOWN  lightbar
  type + ENTER    post         /quit or G  hang up

The keyboard there goes through our own int 9h handler, and the screen is
real VGA text memory at 0xB8000.

Close the DOSBox-X window, or type EXIT at the DOS prompt, when done.
MSG

dosbox-x -conf "$RUNDIR/dosbox-demo.conf" 2>&1 | tee "$RUNDIR/dosbox.log"

echo
echo "logs from that run:"
echo "  hub     : $RUNDIR/hub.log"
echo "  dosbox  : $RUNDIR/dosbox.log"
if [ -f "$RUNDIR/PROBE.TXT" ]; then
    echo "  probe   : $RUNDIR/PROBE.TXT"
    echo
    echo "--- what the DOS side saw when it tried to connect ---"
    cat "$RUNDIR/PROBE.TXT"
fi
