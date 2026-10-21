#!/bin/bash
# Doom's soundtrack, in a browser engine, through a SoundFont.
#
# The browser demo's claim is that you can download a game over Direct
# Connect and hear its music. test/v86/run-midi-test.sh proves the MPU-401
# tap recovers a byte sequence we wrote ourselves; this proves a real game
# produces one.
#
# BOOM 2.02 is the GPL Doom engine and the FreeDOS package ships a free WAD
# with it, so the whole thing is redistributable -- which is why it is in
# the demo share at all. It plays its title music with no input, so a rig
# can listen to it.
#
# The image is built here rather than shipped: 19MB of unpacked Doom has no
# business in a 128MB image that gets published, and the demo's point is
# that you download it from the hub.
#
# What it checks:
#   - Boom starts (its own banner, over the serial port)
#   - the MPU-401 stream has a soundtrack's worth of notes in it
#   - rendering those notes produces continuous, varying music
#     (test/dos/midirms.py --expect-music, the same listener as the
#     DOSBox-X MIDI gate)
set -uo pipefail

# mtools asks about a name clash, and asks on stdin: an mmd for a
# directory that exists, or an mcopy without -o, waits for an answer that
# no rig is going to give. Non-interactive runs get EOF and carry on;
# a person running this in a terminal waits for ever, with the question
# usually hidden by a 2>/dev/null. One line removes the whole class.
exec < /dev/null


ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN="$ROOT/build/v86doom"
ZIP="${BOOM_ZIP:-$ROOT/build/atmos/WAREZ/GAMES/BOOM.ZIP}"
SECS="${1:-75}"
SF="${DOSYPP_SF2:-$ROOT/dosbox-x/sf2/MuseScore_General.sf3}"

command -v node >/dev/null 2>&1 || { echo "need node" >&2; exit 1; }
command -v mcopy >/dev/null 2>&1 || { echo "need mtools" >&2; exit 1; }
command -v unzip >/dev/null 2>&1 || { echo "need unzip" >&2; exit 1; }

[ -f "$ZIP" ] || { echo "no $ZIP -- run: make atmos" >&2; exit 1; }
[ -f "$SF" ] || { echo "no SoundFont at $SF -- run: make sf2" >&2; exit 1; }
[ -f "$ROOT/web/vendor/synth/spessasynth_core.js" ] || {
    echo "no synthesiser -- run: tools/fetchsynth.sh" >&2; exit 1; }
[ -f "$ROOT/web/images/boot.img" ] || {
    echo "no boot floppy -- run: make images" >&2; exit 1; }

export MTOOLS_SKIP_CHECK=1
rm -rf "$RUN"; mkdir -p "$RUN/unz"

echo "--- unpacking Boom"
unzip -q "$ZIP" -d "$RUN/unz"
BOOMDIR="$RUN/unz/GAMES/BOOM"
for f in BOOM.EXE BOOM.CFG CWSDPMI.EXE DOOM.WAD; do
    [ -f "$BOOMDIR/$f" ] || {
        echo "$ZIP has no GAMES/BOOM/$f -- the package changed shape" >&2
        exit 1; }
done

# music_card = 6 is MIDI_MPU in Allegro 3's numbering. The package ships
# -1, autodetect, and autodetect in v86 finds the SB16 and picks an FM
# device v86 does not implement -- so Boom plays, happily, in silence.
# This single number is the difference.
python3 - "$BOOMDIR/BOOM.CFG" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p, "r", errors="replace").read()
s, n = re.subn(r"^music_card\s+-?\d+", "music_card                    6",
               s, flags=re.M)
if n != 1:
    sys.exit(f"could not find music_card in {p} ({n} matches)")
open(p, "w").write(s)
print("  BOOM.CFG: music_card = 6 (MIDI_MPU)")
PY
[ $? -eq 0 ] || exit 1

echo "--- building a scratch image"
IMG="$RUN/doom.img"
mformat -i "$IMG" -C -T $((64 * 1024 * 1024 / 512)) -h 16 -s 63 \
        -v DOOMTEST ::
mmd -i "$IMG" ::GAMES
mmd -i "$IMG" ::GAMES/BOOM
for f in BOOM.EXE BOOM.CFG CWSDPMI.EXE DOOM.WAD; do
    mcopy -i "$IMG" -o "$BOOMDIR/$f" "::GAMES/BOOM/$f"
done
printf '@echo off\r\ncd GAMES\\BOOM\r\nBOOM.EXE > COM1\r\n' \
    > "$RUN/AUTODOOM.BAT"
mcopy -i "$IMG" -o "$RUN/AUTODOOM.BAT" ::AUTODOOM.BAT

cp "$ROOT/web/images/boot.img" "$RUN/boot.img"
cat > "$RUN/FDAUTO.BAT" <<'EOF'
@echo off
set DOSDIR=A:\FREEDOS
path A:\BIN;C:\
c:
call C:\AUTODOOM.BAT
EOF
mcopy -i "$RUN/boot.img" -o "$RUN/FDAUTO.BAT" ::FDAUTO.BAT

FAILED=0
fail() { echo "FAIL: $*"; FAILED=1; }

echo
echo "--- running it for ${SECS}s"
DOSYPP_SF2="$SF" node "$ROOT/test/v86/doom.mjs" \
    "$RUN/boot.img" "$IMG" "$SECS" "$RUN/doom.wav" 2>&1 |
    tee "$RUN/doom.log"
[ "${PIPESTATUS[0]}" -eq 0 ] || fail "Boom did not produce a soundtrack"

echo
echo "--- what it sounds like"
if [ -f "$RUN/doom.wav" ]; then
    "$ROOT/test/dos/midirms.py" "$RUN/doom.wav" --expect-music |
        grep -vE '^\s{4}[0-9]+\.[0-9]+s' || fail "that is not music"
else
    fail "nothing was rendered"
fi

echo
if [ "$FAILED" -eq 0 ]; then
    echo "V86 DOOM OK: a GPL Doom engine, running on DOS inside a browser"
    echo "engine, wrote its soundtrack to an MPU-401 that discards"
    echo "everything -- and the page's own tap and synthesiser turned it"
    echo "back into music."
    echo
    echo "  audio: $RUN/doom.wav  (play it if you want to hear the proof)"
    exit 0
fi

echo "V86 DOOM FAILED (logs and audio in $RUN)"
exit 1
