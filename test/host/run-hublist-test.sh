#!/bin/bash
# Fetch a hub list over HTTP and connect to a hub from it.
#
# The whole path: HTTP GET -> bzip2 -> XML -> a list on screen -> ENTER ->
# the session moves to a different hub. Every step is somewhere a client can
# quietly do nothing, and the only proof that it worked is the *second* hub
# seeing us log in.
#
# Two hubs run: the client starts on the first and should end up on the
# second, which is the only one in the list.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLIENT="$ROOT/build/host/dosypp"
RUN="$(mktemp -d)"
HUB_A=${HUBLIST_HUB_A:-25001}
HUB_B=${HUBLIST_HUB_B:-25002}
WEB_PORT=${HUBLIST_WEB_PORT:-25003}

ok=1
fail() { echo "FAIL: $*" >&2; ok=0; }

cleanup() {
    [ -n "${A_PID:-}" ] && kill "$A_PID" 2>/dev/null
    [ -n "${B_PID:-}" ] && kill "$B_PID" 2>/dev/null
    [ -n "${WEB_PID:-}" ] && kill "$WEB_PID" 2>/dev/null
    pkill -f "dosypp -c $RUN/" 2>/dev/null
    if [ -n "${HUBLIST_KEEP:-}" ] || [ "$ok" != 1 ]; then
        echo "run dir kept: $RUN"
        return
    fi
    rm -rf "$RUN"
}
trap cleanup EXIT

[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }

mkdir -p "$RUN/web" "$RUN/node"

# A list with one usable hub and two the client must refuse: an NMDC hub and
# a TLS one. Compressed, because that is how real lists are served -- and
# compressed by Python rather than by us, so the decompressor is being
# tested against somebody else's bzip2.
python3 - "$RUN/web/hublist.xml.bz2" "$HUB_B" <<'PY'
import bz2, sys
out, port = sys.argv[1], int(sys.argv[2])
xml = f'''<?xml version="1.0" encoding="utf-8"?>
<Hublist>
  <Hubs>
    <Hub Name="The Second Hub" Address="adc://127.0.0.1:{port}"
         Description="where we should end up" Users="42"
         Shared="10995116277760"/>
    <Hub Name="Old Protocol" Address="dchub://nmdc.example" Users="999"/>
    <Hub Name="Encrypted" Address="adcs://secure.example:2781" Users="998"/>
  </Hubs>
</Hublist>
'''
with open(out, "wb") as f:
    f.write(bz2.compress(xml.encode("utf-8")))
print("wrote a bzip2 hub list")
PY

cat > "$RUN/node/node.ini" <<EOF
hub = 127.0.0.1
port = $HUB_A
nick = Wanderer
sound = no
hublist = http://127.0.0.1:$WEB_PORT/hublist.xml.bz2
EOF

( cd "$RUN/web" && exec python3 -m http.server "$WEB_PORT" --bind 127.0.0.1 ) \
    > "$RUN/web.log" 2>&1 &
WEB_PID=$!

"$ROOT/tools/stubhub.py" --port "$HUB_A" > "$RUN/hubA.log" 2>&1 &
A_PID=$!
"$ROOT/tools/stubhub.py" --port "$HUB_B" > "$RUN/hubB.log" 2>&1 &
B_PID=$!

for _ in $(seq 20); do
    nc -z 127.0.0.1 "$HUB_A" 2>/dev/null &&
        nc -z 127.0.0.1 "$HUB_B" 2>/dev/null &&
        nc -z 127.0.0.1 "$WEB_PORT" 2>/dev/null && break
    sleep 0.25
done

echo "hubs on $HUB_A and $HUB_B, list served on $WEB_PORT"

# F fetches the list; ENTER connects to the highlighted hub, which is the
# only usable one in it.
echo "fetching the list and switching hubs..."
(
    sleep 5;  printf 'F'
    sleep 6;  printf '\r'
    sleep 10
) | ( cd "$RUN/node" && exec "$CLIENT" -c "$RUN/node/node.ini" \
        --screenshot "$RUN/shot.txt" --null-display --door 10 \
        --run-secs 24 127.0.0.1 ) > "$RUN/run.log" 2>&1

echo
echo "--- the hub list door ---"
sed -n '2,12p' "$RUN/shot.txt" 2>/dev/null || echo "(no screenshot)"
echo "-------------------------"

# The fetch itself.
grep -q "GET /hublist.xml.bz2" "$RUN/web.log" ||
    fail "the client never asked the web server for the list"
grep -qE '"GET /hublist.xml.bz2 HTTP/1.[01]" 200' "$RUN/web.log" ||
    fail "the web server did not serve the list successfully"

# Parsed, with the unusable entries left out.
grep -q "The Second Hub" "$RUN/shot.txt" 2>/dev/null ||
    fail "the list does not show the hub it contains"
grep -q "Old Protocol" "$RUN/shot.txt" 2>/dev/null &&
    fail "an NMDC hub was offered, which this client cannot speak to"
grep -q "Encrypted" "$RUN/shot.txt" 2>/dev/null &&
    fail "an ADCS hub was offered, and there is no TLS in this client"

# The point of the exercise: the second hub saw us.
grep -q "logged in as 'Wanderer'" "$RUN/hubA.log" ||
    fail "the client never logged into the first hub"
if grep -q "logged in as 'Wanderer'" "$RUN/hubB.log"; then
    echo "the second hub saw the login, so the switch worked"
else
    fail "the second hub never saw us: the switch did not happen"
fi

# And it said goodbye to the first rather than just vanishing.
grep -qE "HDSC|client disconnect" "$RUN/hubA.log" ||
    echo "note: no clean disconnect recorded on the first hub"

echo
if [ "$ok" = 1 ]; then
    echo "HUBLIST OK: fetched over HTTP, decompressed, parsed, and connected."
    exit 0
fi
echo "--- client output ---" >&2
tail -15 "$RUN/run.log" >&2
echo "HUBLIST FAILED" >&2
exit 1
