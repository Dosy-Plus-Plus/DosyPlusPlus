#!/bin/bash
# Fetch DOS packet drivers into dosbox-x/c/.
#
# These are the one runtime dependency DOSy++ cannot ship: which driver you
# need depends on the emulated or real NIC, and they are separately licensed.
# The Crynwr drivers are GPL, which is why they are fetched rather than
# vendored into this repo.
#
# Note crynwr.com no longer resolves -- the company's domain is gone -- so
# this uses a GitHub mirror that also carries the assembler source.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/dosbox-x/c"

NE2000_URL="${NE2000_URL:-https://raw.githubusercontent.com/yeokm1/ne2000plus-collection/master/crynwr/NE2000.COM}"
# Realtek's own packet driver for the RTL8029 (PCI NE2000), which is the chip
# v86 emulates. The ISA NE2000.COM cannot find a PCI card.
PCIPKT_URL="${PCIPKT_URL:-https://archive.org/download/pcind.dos/RTL8029_PKT_v1.10.zip}"
CACHE="${DOSYPP_CACHE:-$ROOT/.cache}"

mkdir -p "$DEST"

get() {
    local url="$1" out="$2" what="$3"
    if [ -f "$DEST/$out" ]; then
        echo "  have $out"
        return 0
    fi
    echo "  get  $out  ($what)"
    if curl -fsSL "$url" -o "$DEST/$out.part"; then
        mv "$DEST/$out.part" "$DEST/$out"
    else
        rm -f "$DEST/$out.part"
        echo "       FAILED -- fetch it by hand, see docs/PKTDRV.md" >&2
        return 1
    fi
}

echo "packet drivers:"

# ISA NE2000: DOSBox-X's emulated card, and real NE2000/NE1000 hardware.
# Verified working: DOSBox-X ne2000=true, nicbase=300, nicirq=10, loaded as
#   NE2000 0x60 10 0x300
get "$NE2000_URL" NE2000.COM "Crynwr ISA NE2000 -- DOSBox-X, real ISA cards" || true

# PCI RTL8029: what v86 emulates. Shipped as a zip, so unpack it.
if [ -f "$DEST/PCIPKT.COM" ]; then
    echo "  have PCIPKT.COM"
else
    mkdir -p "$CACHE"
    if [ ! -f "$CACHE/RTL8029_PKT_v1.10.zip" ]; then
        echo "  get  RTL8029_PKT_v1.10.zip  (Realtek PCI RTL8029 -- v86)"
        curl -fsSL "$PCIPKT_URL" -o "$CACHE/RTL8029_PKT_v1.10.zip.part" \
            && mv "$CACHE/RTL8029_PKT_v1.10.zip.part" "$CACHE/RTL8029_PKT_v1.10.zip"
    fi
    if [ -f "$CACHE/RTL8029_PKT_v1.10.zip" ]; then
        unzip -o -q -j "$CACHE/RTL8029_PKT_v1.10.zip" "EXE/PCIPKT.COM" -d "$DEST"
        echo "  +    PCIPKT.COM"
    else
        echo "       FAILED -- see docs/PKTDRV.md" >&2
    fi
fi

echo
ls -l "$DEST"/*.COM 2>/dev/null || true
echo
echo "Next:"
echo "  ./test/dos/run-dosbox-net.sh      boot on DOSBox-X (ISA NE2000)"
echo "  ./test/v86/run-node-test.sh       boot headless in v86 (PCI RTL8029)"
