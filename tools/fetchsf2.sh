#!/bin/bash
# Fetch a SoundFont, so DOSBox-X's FluidSynth has something to play with.
#
#   tools/fetchsf2.sh            -> dosbox-x/sf2/
#
# An MPU-401 is a serial port, not an instrument: a DOS game writes MIDI
# notes into it and something else has to make the sound. DOSBox-X links
# FluidSynth, which makes it -- from a SoundFont, which is the actual
# source of the "premium" in premium game audio. Without one, every game
# that detects a Roland plays perfect silence.
#
# MuseScore General, by S. Christian Collins: MIT licensed, 40MB, and in
# the Roland SC-55 tradition that DOS games were written for. Fetched
# rather than committed, like Watt-32, bzip2, the packet drivers and the
# FreeDOS packages -- and for one more reason: the better-known
# alternatives are not redistributable. Roland's MT-32 and CM-32L ROMs are
# not, which is why DOSBox-X's munt emulation is documented here and left
# for people who own them.
#
# Already have a SoundFont? Point DOSYPP_SF2 at it and nothing is fetched:
#   DOSYPP_SF2=~/sf2/FluidR3_GM.sf2 make dos-midi-check
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/dosbox-x/sf2"
CACHE="${DOSYPP_CACHE:-$ROOT/.cache}"

NAME="MuseScore_General.sf3"
URL="${SF2_URL:-https://ftp.osuosl.org/pub/musescore/soundfont/MuseScore_General/$NAME}"
SHA="5b85b6c2c61d10b2b91cddd41efcce7b25cd31c8271d511c73afafbef20b6fa3"

die() { echo "error: $*" >&2; exit 1; }

sha_of() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        sha256sum "$1" | cut -d' ' -f1
    fi
}

mkdir -p "$DEST" "$CACHE"

if [ -f "$DEST/$NAME" ] && [ "$(sha_of "$DEST/$NAME")" = "$SHA" ]; then
    echo "  have $NAME  (sha256 verified)"
    exit 0
fi

if [ ! -f "$CACHE/$NAME" ]; then
    echo "fetching $NAME (40MB)..."
    curl -fsSL -m 900 -o "$CACHE/$NAME.part" "$URL" ||
        die "could not fetch $URL"
    mv "$CACHE/$NAME.part" "$CACHE/$NAME"
fi

# RIFF first, so a captive-portal HTML page is named as one rather than
# reported as a hash mismatch.
if [ "$(head -c 4 "$CACHE/$NAME" | tr -d '\0')" != "RIFF" ]; then
    rm -f "$CACHE/$NAME"
    die "what came back is not a SoundFont (no RIFF header).
       Check $URL by hand."
fi

GOT="$(sha_of "$CACHE/$NAME")"
if [ "$GOT" != "$SHA" ]; then
    rm -f "$CACHE/$NAME"
    die "$NAME does not match the hash this script pins.
       wanted $SHA
       got    $GOT
       The file at that URL changed, or the download was tampered with.
       Nothing is installed."
fi

cp "$CACHE/$NAME" "$DEST/$NAME"

cat > "$DEST/README.txt" <<TXT
$NAME

MuseScore General, by S. Christian Collins. MIT licensed. Fetched by
tools/fetchsf2.sh, which pins its sha256; not part of this repository.

    https://musescore.org/en/handbook/soundfonts-and-sfz-files

DOSBox-X plays MIDI through it (see dosbox-x/dosbox-x.conf, [midi]), which
is what a DOS game writing to the MPU-401 at 0x330 ends up being heard
through. Delete this directory to undo.
TXT

echo "  + $NAME  ($(du -h "$DEST/$NAME" | cut -f1), sha256 verified)"
echo "  + README.txt  (where it came from, and its licence)"
echo
echo "DOSBox-X is configured to use it: dosbox-x/dosbox-x.conf, [midi]"
