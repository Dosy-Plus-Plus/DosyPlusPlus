#!/bin/bash
# Browse somebody else's share on real DOS, over a packet driver.
#
# This is the part of browsing the host build cannot vouch for: bzip2
# decompression inside 16MB, on a 386-class target, with the decompressor in
# `small` mode. A list from a stranger may have been compressed at any block
# size, and getting that wrong on DOS means a client that dies when somebody
# with a big share shares it.
#
# --browse-dir walks into a directory as the listings arrive. The DOS rig
# needs it because DOSBox-X's AUTOTYPE is synchronous -- its keystrokes land
# at the command prompt before the program under test starts -- so there is
# no way to press ENTER inside a running client here. It is a useful flag
# in its own right: a browse that opens where you meant to go.
#
# The DOS client browses and the host client shares, because DOSBox-X's slirp
# backend has no inbound mapping: nothing can dial into the guest, so the
# guest has to be the side that dials.
#
# One subtlety that makes or breaks the whole rig: the address a peer is
# dialled at comes from the I4 field the hub observed, not from the CTM. The
# host uploader connects over loopback, so the hub would record 127.0.0.1 --
# which inside the guest means the guest itself. The stub hub is told to
# advertise 10.0.2.2 instead, which is how slirp presents the host. Pointing
# the uploader at the host's LAN address instead would be more lifelike but
# needs the macOS firewall to allow inbound connections, which it does not
# by default.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNDIR="$ROOT/build/dos/browserun"
SRC="$ROOT/dosbox-x/c"
CLIENT="$ROOT/build/host/dosypp"
source "$ROOT/test/host/hublib.sh"
# A free port, not 1511: a rig that reuses whatever is already listening
# spends its run testing somebody else's hub. See hublib.sh.
HUB_PORT=${BROWSE_HUB_PORT:-$(free_port)}
PEER_PORT=${BROWSE_PEER_PORT:-14141}
SIZE=${BROWSE_SIZE:-120000}

command -v dosbox-x >/dev/null 2>&1 || { echo "need dosbox-x (brew install dosbox-x)" >&2; exit 1; }
command -v rhash >/dev/null 2>&1 || { echo "need rhash (brew install rhash)" >&2; exit 1; }
[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }
for f in DOSYPP.EXE CWSDPMI.EXE NE2000.COM; do
    [ -f "$SRC/$f" ] || { echo "missing $SRC/$f (run: make dosfiles)" >&2; exit 1; }
done

# How the host appears from inside slirp.
HOST_AS_GUEST_SEES_IT=10.0.2.2

rm -rf "$RUNDIR"
mkdir -p "$RUNDIR" "$RUNDIR/up/warez/apps" "$RUNDIR/up/mods"

python3 - "$RUNDIR/up" "$SIZE" <<'MAKESHARE'
import os, random, sys
root, size = sys.argv[1], int(sys.argv[2])
random.seed(1994)
# Enough files to make a list worth compressing, and deep enough that
# navigating it on DOS is actually exercised.
for rel in ["warez/MAZERUN2.ZIP", "warez/apps/ARCPACK2.EXE",
            "warez/apps/DSKTOOLS.EXE", "mods/TUNE0001.MOD", "README.TXT"]:
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(bytes(random.getrandbits(8) for _ in range(size // 5)))
print("made a share of 5 files")
MAKESHARE

echo "the guest will dial the host as $HOST_AS_GUEST_SEES_IT:$PEER_PORT"

cp "$SRC"/DOSYPP.EXE "$SRC"/CWSDPMI.EXE "$SRC"/NE2000.COM "$RUNDIR/"

cat > "$RUNDIR/DOSYPP.INI" <<EOF
hub = 10.0.2.2
port = $HUB_PORT
nick = DOSNosey
sound = no
active = no
EOF

cat > "$RUNDIR/up.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = HostSharer
sound = no
active = yes
port.listen = $PEER_PORT
share = $RUNDIR/up
EOF

cleanup() {
    [ -n "${HUB:-}" ] && kill "$HUB" 2>/dev/null
    [ -n "${UP:-}" ] && kill "$UP" 2>/dev/null
}
trap cleanup EXIT

# --no-fake-share so the only answer to the search is the real file.
start_stub_hub "$HUB_PORT" "$RUNDIR/hub.log" --no-fake-share \
    --loopback-as "$HOST_AS_GUEST_SEES_IT"
HUB=$HUB_PID

echo "starting the host sharer (active)..."
# In its own directory: the share's FILES.XML and FILES.BZ2 are written to
# the working directory, and without this they land wherever the script was
# run from.
( cd "$RUNDIR/up" && exec "$CLIENT" -c "$RUNDIR/up.ini" --null-display --door 5 \
    --peer-trace --run-secs 180 127.0.0.1 ) > "$RUNDIR/up.log" 2>&1 &
UP=$!
for _ in $(seq 120); do
    grep -qE "BINF .* SS[1-9]" "$RUNDIR/hub.log" 2>/dev/null && break
    sleep 0.5
done
grep -q "listening on port" "$RUNDIR/up.log" || {
    echo "the sharer is not listening:" >&2; cat "$RUNDIR/up.log" >&2
    exit 1; }

[ -f "$RUNDIR/up/FILES.BZ2" ] || {
    echo "the host sharer never published FILES.BZ2" >&2; exit 1; }
echo "the sharer published FILES.BZ2 ($(wc -c < "$RUNDIR/up/FILES.BZ2" | tr -d ' ') bytes)"

cat > "$RUNDIR/dosbox-xfer.conf" <<EOF
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
DOSYPP.EXE --screenshot SHOT.TXT --null-display --door 8 --peer-trace --browse HostSharer --browse-dir /up/warez/ --run-secs 60 > RUN.TXT
echo DONE >> RUN.TXT
exit
EOF

echo "browsing on DOS..."
SDL_VIDEODRIVER=dummy perl -e 'alarm 400; exec @ARGV' \
    dosbox-x -conf "$RUNDIR/dosbox-xfer.conf" -nogui \
    > "$RUNDIR/dosbox.log" 2>&1

echo "--- the browse door, on DOS ---"
sed -n '2,16p' "$RUNDIR/SHOT.TXT" 2>/dev/null || echo "(no screenshot)"
echo "-------------------------------"

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

# Partial lists are what the client asks for: the root one level deep, and
# then a directory when it is opened. On DOS that is the point of them --
# a stranger's 200,000-file list does not fit in 16MB, and one directory
# always does.
grep -qa "CGET list /" "$RUNDIR/RUN.TXT" ||
    fail "the DOS client never asked for a partial list"
grep -q "CSND list /" "$RUNDIR/up.log" ||
    fail "the host sharer never sent a directory listing"
grep -qa "CGET file files.xml.bz2" "$RUNDIR/RUN.TXT" &&
    fail "the DOS client fell back to the whole list for no reason"

[ -f "$RUNDIR/SHOT.TXT" ] || fail "no screenshot came back from DOS"
# The DOS side has to have walked into a directory, not just shown the
# root: entering one is what triggers the second request.
ASKED=$(grep -ca "CGET list " "$RUNDIR/RUN.TXT")
echo "the DOS client asked for $ASKED listings"
[ "${ASKED:-0}" -ge 2 ] ||
    fail "only $ASKED listing(s) asked for -- nothing was opened"

grep -q "BROWSING HostSharer" "$RUNDIR/SHOT.TXT" ||
    fail "the door is not showing the sharer's list"
grep -q "DIR" "$RUNDIR/SHOT.TXT" ||
    fail "nothing in the list is marked as a directory"
grep -qE "[0-9]+ entries" "$RUNDIR/SHOT.TXT" ||
    fail "no entry count -- the list did not parse"
grep -qi "could not be read" "$RUNDIR/SHOT.TXT" &&
    fail "DOS could not read the file list"
grep -qi "incomplete" "$RUNDIR/SHOT.TXT" &&
    fail "a five-file list was reported as incomplete on DOS"

if grep -qE "[0-9]+ entries" "$RUNDIR/SHOT.TXT"; then
    echo "DOS parsed: $(grep -oE '[0-9]+ entries' "$RUNDIR/SHOT.TXT" | head -1)"
fi

# The temporary must be gone: it is someone else's share.
[ -f "$RUNDIR/PEERLIST.BZ2" ] &&
    fail "the fetched list was left on the DOS disk"

echo
if [ "$ok" = 1 ]; then
    echo "BROWSE OK on DOS: three partial listings fetched, decompressed,"
    echo "parsed and walked -- one directory at a time, inside 16MB."
    exit 0
fi

echo "--- DOS output ---" >&2
cat "$RUNDIR/RUN.TXT" 2>/dev/null >&2
echo "--- what failed ---$FAILURES" >&2
echo "BROWSE FAILED on DOS" >&2
exit 1
