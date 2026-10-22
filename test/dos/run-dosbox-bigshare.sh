#!/bin/bash
# A 5GB share on the target, advertised correctly.
#
# The share total in an INF is 64-bit and the code that printed it was 32:
# a 5GB share told the hub it had 1GB. The host rig (test-bigshare) proves
# the arithmetic; this proves it where `long` really is 32 bits, which is
# the condition the bug needed.
#
# Two things make a 5GB share testable inside an emulator:
#
#   - The files are sparse. A unique 512-byte header and then a hole:
#     5GB costs 180KB of disk and reads back as zeros at memory speed.
#   - The hashes are computed on the host and handed to the guest in a
#     DOSYPP.TTH. Hashing 5GB on DOS is three hours at 500 KB/s, and the
#     hasher is not what is under test -- it has its own rigs.
#
# The cache is keyed on the virtual path, the size and the modification
# time, and the two platforms record that time differently (epoch seconds
# against packed FAT fields). tools/mkhashcache.py translates it, and the
# translation was confirmed against this very target: a DOS run over one
# file touched to 1994-01-01 00:00 recorded 471924736, which is what the
# formula computes.
#
# If the translation is ever wrong the guest hashes everything instead, so
# this rig fails on a timeout rather than on a wrong answer -- and it says
# which it was.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLIENT="$ROOT/build/host/dosypp"
RUNDIR="$ROOT/build/dos/bigsharerun"
SRC="$ROOT/dosbox-x/c"
. "$ROOT/test/host/hublib.sh"

SIZE=${BIGSHARE_SIZE:-5G}
HUB_PORT=$(free_port)
HOST_AS=10.0.2.2        # how slirp presents the host to the guest

command -v dosbox-x >/dev/null 2>&1 || {
    echo "need dosbox-x (brew install dosbox-x)" >&2; exit 1; }
[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }
for f in DOSYPP.EXE CWSDPMI.EXE NE2000.COM; do
    [ -f "$SRC/$f" ] || { echo "missing $SRC/$f (run: make stage-dos)" >&2
                          exit 1; }
done

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
    [ -n "${HOST_PID:-}" ] && kill "$HOST_PID" 2>/dev/null
}
trap cleanup EXIT

rm -rf "$RUNDIR"; mkdir -p "$RUNDIR"
cp "$SRC"/DOSYPP.EXE "$SRC"/CWSDPMI.EXE "$SRC"/NE2000.COM "$RUNDIR/"

# An array, not a string: this repository's own path has a space in it, so
# an unquoted "$REAL" splits into two arguments and the generator gets a
# stray positional. That is the third time a space in the path has cost
# half an hour; see run-dosbox-tests.sh for the second.
REAL=()
[ -d "$ROOT/build/atmos" ] && REAL=(--real "$ROOT/build/atmos")

echo "building a $SIZE share$([ ${#REAL[@]} -gt 0 ] && \
      echo " with real software")..."
TOTAL=$("$ROOT/tools/mkbigshare.py" "$RUNDIR/DL" "$SIZE" ${REAL+"${REAL[@]}"} \
        --manifest "$RUNDIR/manifest.txt" | tail -1)
FILES=$(grep -c "" "$RUNDIR/manifest.txt")
echo "$FILES files, $TOTAL bytes, $(du -sh "$RUNDIR/DL" | cut -f1) on disk"

[ "$TOTAL" -gt 4294967295 ] || {
    echo "$TOTAL bytes is under 4GB, which proves nothing here" >&2; exit 1; }
TRUNCATED=$(python3 -c "print($TOTAL % (2**32))")

# Every file to the same timestamp: one constant to translate instead of
# forty-five, and 1994 is the right year for the share.
find "$RUNDIR/DL" -type f -exec touch -t 199401010000 {} +

# ---- the hashes, on the host, where this takes half a minute
echo
echo "hashing on the host (the guest is handed the result)..."
start_stub_hub "$HUB_PORT" "$RUNDIR/hub.log" --no-fake-share \
    --loopback-as "$HOST_AS"

cat > "$RUNDIR/host.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = HostHasher
active = no
sound = no
music = no
share = $RUNDIR/DL
EOF

( cd "$RUNDIR" && exec "$CLIENT" -c "$RUNDIR/host.ini" --null-display \
    --door 5 --run-secs 600 127.0.0.1 ) > "$RUNDIR/host.log" 2>&1 &
HOST_PID=$!

for _ in $(seq 300); do
    SS=$(grep -aoE "SS[0-9]+" "$RUNDIR/hub.log" 2>/dev/null | tail -1 |
         tr -d 'S')
    [ "${SS:-0}" = "$TOTAL" ] && break
    kill -0 "$HOST_PID" 2>/dev/null || break
    sleep 2
done
kill "$HOST_PID" 2>/dev/null
wait "$HOST_PID" 2>/dev/null
HOST_PID=

[ -f "$RUNDIR/DOSYPP.TTH" ] || {
    echo "the host client wrote no hash cache:" >&2
    tail -5 "$RUNDIR/host.log" >&2; exit 1; }

HOST_SS=$(grep -aoE "SS[0-9]+" "$RUNDIR/hub.log" | tail -1 | tr -d 'S')
echo "the host advertised $HOST_SS"
[ "$HOST_SS" = "$TOTAL" ] ||
    fail "the host side did not finish hashing ($HOST_SS of $TOTAL)"

# ---- the cache, translated for the guest
mv "$RUNDIR/DOSYPP.TTH" "$RUNDIR/host.tth"
"$ROOT/tools/mkhashcache.py" --share "$RUNDIR/DL" \
    --from-cache "$RUNDIR/host.tth" --out "$RUNDIR/DOSYPP.TTH" || exit 1

cat > "$RUNDIR/DOSYPP.INI" <<EOF
hub = $HOST_AS
port = $HUB_PORT
nick = DOSBigShare
sound = no
music = no
active = no
share = C:\\DL
EOF

cat > "$RUNDIR/dosbox-bigshare.conf" <<EOF
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
DOSYPP.EXE --null-display --door 5 --screenshot SHOT.TXT --run-secs 45 > RUN.TXT
echo DONE >> RUN.TXT
exit
EOF

echo
echo "running it on DOS, with the cache already filled in..."
SDL_VIDEODRIVER=dummy perl -e 'alarm 300; exec @ARGV' \
    dosbox-x -conf "$RUNDIR/dosbox-bigshare.conf" -nogui \
    > "$RUNDIR/dosbox.log" 2>&1

echo
echo "--- the share door, on DOS ---"
sed -n '2,14p' "$RUNDIR/SHOT.TXT" 2>/dev/null || echo "(no screenshot)"
echo "------------------------------"

# ---- what the guest told the hub
DOS_LINE=$(grep -aE "NIDOSBigShare" "$RUNDIR/hub.log" | tail -1)
DOS_SS=$(printf '%s' "$DOS_LINE" | grep -oE "SS[0-9]+" | tail -1 | tr -d 'S')
DOS_SF=$(printf '%s' "$DOS_LINE" | grep -oE "SF[0-9]+" | tail -1 |
         tr -d 'SF')

echo "the DOS client advertised SS$DOS_SS SF$DOS_SF"
echo "  the whole share is     SS$TOTAL SF$FILES"
echo "  a 32-bit print says    SS$TRUNCATED"

if [ -z "$DOS_SS" ]; then
    fail "the guest never advertised a share size"
elif [ "$DOS_SS" = "$TRUNCATED" ]; then
    fail "the guest advertised the total modulo 4GB, which is the bug"
elif [ "$DOS_SS" != "$TOTAL" ]; then
    # Most likely the cache was not accepted and it is still hashing: say
    # so, because "wrong number" and "not finished" need different fixes.
    if grep -qa "HASHING" "$RUNDIR/SHOT.TXT" 2>/dev/null; then
        fail "the guest is hashing -- the translated cache was not \
accepted (mtime or path mismatch), so this ran out of time rather than \
disagreeing"
    else
        fail "the guest advertised $DOS_SS, not $TOTAL"
    fi
else
    echo "which is the whole share, from a cache it did not have to build"
fi

[ "$DOS_SF" = "$FILES" ] || fail "the guest advertised $DOS_SF files, not $FILES"

# The cache really was used: hashing 5GB in 45 seconds is not possible, so
# a READY share is proof on its own -- but the door says it too.
grep -qa "READY" "$RUNDIR/SHOT.TXT" 2>/dev/null ||
    fail "the share door does not say READY"
grep -qa "HASHING" "$RUNDIR/SHOT.TXT" 2>/dev/null &&
    fail "the guest is still hashing, so the cache was not accepted"

echo
if [ "$ok" = 1 ]; then
    echo "BIGSHARE OK on DOS: $TOTAL bytes advertised exactly from a"
    echo "machine where long is 32 bits, with no hashing in the guest."
    exit 0
fi
echo "--- what failed ---$FAILURES" >&2
echo "BIGSHARE FAILED on DOS" >&2
exit 1
