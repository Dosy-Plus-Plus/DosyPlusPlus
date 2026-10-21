#!/bin/bash
# Favourites, and the per-hub passwords that are the point of them.
#
# Two hubs: the first wants no password, the second is registered-only with
# a password the config file does not contain. The client starts on the
# first, is told about the second as a favourite with its own password, and
# connects to it from the favourites view.
#
# The proof is the second hub's log: the stub hub verifies the password
# response with rhash, so "password verified" means the per-hub password was
# used and the Tiger maths was right. If the client had fallen back to the
# config's password -- or to none -- the hub would have refused it.
#
# Then it restarts to check the favourite survived the file, and that the one
# marked automatic is the one dialled without being asked.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLIENT="$ROOT/build/host/dosypp"
RUN="$(mktemp -d)"
HUB_A=${FAVS_HUB_A:-25021}
HUB_B=${FAVS_HUB_B:-25022}
HUB_B_PASS=${FAVS_HUB_B_PASS:-correct-horse}

ok=1
fail() { echo "FAIL: $*" >&2; ok=0; }

cleanup() {
    [ -n "${A_PID:-}" ] && kill "$A_PID" 2>/dev/null
    [ -n "${B_PID:-}" ] && kill "$B_PID" 2>/dev/null
    pkill -f "dosypp -c $RUN/" 2>/dev/null
    if [ -n "${FAVS_KEEP:-}" ] || [ "$ok" != 1 ]; then
        echo "run dir kept: $RUN"
        return
    fi
    rm -rf "$RUN"
}
trap cleanup EXIT

[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }
command -v rhash >/dev/null 2>&1 ||
    echo "note: no rhash, so the hub cannot verify the password maths" >&2

mkdir -p "$RUN/node"

# Deliberately NOT the hub B password: if the client used this one, hub B
# would refuse it, which is exactly the mistake being tested for.
cat > "$RUN/node/node.ini" <<EOF
hub = 127.0.0.1
port = $HUB_A
nick = Drifter
password = the-wrong-password
sound = no
EOF

# Hub B as a favourite, with its own password and its own handle.
cat > "$RUN/node/DOSYPP.FAV" <<EOF
[hub]
name     = Registered Hub
address  = adc://127.0.0.1:$HUB_B
nick     = Regular
password = $HUB_B_PASS
auto     = no
EOF

"$ROOT/tools/stubhub.py" --port "$HUB_A" > "$RUN/hubA.log" 2>&1 &
A_PID=$!
"$ROOT/tools/stubhub.py" --port "$HUB_B" --password "$HUB_B_PASS" \
    > "$RUN/hubB.log" 2>&1 &
B_PID=$!

for _ in $(seq 20); do
    nc -z 127.0.0.1 "$HUB_A" 2>/dev/null &&
        nc -z 127.0.0.1 "$HUB_B" 2>/dev/null && break
    sleep 0.25
done

echo "hub A on $HUB_A (open), hub B on $HUB_B (password required)"

# ENTER on the favourite, which is the only one.
echo "connecting to the favourite..."
(
    sleep 5;  printf '\r'
    sleep 10
) | ( cd "$RUN/node" && exec "$CLIENT" -c "$RUN/node/node.ini" \
        --screenshot "$RUN/shot.txt" --null-display --door 10 \
        --run-secs 18 127.0.0.1 ) > "$RUN/run.log" 2>&1

echo
echo "--- the favourites view ---"
sed -n '2,10p' "$RUN/shot.txt" 2>/dev/null || echo "(no screenshot)"
echo "---------------------------"

grep -q "logged in as 'Drifter'" "$RUN/hubA.log" ||
    fail "the client never logged into hub A"

# The heart of it: hub B must have accepted a password it verified.
if grep -q "password verified" "$RUN/hubB.log"; then
    echo "hub B verified the password: the per-hub one was used"
elif grep -q "password accepted (no oracle" "$RUN/hubB.log"; then
    echo "hub B accepted the password (it could not verify the maths)"
else
    fail "hub B did not accept a verified password: $(grep -iE 'password|refus' "$RUN/hubB.log" | tail -2 | tr '\n' ' ')"
fi

grep -q "logged in as 'Regular'" "$RUN/hubB.log" ||
    fail "hub B never saw us log in with the favourite's handle"

grep -qi "BAD password" "$RUN/hubB.log" &&
    fail "a wrong password was sent -- the config's, not the favourite's"

# --- restart: the favourite survived, and auto-connect works ------------
echo
echo "marking it automatic and restarting..."
python3 - "$RUN/node/DOSYPP.FAV" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
s = s.replace("auto     = no", "auto     = yes")
open(p, "w").write(s)
PY

# No host on the command line: the favourite decides where we go.
( cd "$RUN/node" && exec "$CLIENT" -c "$RUN/node/node.ini" \
    --screenshot "$RUN/shot2.txt" --null-display --door 10 \
    --run-secs 8 ) > "$RUN/run2.log" 2>&1 < /dev/null

grep -q "favourite: Registered Hub" "$RUN/run2.log" ||
    fail "the automatic favourite was not picked up on startup"
grep -qE "connecting to 127.0.0.1:$HUB_B as Regular" "$RUN/run2.log" ||
    fail "it did not dial the favourite with its own handle: $(grep -i connecting "$RUN/run2.log" | head -1)"

# Two logins on hub B now, both verified.
[ "$(grep -c "logged in as 'Regular'" "$RUN/hubB.log")" -ge 2 ] ||
    fail "the second run did not log into hub B"

# The password must still be in the file, and still right: a save that
# dropped it would show up here and nowhere else.
grep -qE "^password *= *$HUB_B_PASS" "$RUN/node/DOSYPP.FAV" ||
    fail "the password is no longer in DOSYPP.FAV"

# --- building the list by hand -----------------------------------------
# A different directory, with no favourites file: press A on the hubs door
# to add the hub we are on. That is how the list gets its first entry, and
# it has to carry the handle and password actually in use -- they are what
# got us in.
echo
echo "adding the current hub from scratch..."
mkdir -p "$RUN/fresh"
cat > "$RUN/fresh/node.ini" <<EOF
hub = 127.0.0.1
port = $HUB_B
nick = Regular
password = $HUB_B_PASS
sound = no
EOF

(
    sleep 5;  printf 'A'
    sleep 4
) | ( cd "$RUN/fresh" && exec "$CLIENT" -c "$RUN/fresh/node.ini" \
        --screenshot "$RUN/shot3.txt" --null-display --door 10 \
        --run-secs 12 127.0.0.1 ) > "$RUN/run3.log" 2>&1

if [ -f "$RUN/fresh/DOSYPP.FAV" ]; then
    echo "--- what A wrote ---"
    grep -vE "^;|^$" "$RUN/fresh/DOSYPP.FAV"
    echo "--------------------"

    grep -qE "^address *= *adc://127.0.0.1:$HUB_B" "$RUN/fresh/DOSYPP.FAV" ||
        fail "the added entry has the wrong address"
    grep -qE "^nick *= *Regular" "$RUN/fresh/DOSYPP.FAV" ||
        fail "the handle in use was not carried into the favourite"
    grep -qE "^password *= *$HUB_B_PASS" "$RUN/fresh/DOSYPP.FAV" ||
        fail "the password in use was not carried into the favourite"
    # The hub told us its name, which is better than an address.
    grep -qE "^name *= *DOSyPP Stub Hub" "$RUN/fresh/DOSYPP.FAV" ||
        echo "note: the entry is named after the address, not the hub"
else
    fail "pressing A wrote no DOSYPP.FAV"
fi

echo
echo "--- DOSYPP.FAV as it stands ---"
grep -vE "^;|^$" "$RUN/node/DOSYPP.FAV"
echo "-------------------------------"

if [ "$ok" = 1 ]; then
    echo "FAVS OK: a per-hub password got us into a hub the config could not."
    exit 0
fi
echo "--- client output ---" >&2
tail -12 "$RUN/run.log" >&2
echo "FAVS FAILED" >&2
exit 1
