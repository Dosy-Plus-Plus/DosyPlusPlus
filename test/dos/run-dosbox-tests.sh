#!/bin/bash
# Run the hash tests on actual DOS, under DOSBox-X, and report on the host.
#
# This is the other half of the Phase 1 gate. Passing on arm64/clang says
# nothing about i386 + gnu89 + DJGPP's GCC: Tiger is 64-bit arithmetic that
# a 32-bit target has to synthesise, and both Tiger and TTH are byte-order
# sensitive. If these two runs ever disagree, trust neither.
#
# Needs: make dos-test (which needs the DJGPP container), and dosbox-x.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MODE="${1:-test}"
RUNDIR="$ROOT/build/dos/testrun"
# An array, not a string: this repo's path has a space in it and word
# splitting would turn one file into two missing ones.
EXTRA_FILES=()
PROG_ARGS=""
if [ "$MODE" = "--tokens" ]; then
    EXE="$ROOT/build/dos/TOKTEST.EXE"
    PROG="TOKTEST.EXE"
    # Correctness, not speed: cycles=max.
    CPU_CORE="dynamic"; CPU_TYPE="486"; CYCLES="max"
    LABEL="protocol text tests (64-bit sizes where long is 32 bits)"
elif [ "$MODE" = "--musbench" ]; then
    EXE="$ROOT/build/dos/MUSBENCH.EXE"
    PROG="MUSBENCH.EXE"
    # The same cycle-limited profile as the other two benchmarks. The point
    # of this one is the ratio between mixing and hashing, and that ratio is
    # only meaningful on a CPU of roughly the target's speed.
    CPU_CORE="normal"; CPU_TYPE="486_prefetch"; CYCLES="25000"
    LABEL="music/hash benchmark (approx 486DX2-66)"
    EXTRA_FILES=("$ROOT/art/DOSYPP.MOD" "$ROOT/art/FXTEST.MOD")
    # Both modules: the shipped tune uses no per-tick pitch effects and the
    # fixture uses all of them, so the two bracket what the mixer costs.
    PROG_ARGS="DOSYPP.MOD FXTEST.MOD"
    MODE="--bench"
elif [ "$MODE" = "--searchbench" ]; then
    EXE="$ROOT/build/dos/SRCHBENC.EXE"
    PROG="SRCHBENC.EXE"
    # Same cycle-limited profile as the hash benchmark, for the same
    # reason: at cycles=max the answer flatters us and decides nothing.
    CPU_CORE="normal"; CPU_TYPE="486_prefetch"; CYCLES="25000"
    LABEL="search benchmark (approx 486DX2-66)"
    MODE="--bench"
elif [ "$MODE" = "--bench" ]; then
    EXE="$ROOT/build/dos/HASHBENC.EXE"
    PROG="HASHBENC.EXE"
    # A benchmark is only meaningful on a cycle-limited CPU: cycles=max is
    # far faster than any real 486 and would flatter us badly.
    CPU_CORE="normal"; CPU_TYPE="486_prefetch"; CYCLES="25000"
    LABEL="benchmark (approx 486DX2-66)"
else
    EXE="$ROOT/build/dos/HASHTEST.EXE"
    PROG="HASHTEST.EXE"
    CPU_CORE="dynamic"; CPU_TYPE="486"; CYCLES="max"
    LABEL="hash tests"
fi
CWSDPMI="$ROOT/dosbox-x/c/CWSDPMI.EXE"

command -v dosbox-x >/dev/null 2>&1 || {
    echo "dosbox-x not found (brew install dosbox-x)" >&2; exit 1; }
[ -f "$EXE" ] || {
    echo "$EXE missing -- run: make dos-test (or tools/dosmake.sh make dos-test)" >&2
    exit 1; }
[ -f "$CWSDPMI" ] || {
    echo "CWSDPMI.EXE missing -- run: make dosfiles" >&2; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$RUNDIR"
cp "$EXE" "$RUNDIR/$PROG"
cp "$CWSDPMI" "$RUNDIR/CWSDPMI.EXE"
for f in ${EXTRA_FILES+"${EXTRA_FILES[@]}"}; do
    [ -f "$f" ] || { echo "$f missing -- run: make modules" >&2; exit 1; }
    cp "$f" "$RUNDIR/"
done

# No networking needed for a pure compute test, so no packet driver and no
# ne2000 -- which keeps this runnable before a driver has been sourced.
cat > "$RUNDIR/dosbox-test.conf" <<EOF
[sdl]
output = surface
[dosbox]
machine = svga_s3
memsize = 16
[cpu]
core = $CPU_CORE
cputype = $CPU_TYPE
cycles = $CYCLES
[dos]
ver = 7.1
lfn = false
[ne2000]
ne2000 = false
[autoexec]
mount c "$RUNDIR"
c:
$PROG $PROG_ARGS > RESULT.TXT
echo DONE >> RESULT.TXT
exit
EOF

echo "running $PROG under DOSBox-X -- $LABEL"
SDL_VIDEODRIVER=dummy perl -e 'alarm 600; exec @ARGV' \
    dosbox-x -conf "$RUNDIR/dosbox-test.conf" -nogui \
    > "$RUNDIR/dosbox.log" 2>&1
rc=$?

RESULT="$RUNDIR/RESULT.TXT"
if [ ! -f "$RESULT" ]; then
    echo "no RESULT.TXT -- DOSBox-X exited $rc. Tail of its log:" >&2
    tail -20 "$RUNDIR/dosbox.log" >&2
    exit 1
fi

echo "--- output from DOS ---"
cat "$RESULT"
echo "-----------------------"

if ! grep -q "^DONE" "$RESULT"; then
    echo "FAILED: the test did not run to completion (crash or hang?)" >&2
    exit 1
fi

if [ "$MODE" = "--bench" ]; then
    exit 0
fi

# Must match the host's pass line, not merely avoid the word FAIL: a binary
# that printed nothing would otherwise look like a pass.
if grep -qE "^ok: [0-9]+ checks, 0 failed" "$RESULT"; then
    echo "OK on DOS: $LABEL -- $(grep -oE '^ok: [0-9]+ checks' "$RESULT")"
    exit 0
fi

echo "FAILED: DOS run did not report a clean pass." >&2
exit 1
