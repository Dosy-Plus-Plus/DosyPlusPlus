#!/bin/bash
# Stop anything a previous demo left running.
#
# Needed because an earlier version of demo.sh ended with `exec`, which
# replaces the shell -- and a replaced shell never runs its EXIT trap, so
# the hub and the chatter nodes outlived the demo and kept holding the port.
# Fixed there, but this stays: it is the thing you want when a demo is
# killed with SIGKILL, or the terminal window is closed outright.
#
#   ./tools/demo-stop.sh [port] [--force]
set -uo pipefail

PORT="${1:-1511}"
FORCE=0
for a in "$@"; do
    [ "$a" = "--force" ] && FORCE=1
done
found=0

# Refuse to pull the hub out from under a running DOS demo.
#
# This is not hypothetical: killing the hub while a DOSBox session was open
# is exactly how a working demo turned into "failed to connect" inside DOS,
# with nothing on the host to explain it. The DOS client has no reconnect,
# so it cannot recover.
if [ "$FORCE" != 1 ] && pgrep -f "dosbox-x.*dosbox-demo.conf" >/dev/null 2>&1; then
    echo "A DOS demo is still running:" >&2
    pgrep -fl "dosbox-x.*dosbox-demo.conf" 2>/dev/null | sed 's/^/  /' >&2
    cat >&2 <<'MSG'

Stopping the hub now would break that session: the DOS client would lose
its connection and cannot reconnect. Close the DOSBox-X window first (or
type EXIT at its DOS prompt), then run this again.

To stop everything anyway, including that session's hub:
  ./tools/demo-stop.sh --force
MSG
    exit 1
fi

kill_pattern() {
    local what="$1" pat="$2"
    local pids
    pids="$(pgrep -f "$pat" 2>/dev/null)" || return 0
    [ -n "$pids" ] || return 0
    echo "stopping $what: $(echo "$pids" | tr '\n' ' ')"
    # shellcheck disable=SC2086
    kill $pids 2>/dev/null
    found=1
}

# Scoped to our own command lines and the given port, so nothing unrelated
# on this machine is touched.
kill_pattern "stub hub"      "stubhub.py --port $PORT"
kill_pattern "demo clients"  "dosypp -p $PORT"
kill_pattern "chatter loops" "tools/demo.sh"
kill_pattern "chatter loops" "tools/demo-dos.sh"
kill_pattern "wisp relay"    "wisprelay.py"
[ "$FORCE" = 1 ] && kill_pattern "demo dosbox" "dosbox-x.*dosbox-demo.conf"

sleep 1

# Anything stubborn gets a second, firmer ask.
for pat in "stubhub.py --port $PORT" "dosypp -p $PORT"; do
    pids="$(pgrep -f "$pat" 2>/dev/null)" || true
    if [ -n "${pids:-}" ]; then
        echo "forcing: $pat"
        # shellcheck disable=SC2086
        kill -9 $pids 2>/dev/null
    fi
done

if command -v lsof >/dev/null 2>&1; then
    if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then
        echo
        echo "port $PORT is still held by something we did not start:"
        lsof -nP -iTCP:"$PORT" -sTCP:LISTEN
        echo
        echo "run the demo on another port instead:  DEMO_PORT=1512 make demo"
        exit 1
    fi
fi

[ "$found" = 1 ] && echo "done." || echo "nothing to stop."
echo "port $PORT is free."
