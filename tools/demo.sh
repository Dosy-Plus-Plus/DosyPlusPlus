#!/bin/bash
# Run DOSy++ interactively in your terminal, with a populated hub behind it.
#
# Starts a stub hub (with an ANSI welcome screen), logs a few other nodes in,
# gives them something to say, and then hands your terminal to the UI. Tears
# all of it down when you quit.
#
#   ./tools/demo.sh [theme]        theme: renegade | pcboard | amiga
#   ./tools/demo.sh --stop         stop a previous run and exit
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ "${1:-}" = "--stop" ]; then
    exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/demo-stop.sh" \
        "${DEMO_PORT:-1511}"
fi

THEME="${1:-renegade}"
PORT="${DEMO_PORT:-1511}"
TMP="$(mktemp -d)"

cleanup() {
    # Drop the sentinel first so the chatter loops stop on their own.
    rm -f "$TMP/running" 2>/dev/null

    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    for p in ${NODE_PIDS:-}; do kill "$p" 2>/dev/null; done

    # $! for a pipeline names the LAST command, so the feeding subshells are
    # not in NODE_PIDS. Kill our own children directly -- without this they
    # sit in a 7-second sleep before noticing the sentinel is gone.
    pkill -P $$ 2>/dev/null

    # Then sweep by our own port, which cannot match anything we did not
    # start.
    pkill -f "dosypp -p $PORT" 2>/dev/null
    pkill -f "stubhub.py --port $PORT" 2>/dev/null

    rm -rf "$TMP"
    printf '\n'
}
trap cleanup EXIT INT TERM

if [ ! -x "$ROOT/build/host/dosypp" ]; then
    echo "building..."
    make -C "$ROOT" host >/dev/null || exit 1
fi

if ! [ -t 0 ] || ! [ -t 1 ]; then
    cat >&2 <<'MSG'
This needs a real terminal: the UI puts it into raw mode and draws into the
alternate screen. Run it directly from a shell, not through a pipe.

For a look without a terminal, use:  make screenshot DOOR=0
MSG
    exit 1
fi

# Clear anything a previous demo left behind rather than refusing to start.
# Only our own processes on this port are touched.
if pgrep -f "stubhub.py --port $PORT" >/dev/null 2>&1 ||
   pgrep -f "dosypp -p $PORT" >/dev/null 2>&1; then
    echo "cleaning up a previous demo..."
    "$ROOT/tools/demo-stop.sh" "$PORT" || exit 1
fi

if nc -z 127.0.0.1 "$PORT" 2>/dev/null; then
    echo "port $PORT is held by something we did not start:" >&2
    lsof -nP -iTCP:"$PORT" -sTCP:LISTEN 2>/dev/null >&2
    echo >&2
    echo "use another port:  DEMO_PORT=1512 $0" >&2
    exit 1
fi

echo "starting a hub on $PORT with an ANSI welcome screen..."
"$ROOT/tools/stubhub.py" --port "$PORT" --motd-ansi >"$TMP/hub.log" 2>&1 &
HUB_PID=$!
sleep 1

touch "$TMP/running"

NODE_PIDS=""
add_node() {
    local nick="$1"; shift
    # Each extra node is a --console client fed from a loop, so it posts
    # every few seconds and the message base is not empty while you look at
    # it.
    (
        # The sentinel check means these loops exit when cleanup removes
        # $TMP, rather than lingering until their next write hits a dead
        # pipe.
        while [ -f "$TMP/running" ]; do
            for line in "$@"; do
                [ -f "$TMP/running" ] || break
                printf '%s\n' "$line"
                sleep 7
            done
        done
    ) | "$ROOT/build/host/dosypp" -p "$PORT" -n "$nick" --console \
            -c "$TMP/$nick.ini" 127.0.0.1 >/dev/null 2>&1 &
    NODE_PIDS="$NODE_PIDS $!"
    sleep 0.4
}

add_node Phantom \
    "anyone got the new Doom shareware?" \
    "/me adjusts the dual standard" \
    "ratio check: i uploaded 40 megs last week"
add_node Razor \
    "leeches will be removed, you know the rules" \
    "new in /incoming tonight" \
    "/me lights a cigarette"
add_node Acid \
    "this 14.4 is screaming" \
    "who still runs a 2400?" \
    "greetz to all the sysops"
add_node Mercury \
    "the attic never sleeps" \
    "/me checks the mail runner"

# A share worth pressing 5 for, when there is one. The demo is the one
# place where the atmosphere IS the product: a share door listing
# FREEDOOM.ZIP and OPENCP.ZIP says what this client is for, and an empty
# one says nothing.
ATMOS="${ATMOS_DIR:-$ROOT/build/atmos}"
if [ -d "$ATMOS" ]; then
    "$ROOT/tools/mkbigshare.py" "$TMP/DL" 120M --real "$ATMOS" \
        --quiet >/dev/null 2>&1 &&
        echo "share = $TMP/DL" >> "$TMP/you.ini"
fi

sleep 1
clear
cat <<MSG
DOSy++ demo -- theme: $THEME

  1 / 2 / 3 / 4   message base, who's online, welcome screen, node info
  TAB             cycle doors
  PGUP / PGDN     scroll (the message base and the welcome screen)
  UP / DOWN       move the lightbar in WHO'S ONLINE and scroll the welcome
  type + ENTER    post to the message base
  /me <action>    third person        /pm <handle> <text>   private
  5               your share (real DOS software, if make atmos has run)
  /theme amiga    switch skin live    /motd   welcome screen
  /quit  or  G    hang up

Four other nodes are logged in and will chat every few seconds.
Press ENTER to connect.
MSG
read -r _

# NOT exec: exec replaces this shell, and a replaced shell never runs its
# EXIT trap -- which left the hub and the chatter nodes alive after every
# demo, holding the port so the next run refused to start.
"$ROOT/build/host/dosypp" -p "$PORT" -n Warlord --theme "$THEME" \
    -c "$TMP/you.ini" 127.0.0.1

# Falls through to cleanup() via the EXIT trap.
