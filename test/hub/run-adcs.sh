#!/bin/bash
# ADCS, end to end: can DOSy++ reach a TLS-only hub through a relay?
#
# docs/STUNNEL.md has said "put a relay in front of it" since Phase 0 and
# nobody had ever run it. This does: a real uhub with tls_enable=1, a relay
# in front, and the client logging in through the relay. Four things are
# checked, and the first two are what make the rest mean anything:
#
#   1. The hub really is TLS-only -- a plaintext dial at it must FAIL.
#      Without this the test would pass just as well with no TLS anywhere.
#   2. The relay really did negotiate TLS, by its own log of the cipher.
#   3. The client logs in through it, with a password, so the whole
#      IGPA/HPAS handshake crosses the TLS leg.
#   4. Certificate verification is on by default: against uhub's
#      self-signed certificate the relay must refuse, and --insecure is
#      what makes it work. A relay that silently trusted anything would
#      make the TLS leg decoration.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT/test/hub"
CLIENT="$ROOT/build/host/dosypp"
RELAY="$ROOT/tools/tlsrelay.py"
NAME=dosypp-uhub-tls
TMP="$(mktemp -d)"

free_port() {
    python3 - <<'PY'
import socket
s = socket.socket(); s.bind(("127.0.0.1", 0))
print(s.getsockname()[1]); s.close()
PY
}
HUB_PORT=${ADCS_HUB_PORT:-$(free_port)}
RELAY_PORT=${ADCS_RELAY_PORT:-$(free_port)}

ok=1
fail() { echo "FAIL: $*" >&2; ok=0; }

cleanup() {
    [ -n "${RELAY_PID:-}" ] && kill "$RELAY_PID" 2>/dev/null
    docker rm -f "$NAME" >/dev/null 2>&1
    if [ -n "${ADCS_KEEP:-}" ] || [ "$ok" != 1 ]; then
        echo "run dir kept: $TMP"
        return
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

docker image inspect dosypp-uhub >/dev/null 2>&1 || {
    echo "image dosypp-uhub missing. Build it with:" >&2
    echo "  make uhub-image" >&2
    exit 1; }
[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }

echo "starting uhub with TLS on $HUB_PORT..."
docker rm -f "$NAME" >/dev/null 2>&1
docker run -d --name "$NAME" -p "$HUB_PORT:1511" \
    -v "$HERE:/etc/uhub:ro" \
    dosypp-uhub uhub -c /etc/uhub/uhub-tls.conf -v >/dev/null || exit 1

for _ in $(seq 60); do
    nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null && break
    sleep 0.25
done
nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null || {
    echo "uhub did not start:" >&2
    docker logs "$NAME" 2>&1 | tail -30 >&2
    exit 1; }

# uhub has to actually be in TLS mode, or nothing below means anything.
docker logs "$NAME" 2>&1 | grep -qiE "tls|ssl" || {
    echo "uhub does not report TLS; its log:" >&2
    docker logs "$NAME" 2>&1 | tail -20 >&2
    exit 1; }
echo "uhub is up and reports TLS"
echo

# ---- 1. plaintext at a TLS hub must fail
echo "--- a plaintext dial at the TLS hub (must fail)"
PLAIN="$("$CLIENT" -p "$HUB_PORT" -n plainguy -c "$TMP/plain.ini" \
         --batch 127.0.0.1 2>&1)"
if grep -q "PHASE 2 OK" <<<"$PLAIN"; then
    fail "the hub accepted a plaintext login, so it is not TLS-only and this test proves nothing"
else
    echo "refused, as it should be: $(tail -2 <<<"$PLAIN" | head -1)"
fi
echo

# ---- 4. verification on by default: the relay must refuse a self-signed cert
echo "--- the relay with verification on (must refuse uhub's self-signed cert)"
python3 "$RELAY" --listen "127.0.0.1:$RELAY_PORT" \
    --connect "127.0.0.1:$HUB_PORT" > "$TMP/relay-verify.log" 2>&1 &
RELAY_PID=$!
sleep 1.5
"$CLIENT" -p "$RELAY_PORT" -n verifyguy -c "$TMP/verify.ini" \
    --batch 127.0.0.1 > "$TMP/verify.log" 2>&1
if grep -q "PHASE 2 OK" "$TMP/verify.log"; then
    fail "the relay accepted a self-signed certificate with verification on"
else
    echo "the relay refused it:"
    grep -iE "certificate|verify" "$TMP/relay-verify.log" | head -2 |
        sed 's/^/  /'
fi
kill "$RELAY_PID" 2>/dev/null
RELAY_PID=
echo

# ---- 2 and 3. --insecure relay, and a real login through it
echo "--- the relay with --insecure, and a login through it"
python3 "$RELAY" --listen "127.0.0.1:$RELAY_PORT" \
    --connect "127.0.0.1:$HUB_PORT" --insecure \
    > "$TMP/relay.log" 2>&1 &
RELAY_PID=$!
for _ in $(seq 40); do
    nc -z 127.0.0.1 "$RELAY_PORT" 2>/dev/null && break
    sleep 0.25
done
kill -0 "$RELAY_PID" 2>/dev/null || {
    echo "the relay died:" >&2; cat "$TMP/relay.log" >&2; exit 1; }

OUT="$("$CLIENT" -p "$RELAY_PORT" -n warlord -w hunter2 \
       -c "$TMP/adcs.ini" --batch 127.0.0.1 2>&1)"
echo "$OUT" | tail -6 | sed 's/^/  /'

grep -q "PHASE 2 OK" <<<"$OUT" ||
    fail "the client did not log in through the relay"

grep -q "TLS up:" "$TMP/relay.log" ||
    fail "the relay never reported a TLS cipher, so the hub leg was not TLS"
grep -qi "CERTIFICATES ARE NOT BEING VERIFIED" "$TMP/relay.log" ||
    fail "--insecure did not say so in the log"

echo
echo "--- what the relay saw ---"
grep -vi "^$" "$TMP/relay.log" | head -8 | sed 's/^/  /'
# Captured once, not queried three times: a `docker logs` that comes back
# empty -- a busy daemon, a container already being removed -- would read as
# "the hub never said that" and fail a check about TLS for a reason that has
# nothing to do with TLS. An empty capture is its own error.
docker logs "$NAME" > "$TMP/hub.log" 2>&1
[ -s "$TMP/hub.log" ] ||
    fail "docker logs $NAME came back empty, so the checks below would be meaningless"

echo "--- what the hub logged ---"
grep -iE "tls|cipher" "$TMP/hub.log" | tail -5 | sed 's/^/  /'

# From the hub's side: it accepted a TLS session, and it refused one for an
# unknown CA -- which is the verification check seen from the other end.
# (uhub does not log nicks at this verbosity; the login itself is proved by
# the user list coming back above.)
grep -qiE "accepted: TLS" "$TMP/hub.log" ||
    fail "the hub never reported accepting a TLS session"
grep -qi "unknown ca" "$TMP/hub.log" ||
    fail "the hub never refused the verifying relay, so that check passed for the wrong reason"

echo
if [ "$ok" = 1 ]; then
    echo "ADCS OK: a TLS-only hub refused plaintext, the relay negotiated"
    echo "TLS and verified by default, and DOSy++ logged in through it with"
    echo "a password -- no crypto in the client."
    exit 0
fi
echo "ADCS FAILED"
exit 1
