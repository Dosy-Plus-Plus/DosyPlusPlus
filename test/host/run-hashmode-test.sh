#!/bin/bash
# Deferred and pausable hashing, end to end.
#
# Hashing a real share is half an hour of solid disk work, so "hash = manual"
# has to mean the disk really stays quiet -- not "hashes a bit less eagerly".
# That is checked here by looking for the absence of work: no DOSYPP.TTH, and
# a hub that was never told a non-zero share size.
#
# The resume half works by feeding a keystroke to the client's stdin, which
# the terminal front end reads exactly as it would a key from a keyboard.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLIENT="$ROOT/build/host/dosypp"
HUB_PORT=${HASHMODE_HUB_PORT:-24801}
RUN="$(mktemp -d)"

cleanup() {
    [ -n "${HUB:-}" ] && kill "$HUB" 2>/dev/null
    pkill -f "dosypp -c $RUN/" 2>/dev/null
    rm -rf "$RUN"
}
trap cleanup EXIT

[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }

mkdir -p "$RUN/share/warez"
python3 - "$RUN/share/warez" <<'PY'
import os, random, sys
random.seed(1994)
for name, size in [("MAZERUN2.ZIP", 90_000), ("ARCPACK2.EXE", 40_000),
                   ("README.TXT", 3_000)]:
    with open(os.path.join(sys.argv[1], name), "wb") as f:
        f.write(bytes(random.getrandbits(8) for _ in range(size)))
PY

"$ROOT/tools/stubhub.py" --port "$HUB_PORT" --no-fake-share \
    > "$RUN/hub.log" 2>&1 &
HUB=$!
for _ in $(seq 20); do
    nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null && break
    sleep 0.25
done

# One run of the client in its own directory, so each case starts without a
# hash cache. $1 is the hash mode, $2 a nick, $3 keystrokes to feed (after a
# pause), $4 how long to run.
run_case() {
    local mode=$1 nick=$2 keys=$3 secs=$4
    local dir="$RUN/$nick"

    mkdir -p "$dir"
    cat > "$dir/dosypp.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = $nick
sound = no
hash = $mode
share = $RUN/share
EOF

    (
        cd "$dir" || exit 1
        if [ -n "$keys" ]; then
            ( sleep 3; printf '%s' "$keys" ) |
                "$CLIENT" -c "$dir/dosypp.ini" --screenshot "$dir/shot.txt" \
                    --null-display --door 5 --run-secs "$secs" 127.0.0.1
        else
            "$CLIENT" -c "$dir/dosypp.ini" --screenshot "$dir/shot.txt" \
                --null-display --door 5 --run-secs "$secs" \
                127.0.0.1 < /dev/null
        fi
    ) > "$dir/run.log" 2>&1
}

# What the hub was told about this nick's share, which is the only evidence
# that does not come from the client's own mouth.
hub_told_share() {
    grep -A99 "logged in as '$1'" "$RUN/hub.log" 2>/dev/null |
        grep -cE "BINF .* SS[1-9]"
}

ok=1
# Collected as well as printed: the sweep shows only the tail of a log,
# and these lines are often a screen or two above it. The list is
# reprinted immediately before the verdict, so a red run in CI says which
# check failed instead of only that something did.
FAILURES=""
fail() {
    echo "FAIL: $*" >&2
    FAILURES="$FAILURES
  - $*"
    ok=0
}

echo "--- hash = manual, nobody presses anything ---"
run_case manual Deferred "" 8
sed -n '2,14p' "$RUN/Deferred/shot.txt" 2>/dev/null || echo "(no screenshot)"

grep -q "PAUSED" "$RUN/Deferred/shot.txt" 2>/dev/null ||
    fail "the share door does not show PAUSED"
grep -qi "press H" "$RUN/Deferred/shot.txt" 2>/dev/null ||
    fail "the share door does not say how to start hashing"
[ -f "$RUN/Deferred/DOSYPP.TTH" ] &&
    fail "it hashed anyway: DOSYPP.TTH was written"
[ "$(hub_told_share Deferred)" = "0" ] ||
    fail "it hashed anyway: the hub was told a non-zero share"
grep -q "3 files" "$RUN/Deferred/run.log" "$RUN/Deferred/shot.txt" 2>/dev/null ||
    echo "note: the scan count did not appear; the scan still runs in manual mode"

echo
echo "--- hash = manual, then H ---"
run_case manual Resumed h 12
sed -n '2,14p' "$RUN/Resumed/shot.txt" 2>/dev/null || echo "(no screenshot)"

[ -f "$RUN/Resumed/DOSYPP.TTH" ] ||
    fail "H did not start hashing: no DOSYPP.TTH"
[ "$(hub_told_share Resumed)" -ge 1 ] 2>/dev/null ||
    fail "H hashed but the hub was never told the new share size"
[ -f "$RUN/Resumed/FILES.XML" ] ||
    fail "H hashed but no file list was written"

echo
echo "--- hash = auto (the default) ---"
run_case auto Eager "" 12
[ -f "$RUN/Eager/DOSYPP.TTH" ] ||
    fail "auto mode did not hash"
[ "$(hub_told_share Eager)" -ge 1 ] 2>/dev/null ||
    fail "auto mode hashed but the hub was never told"

echo
if [ "$ok" = 1 ]; then
    echo "HASH MODE OK: manual defers and stays quiet, H starts it, auto still hashes."
    exit 0
fi
echo "--- what failed ---$FAILURES" >&2
echo "HASH MODE FAILED" >&2
exit 1
