#!/bin/bash
# Run uhub (a real, independent ADC hub) and test DOSy++ against it.
#
# Why this exists: tools/stubhub.py is our own code. Agreeing with it proves
# we are self-consistent, not that we are correct. uhub enforces sequencing,
# recomputes our CID from the PID we send, and verifies the password hash in
# its own C implementation.
#
#   ./test/hub/run-uhub.sh            run the tests
#   ./test/hub/run-uhub.sh --serve    just run the hub and wait
#   UHUB_PORT=1511 ./test/hub/run-uhub.sh --serve   on a port you choose
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT/test/hub"
source "$ROOT/test/host/hublib.sh"
# A free port, not 1511. With 1511 taken by anything else that speaks ADC,
# the nc check below passes and every case runs against the wrong hub --
# which is how this rig once reported our password handling broken while
# talking to a stub hub that has no passwords.
PORT="${UHUB_PORT:-$(free_port)}"
NAME=dosypp-uhub-run
CLIENT="$ROOT/build/host/dosypp"

docker image inspect dosypp-uhub >/dev/null 2>&1 || {
    echo "image dosypp-uhub missing. Build it with:" >&2
    echo "  make uhub-image" >&2
    exit 1; }
[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }

cleanup() { docker rm -f "$NAME" >/dev/null 2>&1; }
trap cleanup EXIT
cleanup

echo "starting uhub..."
docker run -d --name "$NAME" -p "$PORT:1511" \
    -v "$HERE:/etc/uhub:ro" \
    dosypp-uhub uhub -c /etc/uhub/uhub.conf -v >/dev/null || exit 1

for _ in $(seq 40); do
    nc -z 127.0.0.1 "$PORT" 2>/dev/null && break
    sleep 0.25
done
nc -z 127.0.0.1 "$PORT" 2>/dev/null || {
    echo "uhub did not start. Log:" >&2
    docker logs "$NAME" 2>&1 | tail -30 >&2
    exit 1; }
echo "uhub listening on $PORT"

# nc proves something is listening, not that it is uhub. uhub.conf names
# the hub, and the client prints the name it was given, so one login says
# whose hub these cases are about to be run against.
WHO="$("$CLIENT" -p "$PORT" -n whois -c "$(mktemp -d)/who.ini" \
       --batch 127.0.0.1 2>&1)"
if ! grep -q "DOSyPP Conformance Hub" <<<"$WHO"; then
    echo "the hub on $PORT is not uhub -- it calls itself:" >&2
    grep -iE "logged in to|hub" <<<"$WHO" | head -3 >&2
    exit 1
fi
echo "and it is uhub: it calls itself DOSyPP Conformance Hub"
echo

if [ "${1:-}" = "--serve" ]; then
    echo "serving; ctrl-c to stop. Connect with:"
    echo "  ./build/host/dosypp -p $PORT -n warlord -w hunter2 127.0.0.1"
    docker logs -f "$NAME"
    exit 0
fi

fail=0
run_case() {
    local label="$1" expect="$2"; shift 2
    local out
    echo "--- $label"
    out="$("$@" 2>&1)"
    if grep -qiE "$expect" <<<"$out"; then
        echo "ok"
    else
        echo "FAIL (expected /$expect/)"
        sed 's/^/    /' <<<"$out" | tail -20
        fail=1
    fi
}

TMP="$(mktemp -d)"

# 1. Unregistered nick, no password: the baseline login.
run_case "unregistered login" "PHASE 2 OK" \
    "$CLIENT" -p "$PORT" -n nobody1 -c "$TMP/guest.ini" --batch 127.0.0.1

# 2. Registered nick with the right password: exercises IGPA/HPAS against
#    uhub's own Tiger. This is the test that matters most.
run_case "registered login with correct password" "PHASE 2 OK" \
    "$CLIENT" -p "$PORT" -n warlord -w hunter2 -c "$TMP/reg.ini" --batch 127.0.0.1

# 3. Registered nick with the wrong password: must be refused, and the
#    refusal must be reported as a password problem.
run_case "wrong password is refused" "wrong password|Invalid password|password" \
    "$CLIENT" -p "$PORT" -n warlord -w nope -c "$TMP/bad.ini" --batch 127.0.0.1

# 4. Registered nick with no password at all.
run_case "missing password is refused" "password" \
    "$CLIENT" -p "$PORT" -n warlord -c "$TMP/none.ini" --batch 127.0.0.1

# 5. Two clients at once: the second must see the first in its user list.
# --console: the UI is the default now, and a backgrounded client has no
# terminal to open a display on.
"$CLIENT" -p "$PORT" -n lurker --console -c "$TMP/a.ini" 127.0.0.1 \
    >"$TMP/a.log" 2>&1 &
BG=$!
sleep 2
run_case "second client sees the first" "lurker" \
    "$CLIENT" -p "$PORT" -n watcher -c "$TMP/b.ini" --batch 127.0.0.1
kill "$BG" 2>/dev/null

echo
echo "--- uhub's own log ---"
docker logs "$NAME" 2>&1 | grep -viE "^$" | tail -25

rm -rf "$TMP"
echo
if [ "$fail" -eq 0 ]; then
    echo "CONFORMANCE OK: DOSy++ works against uhub, an independent hub."
else
    echo "CONFORMANCE FAILED"
fi
exit $fail
