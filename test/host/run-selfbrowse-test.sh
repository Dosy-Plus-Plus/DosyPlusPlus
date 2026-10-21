#!/bin/bash
# Browsing your own share.
#
# It used to do nothing at all. Pressing B on your own node queued a file
# list request addressed to your own SID: the queue asked the hub to put
# you in touch with yourself, the hub routed the RCM back, and a passive
# client cannot answer one -- so the door sat on "NOBODY'S FILES ARE OPEN"
# with no error, no timeout and nothing in the log. It is also the one
# file list this program never has to ask anybody for.
#
# Two things are checked, because they are two different code paths:
#
#   the root        the list the browse door opens with
#   a directory     the one-level-deeper fetch, which for a peer is a
#                   second request over the wire
#
# And one timing case that produced the same empty door while the fix was
# being written: a share that has not finished being scanned has nothing
# to list, so the request waits for the scan rather than failing.
set -uo pipefail
exec < /dev/null

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN="$ROOT/build/selfbrowse"
CLIENT="$ROOT/build/host/dosypp"
HUB_PORT=${SELFBROWSE_HUB_PORT:-11631}

[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }

rm -rf "$RUN"; mkdir -p "$RUN/share/warez" "$RUN/share/mods"
printf 'hello\n' > "$RUN/share/warez/ALPHA.TXT"
printf 'world\n' > "$RUN/share/warez/BETA.TXT"
printf 'tune\n'  > "$RUN/share/mods/SONG.MOD"

cleanup() { [ -n "${HUB:-}" ] && { kill "$HUB" 2>/dev/null; wait "$HUB" 2>/dev/null; }; }
trap cleanup EXIT

"$ROOT/tools/stubhub.py" --port "$HUB_PORT" --no-fake-share \
    > "$RUN/hub.log" 2>&1 &
HUB=$!
for _ in $(seq 40); do
    nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null && break
    sleep 0.25
done

cat > "$RUN/c.ini" <<EOF
hub    = 127.0.0.1
port   = $HUB_PORT
nick   = SelfBrowse
share  = $RUN/share
active = no
music  = no
sound  = no
EOF

FAILED=0
fail() { echo "FAIL: $*"; FAILED=1; }

run_browse() {   # $1 = screenshot, $2 = optional directory
    local shot="$1" dir="${2:-}"
    local args=(-c "$RUN/c.ini" --null-display --screenshot "$shot"
                --door 8 --browse SelfBrowse --run-secs 12)

    [ -n "$dir" ] && args+=(--browse-dir "$dir")
    ( cd "$RUN" && perl -e 'alarm 40; exec @ARGV' "$CLIENT" "${args[@]}" \
        127.0.0.1 ) > "$RUN/$(basename "$shot").log" 2>&1
}

echo "--- the root of your own share"
run_browse "$RUN/root.txt"
sed -n '2,6p' "$RUN/root.txt" | sed 's/^/  /'

grep -qa "BROWSING SelfBrowse" "$RUN/root.txt" ||
    fail "the browse door did not open on our own node"
grep -qa "NOBODY'S FILES ARE OPEN" "$RUN/root.txt" &&
    fail "the door is empty -- browsing yourself did nothing"
grep -qa "share" "$RUN/root.txt" ||
    fail "the share's root directory is not listed"

echo
echo "--- one directory down"
run_browse "$RUN/dir.txt" "/share/warez/"
sed -n '2,7p' "$RUN/dir.txt" | sed 's/^/  /'

grep -qa "ALPHA.TXT" "$RUN/dir.txt" ||
    fail "ALPHA.TXT is missing from our own warez directory"
grep -qa "BETA.TXT" "$RUN/dir.txt" ||
    fail "BETA.TXT is missing from our own warez directory"
grep -qa "SONG.MOD" "$RUN/dir.txt" &&
    fail "a file from another directory is listed: this is not a listing \
of /share/warez/ but of everything"

# Nothing should have gone to the hub about it. A file list fetched over
# the wire would mean an RCM, and an RCM addressed to ourselves is the bug
# this rig exists for.
if grep -qa "DRCM" "$RUN/hub.log"; then
    fail "the client asked the hub to connect it to itself"
fi

# Nothing drawn on either screen may be wider than the screen. The
# transfers door's footer used to run past the panel and overwrite its
# right border, and a failed row's reason was cut mid-word at it.
echo
echo "--- every line fits in 80 columns"
python3 - "$RUN/root.txt" "$RUN/dir.txt" <<'PY'
import sys

bad = 0
for name in sys.argv[1:]:
    for i, line in enumerate(open(name, encoding="utf-8", errors="replace"), 1):
        n = len(line.rstrip("\n"))
        if n > 80:
            print(f"  {name}:{i}: {n} characters")
            bad += 1
if bad:
    print(f"FAIL: {bad} line(s) wider than the screen")
    sys.exit(1)
print("  ok")
PY
[ $? -eq 0 ] || fail "something drew past the right-hand edge"

echo
if [ "$FAILED" -eq 0 ]; then
    echo "SELF-BROWSE OK: your own share opens from the share index, at the"
    echo "root and one level down, without a word to the hub."
    exit 0
fi

echo "SELF-BROWSE FAILED (screens and logs in $RUN)"
exit 1
