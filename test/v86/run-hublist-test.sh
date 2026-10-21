#!/bin/bash
# The hub-list door in the browser demo, and what the guest tries to reach.
#
# Two questions, and the second is the one that matters before strangers
# are pointed at this:
#
#   1. Does door 0 show this demo's own hub? The client's built-in default
#      is a real public hub-list service somebody else pays for, so the
#      demo image overrides it with a one-entry list served beside the demo
#      hub (tools/mkhublist.py).
#
#   2. Does the guest reach anything that was not mapped for it? The relay
#      is an allowlist -- an unmapped address cannot be connected to at all
#      -- so this checks the relay's own log for refusals as well as for
#      the connections it allowed. A demo that quietly tried somebody
#      else's hub would show up here even though it could not get there.
set -uo pipefail

# mtools asks about a name clash, and asks on stdin: an mmd for a
# directory that exists, or an mcopy without -o, waits for an answer that
# no rig is going to give. Non-interactive runs get EOF and carry on;
# a person running this in a terminal waits for ever, with the question
# usually hidden by a 2>/dev/null. One line removes the whole class.
exec < /dev/null


ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN="$ROOT/build/v86hublist"
RELAY_PORT=18103
HUB_PORT=11541
HL_PORT=11542

command -v node >/dev/null 2>&1 || { echo "need node" >&2; exit 1; }
command -v mcopy >/dev/null 2>&1 || { echo "need mtools" >&2; exit 1; }

[ -f "$ROOT/web/images/dosypp.img" ] || {
    echo "no image -- run: make images" >&2; exit 1; }

export MTOOLS_SKIP_CHECK=1
rm -rf "$RUN"; mkdir -p "$RUN/web"

mdir -i "$ROOT/web/images/dosypp.img" ::AUTOHUBL.BAT >/dev/null 2>&1 || {
    echo "dosypp.img has no AUTOHUBL.BAT -- run: ./tools/mkv86img.sh" >&2
    exit 1; }

# The list the demo serves: one hub, its own.
"$ROOT/tools/mkhublist.py" "$RUN/web" \
    --name "The Attic (demo)" \
    --address "adc://10.77.0.1:$HUB_PORT" | sed 's/^/  /'

cp "$ROOT/web/images/boot.img" "$RUN/boottest.img"
cat > "$RUN/FDAUTO.BAT" <<'EOF'
@echo off
set DOSDIR=A:\FREEDOS
path A:\BIN;C:\
c:
call C:\AUTOHUBL.BAT
EOF
mcopy -i "$RUN/boottest.img" -o "$RUN/FDAUTO.BAT" ::FDAUTO.BAT

cleanup() {
    for p in ${HUB_PID:-} ${HL_PID:-} ${RELAY_PID:-}; do
        [ -n "$p" ] && { kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; }
    done
}
trap cleanup EXIT

"$ROOT/tools/stubhub.py" --port "$HUB_PORT" > "$RUN/hub.log" 2>&1 &
HUB_PID=$!

# The hub list over HTTP, which is how the client fetches one.
python3 -m http.server -b 127.0.0.1 -d "$RUN/web" "$HL_PORT" \
    > "$RUN/web.log" 2>&1 &
HL_PID=$!

# Three mappings and nothing else. Anything the guest dials that is not in
# this list is refused by the relay, and logged as refused.
"$ROOT/tools/wisprelay.py" --port "$RELAY_PORT" -v \
    --map "10.77.0.1=127.0.0.1:$HUB_PORT" \
    --map "10.77.0.3=127.0.0.1:$HL_PORT" > "$RUN/relay.log" 2>&1 &
RELAY_PID=$!
sleep 1.5

FAILED=0
fail() { echo "FAIL: $*"; FAILED=1; }

node "$ROOT/test/v86/boot.mjs" \
    "wisp://localhost:$RELAY_PORT/" \
    "$RUN/boottest.img" \
    "$ROOT/web/images/dosypp.img" \
    "${1:-240}" "AUTOHUBL-DONE" "AUTOHUBL-DONE" \
    > "$RUN/guest.log" 2>&1
rc=$?

sed -n '/--- COM1/,$p' "$RUN/guest.log" | head -24 | sed 's/^/  /'

echo
echo "--- what the guest asked the relay for ---"
grep -aoE 'CONNECT [^ ]+:[0-9]+|refus[^ ]*.*' "$RUN/relay.log" |
    sort -u | sed 's/^/  /'

echo
[ "$rc" -eq 0 ] || fail "the guest did not finish its run"

grep -qa "GET /hublist.xml.bz2" "$RUN/web.log" ||
    fail "the guest never fetched the demo's hub list"

# The client reports the list on stdout under --fetch-hublist, which is
# the only way a rig reading a serial port can see it: the door itself
# only exists on a screen nobody is watching.
grep -qa "hublist: 1 hub(s) from http://10.77.0.3/hublist.xml.bz2" \
    "$RUN/guest.log" ||
    fail "the guest did not fetch one hub from the demo's own list
$(grep -a 'hublist:' "$RUN/guest.log" | sed 's/^/       /')"

grep -qa "The Attic" "$RUN/guest.log" ||
    fail "the hub in the list is not the demo's hub"

# The point of the whole rig: the guest asked for the hub and the list, and
# nothing else. te-home.net is the client's built-in default and must not
# appear; neither must any address outside the two mapped sentinels.
if grep -qai 'te-home\|hublist.org' "$RUN/relay.log" "$RUN/guest.log"; then
    fail "something reached for the public hub-list service -- the demo
       image's hublist setting is not taking effect"
fi

STRAY=$(grep -aoE 'CONNECT [0-9.]+:[0-9]+' "$RUN/relay.log" |
        awk '{print $2}' | cut -d: -f1 | sort -u |
        grep -v -E '^10\.77\.0\.(1|3)$' || true)
if [ -n "$STRAY" ]; then
    fail "the guest tried to reach addresses nobody mapped for it:
$(echo "$STRAY" | sed 's/^/       /')"
fi

if [ "$FAILED" -eq 0 ]; then
    echo "V86 HUBLIST OK: door 0 shows this demo's own hub, fetched from"
    echo "this demo's own list -- and the guest asked the relay for nothing"
    echo "but the two addresses it was given. Nobody else's hub was touched."
    exit 0
fi

echo "V86 HUBLIST FAILED (logs in $RUN)"
exit 1
