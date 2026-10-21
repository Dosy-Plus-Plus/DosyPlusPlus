#!/bin/bash
# Game MIDI in the browser demo, end to end and measured.
#
# v86 emulates an MPU-401 that answers a game's probe and discards every
# data byte, so a DOS game that offers "Roland MPU-401" detects one and
# plays perfect silence -- worse than finding nothing, because with no
# interface at all it would have used the Sound Blaster.
#
# The page takes the bytes off the port itself (web/midi.mjs) and plays
# them through a SoundFont synthesiser (web/synth.mjs). This checks both
# halves with no browser:
#
#   1. the stream -- a real DOS program writes MIDI inside v86, and every
#      byte is recovered outside the engine, in order, byte for byte
#   2. the sound -- those messages, with the timing they arrived with, are
#      rendered through the same synthesiser the page uses, and the audio
#      is measured by test/dos/midirms.py: the same listener that judges
#      the DOSBox-X MIDI path
#
# What neither half covers is the AudioWorklet in the page -- there is no
# browser here. The synthesis is the same code; the handoff to a speaker is
# not, and nothing in this repository can hear it.
set -uo pipefail

# mtools asks about a name clash, and asks on stdin: an mmd for a
# directory that exists, or an mcopy without -o, waits for an answer that
# no rig is going to give. Non-interactive runs get EOF and carry on;
# a person running this in a terminal waits for ever, with the question
# usually hidden by a 2>/dev/null. One line removes the whole class.
exec < /dev/null


ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN="$ROOT/build/v86midi"
WAV="$RUN/rendered.wav"

command -v node >/dev/null 2>&1 || { echo "need node" >&2; exit 1; }
command -v mcopy >/dev/null 2>&1 || { echo "need mtools" >&2; exit 1; }

for f in web/images/boot.img web/images/dosypp.img \
         web/vendor/synth/spessasynth_core.js; do
    [ -f "$ROOT/$f" ] || {
        echo "missing $f" >&2
        case "$f" in
            web/vendor/synth/*) echo "  run: tools/fetchsynth.sh" >&2 ;;
            *)                  echo "  run: make images" >&2 ;;
        esac
        exit 1; }
done

SF="${DOSYPP_SF2:-$ROOT/dosbox-x/sf2/MuseScore_General.sf3}"
[ -f "$SF" ] || { echo "no SoundFont at $SF -- run: make sf2" >&2; exit 1; }

mdir -i "$ROOT/web/images/dosypp.img" ::MIDITEST.EXE >/dev/null 2>&1 || {
    echo "dosypp.img has no MIDITEST.EXE" >&2
    echo "  run: make dos-midi-any && make images" >&2
    exit 1; }

export MTOOLS_SKIP_CHECK=1
rm -rf "$RUN"; mkdir -p "$RUN"

# A boot floppy that runs the MIDI batch instead of the interactive
# startup. No hub and no relay: this has nothing to do with the network,
# and a failure should not be able to mean "the hub was down".
cp "$ROOT/web/images/boot.img" "$RUN/boottest.img"
cat > "$RUN/FDAUTO.BAT" <<'EOF'
@echo off
set DOSDIR=A:\FREEDOS
path A:\BIN;C:\
c:
call C:\AUTOMIDI.BAT
EOF
mcopy -i "$RUN/boottest.img" -o "$RUN/FDAUTO.BAT" ::FDAUTO.BAT

FAILED=0
fail() { echo "FAIL: $*"; FAILED=1; }

echo "--- the stream: MIDI out of the emulated MPU-401"
DOSYPP_SF2="$SF" MIDI_WAV="$WAV" \
    node "$ROOT/test/v86/midi.mjs" \
    "$RUN/boottest.img" "$ROOT/web/images/dosypp.img" "${1:-240}" \
    2>&1 | tee "$RUN/midi.log" | grep -vE '^\s{2}[0-9a-f]{2} '
rc=${PIPESTATUS[0]}

[ "$rc" -eq 0 ] || fail "the MIDI stream did not come out of v86 intact"

echo
echo "--- the sound: those messages through the page's own synthesiser"
if [ ! -f "$WAV" ]; then
    fail "nothing was rendered, so there is no audio to judge"
else
    "$ROOT/test/dos/midirms.py" "$WAV" --expect-sound |
        grep -vE '^\s{4}[0-9]+\.[0-9]+s' ||
        fail "the rendered audio is not two chords and a gap"
fi

echo
if [ "$FAILED" -eq 0 ]; then
    echo "V86 MIDI OK: a DOS program inside the browser engine wrote MIDI to"
    echo "the MPU-401 v86 throws away, every byte was recovered outside the"
    echo "engine, and the page's synthesiser rendered the chords it was sent."
    echo
    echo "Not checked: the AudioWorklet, and the hop from it to a speaker."
    echo "There is no browser in this rig. The synthesis is the same code."
    exit 0
fi

echo "V86 MIDI FAILED (logs and audio in $RUN)"
exit 1
