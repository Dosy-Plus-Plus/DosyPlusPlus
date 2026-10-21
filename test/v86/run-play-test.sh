#!/bin/bash
# Does a game in the demo actually start?
#
# make test-v86-dl ends at "the executable is on disk", which turned out
# not to be the same thing: BLKDROP unpacked and then died with "unable to
# find the mouse. it is really necessary." DOS has no int 33h unless
# something provides one, and nothing on the image did.
set -uo pipefail
exec < /dev/null

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN="$ROOT/build/v86play"

command -v node >/dev/null 2>&1 || { echo "need node" >&2; exit 1; }
command -v mdir >/dev/null 2>&1 || { echo "need mtools" >&2; exit 1; }

export MTOOLS_SKIP_CHECK=1
[ -f "$ROOT/web/images/dosypp.img" ] || {
    echo "no image -- run: make images" >&2; exit 1; }

for f in CTMOUSE.EXE PLAY.BAT UNZIP.EXE; do
    mdir -i "$ROOT/web/images/dosypp.img" "::$f" >/dev/null 2>&1 || {
        echo "dosypp.img has no $f" >&2
        echo "  run: make atmos && ./tools/mkv86img.sh" >&2
        exit 1; }
done

mkdir -p "$RUN"
node "$ROOT/test/v86/play.mjs" \
    "$ROOT/web/images/boot.img" "$ROOT/web/images/dosypp.img" "${1:-150}" \
    2>&1 | tee "$RUN/play.log"
exit "${PIPESTATUS[0]}"
