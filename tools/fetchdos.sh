#!/bin/bash
# Fetch the freely redistributable DOS runtime bits into dosbox-x/c/, where
# both the DOSBox-X rig and tools/mkv86img.sh look for them.
#
# CWSDPMI is DJGPP's DPMI host: every DJGPP binary needs it at runtime, and
# build-djgpp does not install it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/dosbox-x/c"
CACHE="${DOSYPP_CACHE:-$ROOT/.cache}"

CSDPMI_URL="${CSDPMI_URL:-https://www.delorie.com/pub/djgpp/current/v2misc/csdpmi7b.zip}"

mkdir -p "$DEST" "$CACHE"

if [ ! -f "$DEST/CWSDPMI.EXE" ]; then
    if [ ! -f "$CACHE/csdpmi7b.zip" ]; then
        echo "fetching CWSDPMI..."
        curl -fsSL "$CSDPMI_URL" -o "$CACHE/csdpmi7b.zip.part"
        mv "$CACHE/csdpmi7b.zip.part" "$CACHE/csdpmi7b.zip"
    fi
    unzip -o -q -j "$CACHE/csdpmi7b.zip" "bin/CWSDPMI.EXE" -d "$DEST"
    echo "  + CWSDPMI.EXE"
else
    echo "  have CWSDPMI.EXE"
fi

echo
echo "Packet drivers come from ./tools/fetchpktdrv.sh (make pktdrv) -- they"
echo "are separately licensed, and the right one depends on the target:"
echo
printf '  %-14s %-28s %s\n' FILE TARGET WHERE
printf '  %-14s %-28s %s\n' NE2000.COM "DOSBox-X, ISA NE2000 cards" "packetdriversdos.net"
printf '  %-14s %-28s %s\n' PCIPKT.COM "v86 (PCI RTL8029)" "Realtek PCI packet driver"
echo
echo "Fetch them with:  make pktdrv"
