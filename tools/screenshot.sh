#!/bin/bash
# Render a door against a throwaway stub hub and print it.
#
# The UI needs a terminal, but --screenshot uses a null display that fills
# the cell buffer without touching one -- so this works over a pipe and in
# CI, which is what makes the layout checkable at all.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOOR="${1:-0}"
PORT=18110
TMP="$(mktemp -d)"
trap 'pkill -f "stubhub.py --port $PORT" 2>/dev/null; rm -rf "$TMP"' EXIT

[ -x "$ROOT/build/host/dosypp" ] || { echo "run: make host" >&2; exit 1; }

"$ROOT/tools/stubhub.py" --port "$PORT" >"$TMP/hub.log" 2>&1 &
sleep 1

# A few extra nodes, so the who list has something in it.
for n in Phantom Razor Acid; do
    "$ROOT/build/host/dosypp" -p "$PORT" -n "$n" --console \
        -c "$TMP/$n.ini" 127.0.0.1 >/dev/null 2>&1 &
    sleep 0.3
done
sleep 1

"$ROOT/build/host/dosypp" -p "$PORT" -n Warlord -c "$TMP/w.ini" \
    --screenshot "$TMP/shot.txt" --null-display --door "$DOOR" 127.0.0.1 >/dev/null 2>&1

pkill -f "dosypp -p $PORT" 2>/dev/null
cat "$TMP/shot.txt"
