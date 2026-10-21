#!/bin/bash
# Fetch v86 runtime files into web/vendor/.
#
# The npm package ships the engine but no BIOS, so the BIOS images come from
# the v86 repository. Pin the version: v86's config shape has changed before,
# and a silently-updated engine is a bad surprise in an emulator.
set -euo pipefail

V86_VERSION="${V86_VERSION:-0.5.470}"
V86_REF="${V86_REF:-master}"          # ref for the BIOS files

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENDOR="$ROOT/web/vendor"
mkdir -p "$VENDOR"

CDN="https://cdn.jsdelivr.net/npm/v86@${V86_VERSION}/build"
RAW="https://raw.githubusercontent.com/copy/v86/${V86_REF}/bios"

get() {
    local url="$1" out="$2"
    if [ -f "$out" ]; then
        echo "  have $(basename "$out")"
        return
    fi
    echo "  get  $(basename "$out")"
    curl -fsSL "$url" -o "$out.part"
    mv "$out.part" "$out"
}

echo "v86 engine ${V86_VERSION}:"
get "$CDN/libv86.mjs" "$VENDOR/libv86.mjs"
get "$CDN/v86.wasm"   "$VENDOR/v86.wasm"

echo "BIOS (v86 @ ${V86_REF}):"
get "$RAW/seabios.bin" "$VENDOR/seabios.bin"
get "$RAW/vgabios.bin" "$VENDOR/vgabios.bin"

cat > "$VENDOR/VERSIONS.txt" <<EOF
v86 engine: ${V86_VERSION} (npm)
BIOS:       copy/v86 @ ${V86_REF}
fetched:    $(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

echo
ls -lh "$VENDOR"
echo
echo "next: tools/mkv86img.sh to build images/dosypp.img"
