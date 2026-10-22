#!/bin/bash
# The Sound Blaster, in a browser engine.
#
# v86 emulates an SB16: DSP, mixer and DMA. It does NOT emulate the OPL
# chip, so FM music -- AdLib, most DOS game soundtracks -- is silent there.
# What does work is digital audio over DMA, which is exactly what this
# client's mixer produces: four channels of a ProTracker module summed to
# signed 8-bit and handed to the card.
#
# So the question this answers is whether the browser demo has music, and
# the measurement is the same pair of counters the DOS rig uses: frames the
# card has taken from the ring, and frames the mixer has put in it. There is
# no speaker in Node and it does not matter -- the DMA controller's count
# register is the instrument.
set -uo pipefail

# mtools asks about a name clash, and asks on stdin: an mmd for a
# directory that exists, or an mcopy without -o, waits for an answer that
# no rig is going to give. Non-interactive runs get EOF and carry on;
# a person running this in a terminal waits for ever, with the question
# usually hidden by a 2>/dev/null. One line removes the whole class.
exec < /dev/null


ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN="$ROOT/build/v86sound"
. "$ROOT/test/host/hublib.sh"

command -v node >/dev/null 2>&1 || {
    echo "need node (brew install node)" >&2; exit 1; }
command -v mcopy >/dev/null 2>&1 || {
    echo "need mtools (brew install mtools)" >&2; exit 1; }
for f in web/vendor/libv86.mjs web/images/boot.img web/images/dosypp.img; do
    [ -f "$ROOT/$f" ] || { echo "missing $f -- run: make images" >&2; exit 1; }
done

RELAY_PORT=$(free_port)
HUB_PORT=$(free_port)

export MTOOLS_SKIP_CHECK=1
rm -rf "$RUN"; mkdir -p "$RUN"

ok=1
# Collected as well as printed: the sweep shows only the tail of a log,
# and these lines are often a screen or two above it. The list is
# reprinted immediately before the verdict, so a red run in CI says which
# check failed instead of only that something did.
FAILURES=""
fail() {
    echo "FAIL: $*" >&2
    FAILURES="$FAILURES
  - $*"
    ok=0
}

cleanup() {
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    [ -n "${RELAY_PID:-}" ] && kill "$RELAY_PID" 2>/dev/null
}
trap cleanup EXIT

# The boot floppy runs AUTOSND.BAT instead of the interactive GO.BAT.
cp "$ROOT/web/images/boot.img" "$RUN/bootsnd.img"
cat > "$RUN/FDAUTO.BAT" <<'EOF'
@echo off
set DOSDIR=A:\FREEDOS
path A:\BIN;C:\
c:
call C:\AUTOSND.BAT
EOF
mcopy -i "$RUN/bootsnd.img" -o "$RUN/FDAUTO.BAT" ::FDAUTO.BAT

mdir -i "$ROOT/web/images/dosypp.img" ::AUTOSND.BAT >/dev/null 2>&1 || {
    echo "dosypp.img has no AUTOSND.BAT -- run: make images" >&2; exit 1; }

start_stub_hub "$HUB_PORT" "$RUN/hub.log"
"$ROOT/tools/wisprelay.py" --port "$RELAY_PORT" \
    --map "10.77.0.1=127.0.0.1:$HUB_PORT" > "$RUN/relay.log" 2>&1 &
RELAY_PID=$!
for _ in $(seq 40); do
    grep -q "wisprelay: http" "$RUN/relay.log" 2>/dev/null && break
    sleep 0.25
done

echo "booting with sound..."
node "$ROOT/test/v86/boot.mjs" \
    "wisp://localhost:$RELAY_PORT/" \
    "$RUN/bootsnd.img" \
    "$ROOT/web/images/dosypp.img" \
    "${1:-240}" \
    "AUTOSND-DONE" \
    "THIS-NEVER-APPEARS" > "$RUN/guest.log" 2>&1
rc=$?

echo
echo "--- what the guest reported ---"
grep -a "MUSIC:" "$RUN/guest.log" | tail -6
grep -a "no music" "$RUN/guest.log" | tail -2
echo "-------------------------------"

[ $rc -eq 0 ] || fail "the guest did not finish its run (harness exit $rc)"

grep -qa "no music:" "$RUN/guest.log" &&
    fail "the client could not open the card: $(grep -ao 'no music:.*' \
"$RUN/guest.log" | head -1)"

grep -qa "Sound Blaster at 220h" "$RUN/guest.log" ||
    fail "no Sound Blaster was found at 220h -- is BLASTER set in \
AUTOSND.BAT, and does this v86 build have sb16?"

# The numbers. `played` is what the card took from the ring, `fed` is what
# the mixer put in: the first proves v86's DMA is running, the second that
# the client kept up with it.
PLAYED=$(grep -a "MUSIC:" "$RUN/guest.log" |
         sed -n 's/.*played=\([0-9]*\).*/\1/p')
FED=$(grep -a "MUSIC:" "$RUN/guest.log" |
      sed -n 's/.*fed=\([0-9]*\).*/\1/p')

LAST_PLAYED=$(echo "$PLAYED" | tail -1)
LAST_FED=$(echo "$FED" | tail -1)
REPORTS=$(echo "$PLAYED" | grep -c .)

echo "reports: $REPORTS, played $LAST_PLAYED frames, fed $LAST_FED"

# Which method: the counter, or the clock because v86 does not implement
# the counter. Both are legitimate; they are not the same claim, so the
# output says which one this run used.
if grep -qa "timed: no DMA count" "$RUN/guest.log"; then
    echo "method: the clock (v86 does not expose a DMA count)"
    TIMED=1
else
    echo "method: the DMA controller's own count register"
    TIMED=0
fi

# The rate the mixer actually produced at, which is the claim that matters
# in timed mode: an estimate of consumption proves nothing on its own, but
# audio appearing at the sampling rate does.
RATE=$(echo "$FED" | python3 -c '
import sys
n = [int(x) for x in sys.stdin.read().split()]
d = [b - a for a, b in zip(n, n[1:])]
tail = d[-10:] if len(d) >= 10 else d
print(sum(tail) // len(tail) if tail else 0)
')
# The rate the card was opened at, as the client reports it, rather than a
# number repeated here: the image's DOSYPP.INI owns it, and a rig that
# hard-codes 11025 passes a 22050 image while measuring nothing.
WANT_RATE=$(grep -oa "[0-9]\+ Hz" "$RUN/guest.log" | head -1 |
            grep -o "[0-9]\+")
WANT_RATE=${WANT_RATE:-11025}
echo "the mixer produced about $RATE frames a second ($WANT_RATE wanted)"
LO=$((WANT_RATE * 7 / 10))
HI=$((WANT_RATE * 13 / 10))
if [ "${RATE:-0}" -lt "$LO" ] || [ "${RATE:-0}" -gt "$HI" ]; then
    fail "the mixer produced $RATE frames a second, not about $WANT_RATE
       -- at that rate the music stutters, which is what an underfed
       ring sounds like"
fi

if [ "${REPORTS:-0}" -lt 3 ]; then
    fail "only $REPORTS reports -- the run was too short to tell"
elif [ "${LAST_PLAYED:-0}" -lt "$WANT_RATE" ]; then
    fail "the card took only $LAST_PLAYED frames, under a second's worth: \
v86's DMA is not reading the ring"
elif [ "${LAST_FED:-0}" -lt "$WANT_RATE" ]; then
    fail "the mixer fed only $LAST_FED frames: the music is not being mixed"
else
    PCT=$((LAST_FED * 100 / LAST_PLAYED))
    echo "the mixer supplied $PCT% of what the card took"
    [ "$PCT" -ge 70 ] ||
        fail "the mixer supplied $PCT% of what the card took -- it is \
starving inside v86, which is faster than a 486 and should not"
fi

echo
if [ "$ok" = 1 ]; then
    echo "V86 SOUND OK: the emulated Sound Blaster was found and the mixer"
    echo "fed its ring at $WANT_RATE frames a second, so the browser demo"
    echo "has music."
    echo
    echo "What this does NOT prove: that a speaker makes a noise. There is no"
    echo "audio sink in Node, and v86 has no OPL either -- FM music would be"
    echo "silent in a browser whatever this says. What it does prove is that"
    echo "the card is found, the DMA ring is programmed, and PCM goes into"
    echo "it at $WANT_RATE frames a second."
    exit 0
fi
echo "--- what failed ---$FAILURES" >&2
echo "V86 SOUND FAILED"
exit 1
