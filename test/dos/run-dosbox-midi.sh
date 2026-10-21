#!/bin/bash
# Premium game audio on DOS, measured rather than asserted.
#
# A game that wants an MT-32 or an SC-55 writes MIDI notes to an MPU-401 at
# 0x330. The interface is a serial port; the sound comes from whatever is
# listening. Under DOSBox-X that is FluidSynth with a SoundFont, and this
# rig checks the whole chain end to end:
#
#   MIDITEST.EXE -> MPU-401 at 0x330 -> DOSBox-X -> FluidSynth -> samples
#
# Why the samples and not a log line: every part of this can report success
# and produce silence. Writing this rig found exactly that -- the MPU-401
# answered, FluidSynth loaded a 40MB SoundFont, the program played its
# notes, and eleven seconds of captured audio were every-sample-zero,
# because DOSBox-X's FluidSynth opens its OWN audio device and never
# touches the emulator's mixer. Nothing in any log said so.
#
# Hence fluid.driver = file: FluidSynth renders to fluidsynth.wav in the
# working directory, which is the audio a listener would hear, minus the
# handoff to the host's speakers. That last hop is the one thing here that
# no rig can check.
#
# Two runs, because sound on its own proves less than it looks:
#
#   mpu401 = intelligent   the notes arrive: two chords and a gap
#   mpu401 = none          nothing to play through: silence
#
# The second is what makes the first mean anything. Without it, a startup
# beep or a stuck channel would pass.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNDIR="$ROOT/build/dos/midirun"
EXE="$ROOT/build/dos/MIDITEST.EXE"
DPMI="$ROOT/dosbox-x/c/CWSDPMI.EXE"
SF2="${DOSYPP_SF2:-$ROOT/dosbox-x/sf2/MuseScore_General.sf3}"

command -v dosbox-x >/dev/null 2>&1 || {
    echo "need dosbox-x (brew install dosbox-x)" >&2; exit 1; }
[ -f "$EXE" ] || { echo "run: make dos-midi-any" >&2; exit 1; }
[ -f "$DPMI" ] || { echo "missing CWSDPMI.EXE (run: make dosfiles)" >&2; exit 1; }
[ -f "$SF2" ] || {
    echo "no SoundFont at $SF2" >&2
    echo "  run: make sf2        (or set DOSYPP_SF2 to one you have)" >&2
    exit 1; }

# FluidSynth is a build option, and a DOSBox-X without it reports the
# device as unavailable and plays nothing -- which would look exactly like
# a broken MIDI path.
if ! otool -L "$(command -v dosbox-x)" 2>/dev/null | grep -qi fluidsynth &&
   ! ldd "$(command -v dosbox-x)" 2>/dev/null | grep -qi fluidsynth; then
    echo "note: cannot see libfluidsynth linked into dosbox-x." >&2
    echo "      If this run finds no audio, that is the first thing to" >&2
    echo "      check -- DOSBox-X needs to be built with FluidSynth." >&2
fi

rm -rf "$RUNDIR"
mkdir -p "$RUNDIR/on" "$RUNDIR/off"

FAILED=0
fail() { echo "FAIL: $*"; FAILED=1; }

# $1 = directory, $2 = mpu401 mode
conf() {
    cat > "$1/dbx.conf" <<EOF
[dosbox]
machine = svga_s3
memsize = 16
[cpu]
core = dynamic
cputype = 486
cycles = max
[midi]
mpu401 = $2
mididevice = fluidsynth
; FluidSynth's own file driver, so there is something to measure. A person
; listening leaves this unset and gets their sound card.
fluid.driver = file
fluid.soundfont = $SF2
fluid.samplerate = 48000
[sblaster]
sbtype = sb16
[autoexec]
mount c "$1"
c:
MIDITEST.EXE > MIDI.TXT
exit
EOF
}

run() {
    local dir="$1" mode="$2" secs="${3:-150}"

    cp "$EXE" "$DPMI" "$dir/"
    conf "$dir" "$mode"

    # cd into the run directory: FluidSynth's file driver writes
    # fluidsynth.wav relative to the process, and DOSBox-X gives us no way
    # to name it.
    ( cd "$dir" && SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy \
        perl -e "alarm $secs; exec @ARGV" \
        dosbox-x -conf "$dir/dbx.conf" -nogui > "$dir/dosbox.log" 2>&1 )
}

echo "--- mpu401 = intelligent, a SoundFont, and a program that plays notes"
run "$RUNDIR/on" intelligent

sed 's/^/  /' "$RUNDIR/on/MIDI.TXT" 2>/dev/null ||
    fail "the guest wrote no output at all"

grep -qa "reset acknowledged" "$RUNDIR/on/MIDI.TXT" 2>/dev/null ||
    fail "no MPU-401 answered a reset at 330h"

# Said out loud because a missing SoundFont is a silent failure with a
# cheerful log: FluidSynth starts, loads nothing, and renders zeros.
if grep -qa "Loaded SoundFont" "$RUNDIR/on/dosbox.log"; then
    echo "  $(grep -a 'Loaded SoundFont' "$RUNDIR/on/dosbox.log" | head -1 |
            sed 's/^LOG: //')"
else
    fail "FluidSynth never reported loading a SoundFont"
fi

echo
echo "--- what came out"
"$ROOT/test/dos/midirms.py" "$RUNDIR/on/fluidsynth.wav" --expect-sound ||
    fail "the audio does not have the shape of two chords and a gap"

echo
echo "--- mpu401 = none, the control: the same everything, no interface"
run "$RUNDIR/off" none 90

sed 's/^/  /' "$RUNDIR/off/MIDI.TXT" 2>/dev/null

grep -qa "no MPU-401 answered" "$RUNDIR/off/MIDI.TXT" 2>/dev/null ||
    fail "with mpu401=none the program should have found nothing, and it
       did not say so -- then the two runs are not the comparison this
       rig claims"

echo
"$ROOT/test/dos/midirms.py" "$RUNDIR/off/fluidsynth.wav" --expect-silence ||
    fail "the control run made a sound, so the first run's sound is not
       evidence of anything"

echo
if [ "$FAILED" -eq 0 ]; then
    echo "MIDI OK on DOS: a program wrote MIDI to the MPU-401 at 330h,"
    echo "DOSBox-X handed it to FluidSynth, and the SoundFont rendered the"
    echo "two chords it was sent -- with silence when the interface was"
    echo "taken away."
    echo
    echo "Not checked: that it reaches your speakers. Leave fluid.driver"
    echo "unset and it goes to your sound card instead of a file; that hop"
    echo "needs ears. See docs/DOSBOX.md."
    exit 0
fi

echo "MIDI FAILED on DOS (logs and audio in $RUNDIR)"
exit 1
