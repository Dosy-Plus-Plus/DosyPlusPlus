#!/bin/bash
# DOSy++ in a browser tab: the real DJGPP binary on real DOS, in v86.
#
# Starts a stub hub with an ANSI welcome screen, a few chattering nodes, and
# the Wisp relay -- which also serves the page, because v86 wants
# SharedArrayBuffer and that needs COOP/COEP headers.
#
#   ./tools/demo-web.sh            then open the URL it prints
#   ./tools/demo-web.sh --stop
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "${1:-}" = "--stop" ]; then
    pkill -f "wisprelay.py" 2>/dev/null
    exec "$ROOT/tools/demo-stop.sh" "${WEB_HUB_PORT:-1511}" --force
fi

RELAY_PORT="${WEB_PORT:-8086}"
HUB_PORT="${WEB_HUB_PORT:-1511}"
ATTIC_PORT="${WEB_ATTIC_PORT:-1512}"
WAREZ="${WAREZ_DIR:-$ROOT/build/warez}"
RUNDIR="$ROOT/build/web-demo"

cleanup() {
    rm -f "$RUNDIR/running" 2>/dev/null
    pkill -P $$ 2>/dev/null
    [ -n "${RELAY_PID:-}" ] && kill "$RELAY_PID" 2>/dev/null
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    pkill -f "dosypp -p $HUB_PORT" 2>/dev/null
    pkill -f "stubhub.py --port $HUB_PORT" 2>/dev/null
    pkill -f "wisprelay.py --port $RELAY_PORT" 2>/dev/null
}
trap cleanup EXIT INT TERM

# --- the pieces the browser needs ---------------------------------------

[ -f "$ROOT/build/dos/DOSYPP.EXE" ] || {
    echo "No DOS binary yet. Build it first:" >&2
    echo "  make dos-docker" >&2
    exit 1; }

[ -f "$ROOT/dosbox-x/c/PCIPKT.COM" ] || {
    echo "Missing the RTL8029 packet driver that v86's NIC needs:" >&2
    echo "  make pktdrv" >&2
    exit 1; }

[ -f "$ROOT/web/vendor/v86.wasm" ] || {
    echo "fetching the v86 engine..."
    "$ROOT/tools/fetchv86.sh" >/dev/null || exit 1; }

# Always rebuild the images: the whole point is to run the binary you just
# built, and a stale image is a confusing way to discover otherwise.
echo "building the disk images..."
cp "$ROOT/build/dos/DOSYPP.EXE" "$ROOT/dosbox-x/c/" 2>/dev/null
"$ROOT/tools/mkv86img.sh" >/dev/null || exit 1

mkdir -p "$RUNDIR"
touch "$RUNDIR/running"

# The page's footer reads this. The published bundle gets one from
# tools/mkpages.sh; locally there would be none, and the footer would
# quietly say nothing about which build you are looking at.
cat > "$ROOT/web/build.json" <<JSON
{
  "built": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "rev": "$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo local)",
  "v86": "$(sed -n 's/^v86 engine: *//p' "$ROOT/web/vendor/VERSIONS.txt" \
            2>/dev/null | head -1)",
  "disk_bytes": $(wc -c < "$ROOT/web/images/dosypp.img" | tr -d ' ')
}
JSON

# --- hub, chatter, relay -------------------------------------------------

if pgrep -f "stubhub.py --port $HUB_PORT" >/dev/null 2>&1 ||
   pgrep -f "wisprelay.py --port $RELAY_PORT" >/dev/null 2>&1; then
    echo "cleaning up a previous web demo..."
    pkill -f "wisprelay.py --port $RELAY_PORT" 2>/dev/null
    "$ROOT/tools/demo-stop.sh" "$HUB_PORT" >/dev/null 2>&1
    sleep 1
fi

# The share the hub really serves, so the demo can be followed all the way
# to playing something. Without it the file area is invented names that
# cannot be downloaded, which is a worse demo than no file area.
SERVE_ARGS=()
if [ -f "$WAREZ/SHARE.TXT" ]; then
    SERVE_ARGS=(--serve "$WAREZ" --serve-ip 10.77.0.2
                --serve-port "$ATTIC_PORT")
else
    echo "note: no real share to serve (run: make warez)."
    echo "      searches will return invented names that cannot be"
    echo "      downloaded."
fi

echo "starting the hub..."
"$ROOT/tools/stubhub.py" --port "$HUB_PORT" --motd-ansi \
    "${SERVE_ARGS[@]}" \
    >"$RUNDIR/hub.log" 2>&1 &
HUB_PID=$!
for _ in $(seq 20); do
    nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null && break
    sleep 0.25
done
nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null || {
    echo "the hub did not start:" >&2; cat "$RUNDIR/hub.log" >&2; exit 1; }

for n in Phantom Razor Acid; do
    (
        while [ -f "$RUNDIR/running" ]; do
            echo "greetz from $n"
            sleep 8
        done
    ) | "$ROOT/build/host/dosypp" -p "$HUB_PORT" -n "$n" --console \
            -c "$RUNDIR/$n.ini" 127.0.0.1 >/dev/null 2>&1 &
    sleep 0.3
done

echo "starting the Wisp relay (also serves the page)..."
"$ROOT/tools/wisprelay.py" --port "$RELAY_PORT" \
    --map "10.77.0.1=127.0.0.1:$HUB_PORT" \
    --map "10.77.0.2=127.0.0.1:$ATTIC_PORT" >"$RUNDIR/relay.log" 2>&1 &
RELAY_PID=$!
sleep 1
nc -z 127.0.0.1 "$RELAY_PORT" 2>/dev/null || {
    echo "the relay did not start:" >&2; cat "$RUNDIR/relay.log" >&2; exit 1; }

URL="http://127.0.0.1:$RELAY_PORT/"

cat <<MSG

  Open: $URL

  Click Boot. FreeDOS comes up, loads the RTL8029 packet driver, and starts
  DOSy++ against the relay -- which forwards to the hub on this machine.

  Try the whole thing: press 3, type BLKDROP, ENTER to search, ENTER on
  the result to queue it, 4 to watch it arrive. Then /quit and, at the
  DOS prompt, PLAY BLKDROP.

  Keys: 1-4 doors, TAB cycle, PGUP/PGDN scroll, type + ENTER to post,
        /quit or G to hang up.

  Click into the emulator before typing, or the keys go to the browser.

  Logs: $RUNDIR/{hub,relay}.log
  Ctrl-C here when you are done.

MSG

command -v open >/dev/null 2>&1 && open "$URL" 2>/dev/null || true

# Stay alive so the trap owns the teardown, and surface relay activity --
# a CONNECT line means the guest reached the hub.
tail -f "$RUNDIR/relay.log"
