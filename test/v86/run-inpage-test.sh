#!/bin/bash
# The whole demo with no server at all: no relay, no hub process, no ports.
#
# web/wisp.mjs stands in for the WebSocket v86 would open, and
# web/adchub.mjs answers as the hub and as the node that has the files --
# the same modules the page loads. So this rig is not a model of the
# in-page demo; it is the in-page demo with Node where the browser would
# be.
#
# The verdict is the client's own: it logs in, searches, downloads a game
# and reports it complete -- which it only does after the TTH matches the
# one the hub advertised.
set -uo pipefail

# mtools asks about a name clash, and asks on stdin: an mmd for a
# directory that exists, or an mcopy without -o, waits for an answer that
# no rig is going to give. Non-interactive runs get EOF and carry on;
# a person running this in a terminal waits for ever, with the question
# usually hidden by a 2>/dev/null. One line removes the whole class.
exec < /dev/null


ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN="$ROOT/build/v86inpage"
WAREZ="${WAREZ_DIR:-$ROOT/build/warez}"

command -v node >/dev/null 2>&1 || { echo "need node" >&2; exit 1; }
command -v mcopy >/dev/null 2>&1 || { echo "need mtools" >&2; exit 1; }

[ -f "$WAREZ/SHARE.TXT" ] || {
    echo "no share -- run: make warez" >&2; exit 1; }
[ -f "$ROOT/web/images/dosypp.img" ] || {
    echo "no image -- run: make images" >&2; exit 1; }

export MTOOLS_SKIP_CHECK=1

# What the guest is about to be told to run. Checked rather than assumed:
# a half-written image boots to a DOS prompt and says "Bad command or
# filename" on a screen no rig reads, which is a long way to go to learn
# that `make images` was interrupted.
for f in AUTODL.BAT DOSYPP.EXE UNZIP.EXE PCIPKT.COM DOSYPP.INI; do
    mdir -i "$ROOT/web/images/dosypp.img" "::$f" >/dev/null 2>&1 || {
        echo "dosypp.img has no $f -- the image is incomplete." >&2
        echo "  run: ./tools/mkv86img.sh" >&2
        exit 1; }
done

rm -rf "$RUN"; mkdir -p "$RUN"
cp "$ROOT/web/images/boot.img" "$RUN/boottest.img"
cat > "$RUN/FDAUTO.BAT" <<'EOF'
@echo off
set DOSDIR=A:\FREEDOS
path A:\BIN;C:\
c:
call C:\AUTODL.BAT
EOF
mcopy -i "$RUN/boottest.img" -o "$RUN/FDAUTO.BAT" ::FDAUTO.BAT

node "$ROOT/test/v86/inpage.mjs" \
    "$RUN/boottest.img" "$ROOT/web/images/dosypp.img" "${1:-240}" \
    2>&1 | tee "$RUN/inpage.log" | grep -vE '^\[wisp\] connected'
rc=${PIPESTATUS[0]}

if [ "$rc" -eq 0 ]; then
    echo
    echo "  no process listened on any port during that run:"
    echo "  the relay, the hub and the peer were all objects in one script."
fi
exit $rc
