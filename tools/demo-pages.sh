#!/bin/bash
# Run the published demo, locally, with no GitHub involved.
#
#   make demo-pages                then open the URL it prints
#   ./tools/demo-pages.sh --detach  start it and give the shell back
#   ./tools/demo-pages.sh --stop
#
# Without --detach this stays in the foreground on purpose: three servers
# with nobody watching them is how you end up with a hub running for a
# week. It prints what it is doing and then follows the relay's log, which
# is the thing worth watching -- a CONNECT line means the guest got out.
#
# This serves build/pages -- the exact directory the Pages workflow
# uploads -- together with the three things a visitor's machine would be
# running: the hub, the node that really has the files, and the Wisp relay
# that gives a browser tab a TCP network.
#
# It is the rehearsal. Everything a stranger would see, on your own
# machine, before anybody is pointed at it.
#
# Nothing here can reach anybody else's hub. The relay is an allowlist: the
# guest can dial the three sentinel addresses mapped below and nothing
# else, and the demo image's hub-list door points at the one-entry list
# served here rather than at the public service the client defaults to.
# make test-v86-hublist checks both of those claims rather than asserting
# them.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE="${BUNDLE:-$ROOT/build/pages}"
RUNDIR="$ROOT/build/pages-demo"

PORT="${PAGES_PORT:-8086}"          # the relay, which also serves the page
HUB_PORT="${PAGES_HUB_PORT:-1511}"
ATTIC_PORT="${PAGES_ATTIC_PORT:-1512}"
HL_PORT="${PAGES_HL_PORT:-1513}"

DETACH=0
case "${1:-}" in
    --detach) DETACH=1 ;;
esac

if [ "${1:-}" = "--stop" ]; then
    pkill -f "wisprelay.py --port $PORT" 2>/dev/null
    pkill -f "stubhub.py --port $HUB_PORT" 2>/dev/null
    pkill -f "http.server -b 127.0.0.1 -d $RUNDIR" 2>/dev/null
    echo "stopped"
    exit 0
fi

die() { echo "error: $*" >&2; exit 1; }

[ -d "$BUNDLE" ] || die "no bundle at $BUNDLE
       run: make pages
       (this script deliberately does not build it for you: that pulls in
       the DOS binary, the images and the share, which is minutes of work
       and looks like a hang when all you wanted was to open a page)"
[ -f "$BUNDLE/index.html" ] || die "$BUNDLE has no index.html"

# A hub with nothing in it is a worse rehearsal than no rehearsal.
WAREZ="${WAREZ_DIR:-$ROOT/build/warez}"
SERVE=()
if [ -f "$WAREZ/SHARE.TXT" ]; then
    SERVE=(--serve "$WAREZ" --serve-ip 10.77.0.2 --serve-port "$ATTIC_PORT")
else
    echo "note: no share to serve (run: make warez). Searches will return"
    echo "      invented names that cannot be downloaded."
fi

for p in "$PORT" "$HUB_PORT" "$ATTIC_PORT" "$HL_PORT"; do
    if nc -z 127.0.0.1 "$p" 2>/dev/null; then
        die "something is already listening on 127.0.0.1:$p.
       Stop it, or set PAGES_PORT / PAGES_HUB_PORT / PAGES_ATTIC_PORT /
       PAGES_HL_PORT. This script will not touch a port it did not open."
    fi
done

rm -rf "$RUNDIR"; mkdir -p "$RUNDIR"

cleanup() {
    for p in ${HUB_PID:-} ${HL_PID:-} ${RELAY_PID:-}; do
        [ -n "$p" ] && { kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; }
    done
}
trap cleanup EXIT INT TERM

# --- the hub list this demo serves: one entry, its own hub ------------------
"$ROOT/tools/mkhublist.py" "$RUNDIR" \
    --address "adc://10.77.0.1:$HUB_PORT" >/dev/null

# --- the hub, and the node that really has the files -----------------------
"$ROOT/tools/stubhub.py" --port "$HUB_PORT" --motd-ansi "${SERVE[@]}" \
    > "$RUNDIR/hub.log" 2>&1 &
HUB_PID=$!

python3 -m http.server -b 127.0.0.1 -d "$RUNDIR" "$HL_PORT" \
    > "$RUNDIR/hublist.log" 2>&1 &
HL_PID=$!

for _ in $(seq 40); do
    nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null && break
    sleep 0.25
done
nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null || {
    echo "the hub did not start:" >&2; cat "$RUNDIR/hub.log" >&2; exit 1; }

# --- the relay, which also serves the bundle -------------------------------
#
# One origin for everything: the page and its WebSocket come from the same
# place, so there is no mixed content and no local-network permission to
# argue with -- which is exactly the fallback docs/PAGES.md recommends when
# a browser refuses ws:// from an https page.
"$ROOT/tools/wisprelay.py" --port "$PORT" --web "$BUNDLE" \
    --map "10.77.0.1=127.0.0.1:$HUB_PORT" \
    --map "10.77.0.2=127.0.0.1:$ATTIC_PORT" \
    --map "10.77.0.3=127.0.0.1:$HL_PORT" \
    > "$RUNDIR/relay.log" 2>&1 &
RELAY_PID=$!
sleep 1

nc -z 127.0.0.1 "$PORT" 2>/dev/null || {
    echo "the relay did not start:" >&2; cat "$RUNDIR/relay.log" >&2
    exit 1; }

URL="http://127.0.0.1:$PORT/"
DISK="one file, $(du -h "$BUNDLE/images/dosypp.img" 2>/dev/null | cut -f1)"
if [ -f "$BUNDLE/images/dosypp.img.json" ]; then
    DISK="$(python3 - "$BUNDLE/images/dosypp.img.json" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
print("%d compressed pieces, %.1fMB for a %dMB drive"
      % (m["parts"], m["bytes"] / 1048576, m["size"] / 1048576))
PY
)"
fi

cat <<MSG

  Open: $URL

  That is the published bundle, served from this machine. No GitHub, no
  account, nothing uploaded anywhere.

  Click Boot. Then: 3, type BOOM, ENTER to search, ENTER to queue, 4 to
  watch it arrive hash-checked. /quit, then PLAY BOOM at the DOS prompt --
  and press "Play it here" first if you want to hear Doom's music.

  Bundle: $BUNDLE ($(du -sh "$BUNDLE" | cut -f1))
  Disk:   $DISK
  Hub:    127.0.0.1:$HUB_PORT   files: 127.0.0.1:$ATTIC_PORT
  List:   one entry, this hub (door 0 reaches nothing else)
  Logs:   $RUNDIR/{hub,relay,hublist}.log

  The relay is an allowlist: the guest can dial 10.77.0.1, .2 and .3 and
  nothing else, so no hub on the internet sees any of this.

MSG

command -v open >/dev/null 2>&1 && open "$URL" 2>/dev/null || true

if [ "$DETACH" = "1" ]; then
    # The three servers keep running; this shell does not own them any
    # more, so the trap must not tear them down on the way out.
    trap - EXIT INT TERM
    echo "  Left running in the background. Stop it with:"
    echo "      ./tools/demo-pages.sh --stop"
    echo
    exit 0
fi

echo "  ---- running. Ctrl-C here stops all three. ----"
echo "  (./tools/demo-pages.sh --detach starts it and gives the shell back)"
echo

# Relay activity, because a CONNECT line is how you know the guest got out.
tail -f "$RUNDIR/relay.log"
