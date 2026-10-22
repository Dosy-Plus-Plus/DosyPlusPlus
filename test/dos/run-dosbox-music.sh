#!/bin/bash
# The Sound Blaster path, on DOS. The one part of the music nothing else
# touches: BLASTER parsing, the DSP reset handshake, a DMA ring in
# conventional memory, and auto-init playback.
#
# There is no way to listen to a scripted DOSBox-X run, so this measures
# instead of asserting. The client prints the frame count once a second
# with --music-report, and that count is read back from the DMA
# controller's own current-count register. If it advances, the card is
# reading the ring -- which is the thing that goes wrong when the mode byte
# or the page register is set wrong, and the thing a "did sndout_open()
# return 0" check would miss entirely.
#
# Two runs, because the two interesting failures look the same from inside
# the program:
#
#   with BLASTER set    -- the card is found, and the count moves
#   with BLASTER unset  -- the client says so and keeps running
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNDIR="$ROOT/build/dos/musicrun"
SRC="$ROOT/dosbox-x/c"
MOD="$ROOT/art/DOSYPP.MOD"
RATE=${MUSIC_RATE:-11025}
HUB_PORT=${MUSIC_HUB_PORT:-24998}

command -v dosbox-x >/dev/null 2>&1 || { echo "need dosbox-x (brew install dosbox-x)" >&2; exit 1; }
[ -f "$MOD" ] || { echo "run: make modules" >&2; exit 1; }
for f in DOSYPP.EXE CWSDPMI.EXE NE2000.COM; do
    [ -f "$SRC/$f" ] || { echo "missing $SRC/$f (run: make dosfiles)" >&2
                          exit 1; }
done

rm -rf "$RUNDIR"
mkdir -p "$RUNDIR"
cp "$SRC"/DOSYPP.EXE "$SRC"/CWSDPMI.EXE "$SRC"/NE2000.COM "$RUNDIR/"
cp "$MOD" "$RUNDIR/DOSYPP.MOD"

# A stub hub is needed even though the music has nothing to do with the
# network: the client is a hub client and will not reach its main loop
# without one. 10.0.2.2 is how slirp presents the host to the guest.
cat > "$RUNDIR/DOSYPP.INI" <<EOF
hub = 10.0.2.2
port = $HUB_PORT
nick = DOSPlayer
sound = no
active = no
music = yes
music.file = DOSYPP.MOD
music.rate = $RATE
EOF

conf() {
    # $1 = config file to write, $2 = extra autoexec lines,
    # $3/$4/$5 = core/cputype/cycles (default: as fast as the host allows)
    CORE=${3:-dynamic}
    CPUTYPE=${4:-486}
    CYCLES=${5:-max}
    cat > "$1" <<EOF
[sdl]
output = surface
[dosbox]
machine = svga_s3
memsize = 16
[cpu]
core = $CORE
cputype = $CPUTYPE
cycles = $CYCLES
[ne2000]
ne2000 = true
nicbase = 300
nicirq = 10
backend = slirp
[sblaster]
sbtype = sb16
sbbase = 220
irq = 5
dma = 1
[dos]
ver = 7.1
lfn = false
[autoexec]
mount c "$RUNDIR"
c:
NE2000 0x60 10 0x300 > NUL
$2
exit
EOF
}

HUB=
cleanup() { [ -n "$HUB" ] && kill "$HUB" 2>/dev/null; }
trap cleanup EXIT

"$ROOT/tools/stubhub.py" --port "$HUB_PORT" > "$RUNDIR/hub.log" 2>&1 &
HUB=$!
for _ in $(seq 20); do
    nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null && break
    sleep 0.25
done
nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null || {
    echo "the stub hub did not come up on $HUB_PORT" >&2
    cat "$RUNDIR/hub.log" >&2; exit 1; }

# ---- run one: the card is there
conf "$RUNDIR/dosbox-music.conf" \
'DOSYPP.EXE --screenshot SHOT.TXT --null-display --door 7 --music-report --run-secs 12 > RUN.TXT
echo DONE >> RUN.TXT'

echo "playing on DOS with a Sound Blaster at 220h..."
SDL_VIDEODRIVER=dummy perl -e 'alarm 180; exec @ARGV' \
    dosbox-x -conf "$RUNDIR/dosbox-music.conf" -nogui \
    > "$RUNDIR/dosbox.log" 2>&1

# ---- run two: no BLASTER in the environment
conf "$RUNDIR/dosbox-nosb.conf" \
'set BLASTER=
DOSYPP.EXE --screenshot NOSB.TXT --null-display --door 1 --run-secs 6 > NOSB.TXT
echo DONE >> NOSB.TXT'

echo "and again with BLASTER unset..."
SDL_VIDEODRIVER=dummy perl -e 'alarm 120; exec @ARGV' \
    dosbox-x -conf "$RUNDIR/dosbox-nosb.conf" -nogui \
    > "$RUNDIR/dosbox2.log" 2>&1

# ---- run three: hashing a share at 486 speed, with the music playing
#
# The case the benchmark says is hard. A pass of the main loop that hashes
# HASHQ_BUDGET takes 126ms on a 486 at 521 KB/s, which is nearly three
# slices of audio: a tick that mixed one slice starved the ring and the
# music stuttered for as long as the share was being hashed. This is the
# measurement that it no longer does, on a CPU of roughly the right speed
# rather than a host that is a hundred times too fast.
mkdir -p "$RUNDIR/DL"
python3 - "$RUNDIR/DL" <<'PY'
import os, random, sys
root = sys.argv[1]
random.seed(1994)
# About 8MB, which is sixteen seconds of hashing at 500 KB/s. The run is
# shorter than that on purpose: the screenshot has to land while hashing is
# still going, or the frame counts would be from an idle loop.
for i in range(16):
    with open(os.path.join(root, "BIGFILE%d.ZIP" % i), "wb") as f:
        f.write(bytes(random.getrandbits(8) for _ in range(512 * 1024)))
print("made a 5MB share to hash")
PY

cat > "$RUNDIR/HASH.INI" <<EOF
hub = 10.0.2.2
port = $HUB_PORT
nick = DOSHasher
sound = no
active = no
share = C:\\DL
music = yes
music.file = DOSYPP.MOD
music.rate = $RATE
EOF

conf "$RUNDIR/dosbox-hash.conf" \
'DOSYPP.EXE -c HASH.INI --screenshot HSHOT.TXT --null-display --door 5 --music-report --loop-trace 400 --run-secs 14 > HASH.TXT
echo DONE >> HASH.TXT' \
    normal 486_prefetch 25000

echo "hashing a 5MB share with the music on, at 486DX2-66 speed..."
SDL_VIDEODRIVER=dummy perl -e 'alarm 420; exec @ARGV' \
    dosbox-x -conf "$RUNDIR/dosbox-hash.conf" -nogui \
    > "$RUNDIR/dosbox3.log" 2>&1

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

echo
echo "--- what the client reported ---"
grep -a "MUSIC:" "$RUNDIR/RUN.TXT" 2>/dev/null | head -14
echo "--------------------------------"

[ -f "$RUNDIR/RUN.TXT" ] || { echo "nothing came back from DOS" >&2; exit 1; }

grep -qa "MUSIC:" "$RUNDIR/RUN.TXT" ||
    fail "the client never reported on the music"
grep -qa "Sound Blaster at 220h" "$RUNDIR/RUN.TXT" ||
    fail "the card at 220h was not found (BLASTER or the DSP reset)"
grep -qai "no music" "$RUNDIR/RUN.TXT" &&
    fail "the client gave up on the music: $(grep -ai 'no music' "$RUNDIR/RUN.TXT" | head -1)"

# The measurement: the frame count has to move, and by roughly a second's
# worth per second. "Roughly" is generous on purpose -- DOSBox-X at
# cycles=max does not run in step with the wall clock -- but zero, or a
# count that never changes, is the failure this rig exists to catch.
FRAMES=$(grep -a "MUSIC:" "$RUNDIR/RUN.TXT" |
         sed -n 's/.*played=\([0-9]*\).*/\1/p')
FED=$(grep -a "MUSIC:" "$RUNDIR/RUN.TXT" |
      sed -n 's/.*fed=\([0-9]*\).*/\1/p')
FIRST=$(echo "$FRAMES" | head -1)
LAST=$(echo "$FRAMES" | tail -1)
COUNT=$(echo "$FRAMES" | wc -l | tr -d ' ')

echo "frame count went $FIRST -> $LAST over $COUNT reports"

if [ -z "$LAST" ]; then
    fail "no frame counts were printed"
elif [ "$LAST" -le 0 ]; then
    fail "the card played nothing at all"
elif [ "$LAST" -le "${FIRST:-0}" ]; then
    fail "the frame count never advanced ($FIRST -> $LAST): the DMA ring \
is set up but the card is not reading it"
else
    # It has to keep going, not just start. Playback stopping after two
    # blocks is a real bug this rig found: the DSP latches an end-of-block
    # interrupt and nothing acknowledged it, so the count froze at exactly
    # two rings (32768) while the client happily went on mixing. So:
    # more than four rings played, and every one of the last few seconds
    # moving.
    MIN_TOTAL=$(( 4 * 16384 ))
    if [ "$LAST" -lt "$MIN_TOTAL" ]; then
        fail "only $LAST frames played, under $MIN_TOTAL (four rings) -- \
playback stalls after a block or two"
    fi

    STALL=$(echo "$FRAMES" | python3 -c '
import sys
# The deltas between consecutive reports, which are a second apart. A zero
# in there is a stall even if the total looks healthy.
n = [int(x) for x in sys.stdin.read().split()]
d = [b - a for a, b in zip(n, n[1:])]
tail = d[-5:] if len(d) >= 5 else d
print(min(tail) if tail else 0)
')
    echo "slowest second of the last five: $STALL frames"
    if [ "${STALL:-0}" -lt $(( RATE / 4 )) ]; then
        fail "a second passed with only $STALL frames played: the ring \
stops being read"
    fi

    # And the rate it plays at, which is the other thing that can be wrong
    # while the count still moves: a time constant off by a factor would
    # play the tune at the wrong speed and nothing else would notice.
    SPAN=$(( COUNT - 1 ))
    if [ "$SPAN" -gt 3 ]; then
        PER=$(( (LAST - FIRST) / SPAN ))
        echo "about $PER frames a second, configured for $RATE"
        LO=$(( RATE * 3 / 4 ))
        HI=$(( RATE * 5 / 4 ))
        if [ "$PER" -lt "$LO" ] || [ "$PER" -gt "$HI" ]; then
            fail "playing at about $PER Hz, not $RATE: the DSP time \
constant is wrong"
        fi
    fi
fi

# The info door, drawn on the DOS machine, has to agree with it.
if [ -f "$RUNDIR/SHOT.TXT" ]; then
    grep -qa "Sound Blaster" "$RUNDIR/SHOT.TXT" ||
        fail "the info door does not name the card"
    grep -qai "DOSy++ attic" "$RUNDIR/SHOT.TXT" ||
        fail "the info door does not show the module title"
else
    fail "no screenshot came back from DOS"
fi

# ---- and without a card: a message, not a dead client
if [ -f "$RUNDIR/NOSB.TXT" ]; then
    grep -qa "DONE" "$RUNDIR/NOSB.TXT" ||
        fail "the client did not finish its run with BLASTER unset"
    grep -qai "BLASTER is not set\|no music" "$RUNDIR/NOSB.TXT" ||
        fail "a missing card was not reported"
else
    fail "the no-card run produced nothing"
fi

# ---- run three's numbers
echo
echo "--- hashing a share with the music on (486 speed) ---"
grep -a "MUSIC:" "$RUNDIR/HASH.TXT" 2>/dev/null | tail -6
echo "-----------------------------------------------------"

if [ ! -f "$RUNDIR/HASH.TXT" ]; then
    fail "the hashing run produced nothing"
else
    # It has to have been hashing during the window, or the measurement is
    # of an idle loop: the cache file and a share size at the hub say so.
    grep -qa "HASHING" "$RUNDIR/HSHOT.TXT" 2>/dev/null ||
        fail "the share door was not hashing when the screen was taken"

    # fed, not played: the card empties the ring at 11025 Hz whether or
    # not the mixer refilled it, so `played` cannot see an underrun. This
    # is the number that caught the one-slice tick.
    HFRAMES=$(grep -a "MUSIC:" "$RUNDIR/HASH.TXT" |
              sed -n 's/.*fed=\([0-9]*\).*/\1/p')
    HPLAYED=$(grep -a "MUSIC:" "$RUNDIR/HASH.TXT" |
              sed -n 's/.*played=\([0-9]*\).*/\1/p')

    # Sustained keep-up, over the window as a whole: how much audio the
    # mixer produced against how much the card took. A single bad second
    # under an emulator cannot be told apart from the emulator being
    # descheduled by the host, so it is reported rather than failed; a
    # mixer that cannot keep up at all shows here and cannot hide. The
    # one-slice tick this replaced managed 25%.
    read -r HPCT HSLOW HWORST <<EOF
$(paste <(echo "$HPLAYED") <(echo "$HFRAMES") | python3 -c '
import sys
rows = [tuple(int(x) for x in line.split()) for line in sys.stdin if line.strip()]
# The first two reports cover opening the card and logging in; the ring is
# primed with silence there and there is nothing to compare yet.
rows = rows[2:]
if len(rows) < 3:
    print("0 0 0")
    raise SystemExit
played = rows[-1][0] - rows[0][0]
fed    = rows[-1][1] - rows[0][1]
deltas = [b[1] - a[1] for a, b in zip(rows, rows[1:])]
pct = (fed * 100 // played) if played else 0
print(pct, min(deltas) if deltas else 0, max(deltas) if deltas else 0)
')
EOF

    echo "mixed $HPCT% of what the card took while hashing"
    echo "  per second: slowest $HSLOW, fastest $HWORST, wanted $RATE"

    if [ "${HPCT:-0}" -lt 85 ]; then
        fail "the music starves while hashing: the mixer produced $HPCT% \
of what the card consumed, so the ring was replaying stale audio"
    fi

    # How far behind the mixer ever got, in total. This is the sharp one:
    # the writer deliberately stays about a slice ahead, so a few hundred
    # frames is normal and a ring's worth is a dropout that never heals --
    # which is what the two-second splash used to cause by not servicing
    # the audio at all. It read 16,940 then and about 557 now.
    LAG=$(paste <(echo "$HPLAYED") <(echo "$HFRAMES") | python3 -c '
import sys
rows = [tuple(int(x) for x in l.split()) for l in sys.stdin if l.strip()]
print(max((p - f) for p, f in rows) if rows else 0)
')
    echo "furthest the mixer fell behind: $LAG frames (a ring is 16384)"
    if [ "${LAG:-0}" -gt 4096 ]; then
        fail "the mixer fell $LAG frames behind the card -- a quarter of a \
ring or more means something in the loop is not servicing the audio"
    fi

    # And the loop itself: nothing in a pass should block for the best part
    # of a second. The tracer reports any pass over 400ms; there should be
    # none, because the share tick's own budget keeps a pass near 165ms.
    OVER=$(grep -a "LOOP SUMMARY" "$RUNDIR/HASH.TXT" |
           sed -n 's/.*passes, \([0-9]*\) over.*/\1/p')
    echo "main-loop passes over 400ms: ${OVER:-?}"
    grep -a "LOOP SUMMARY" "$RUNDIR/HASH.TXT" >/dev/null ||
        fail "the loop tracer reported nothing"
    [ "${OVER:-1}" = 0 ] ||
        fail "$OVER main-loop passes took over 400ms: $(grep -a '^LOOP: pass' "$RUNDIR/HASH.TXT" | head -2)"

    echo "--- where a pass goes ---"
    grep -a "^LOOP: [a-z]" "$RUNDIR/HASH.TXT" | sed 's/^/  /'

fi

echo
if [ "$ok" = 1 ]; then
    echo "MUSIC OK on DOS: the card was found, the ring advanced $LAST \
frames at about $PER Hz without stalling, and a missing card is a message."
    exit 0
fi

echo "--- DOS output ---" >&2
cat "$RUNDIR/RUN.TXT" 2>/dev/null >&2
echo "--- no-card run ---" >&2
cat "$RUNDIR/NOSB.TXT" 2>/dev/null >&2
echo "--- what failed ---$FAILURES" >&2
echo "MUSIC FAILED on DOS" >&2
exit 1
