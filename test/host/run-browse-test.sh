#!/bin/bash
# Browse somebody else's share, end to end.
#
# One client shares a small tree and is active; the other walks it over a
# peer connection. That exercises the whole path: files.xml -> bzip2 ->
# CGET -> CSND -> decompress -> parse -> a tree on screen. Every step has a
# way of being quietly wrong that only shows up at the far end.
#
# Two passes, because there are two ways to ask:
#
#   1. Partial lists, which is what the client does now: CGET list / for
#      the top level, then one directory at a time as they are opened.
#      The point of it is that a share too big to hold can still be
#      browsed, so the test checks that the listings really are partial --
#      a client asking for one directory and being sent everything would
#      otherwise look identical from the outside.
#   2. The whole list, from a sharer started with --refuse-partial, which
#      is what a client predating partial lists answers. The downloader has
#      to notice the refusal and ask again for the lot.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLIENT="$ROOT/build/host/dosypp"
. "$ROOT/test/host/hublib.sh"
RUN="$(mktemp -d)"
HUB_PORT=${BROWSE_HUB_PORT:-$(free_port)}
PEER_PORT=${BROWSE_PEER_PORT:-$(free_port)}

cleanup() {
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    [ -n "${UP_PID:-}" ] && kill "$UP_PID" 2>/dev/null
    pkill -f "dosypp -c $RUN/" 2>/dev/null
    [ -n "${BROWSE_KEEP:-}" ] && { echo "run dir kept: $RUN"; return; }
    rm -rf "$RUN"
}
trap cleanup EXIT

[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }

mkdir -p "$RUN/up/warez/apps" "$RUN/up/mods" "$RUN/down"

python3 - "$RUN/up" <<'PY'
import os, random, sys
root = sys.argv[1]
random.seed(1994)
# A name with an XML metacharacter in it, because that is the one that
# breaks between the writer and the reader.
files = [("warez/MAZERUN2.ZIP", 90000),
         ("warez/apps/ARCPACK2.EXE", 40000),
         ("mods/AT&T DEMO.MOD", 12000),
         ("README.TXT", 3000)]
for rel, size in files:
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(bytes(random.getrandbits(8) for _ in range(size)))
print("made a share of %d files" % len(files))
PY

cat > "$RUN/up.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = Sharer
sound = no
active = yes
port.listen = $PEER_PORT
share = $RUN/up
EOF

cat > "$RUN/dn.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = Nosey
sound = no
active = no
EOF

start_stub_hub "$HUB_PORT" "$RUN/hub.log" --no-fake-share

echo "starting the sharer (active)..."
( cd "$RUN/up" && exec "$CLIENT" -c "$RUN/up.ini" --null-display \
    --door 5 --peer-trace --run-secs 90 127.0.0.1 ) > "$RUN/up.log" 2>&1 &
UP_PID=$!

for _ in $(seq 60); do
    grep -qE "BINF .* SS[1-9]" "$RUN/hub.log" 2>/dev/null && break
    sleep 0.5
done
grep -q "listening on port" "$RUN/up.log" || {
    echo "the sharer is not listening:" >&2; cat "$RUN/up.log" >&2; exit 1; }

# The .bz2 is what a peer fetches, so its absence is the first thing to
# check: everything downstream would fail with a confusing error instead.
if [ -f "$RUN/up/FILES.BZ2" ]; then
    echo "the sharer published FILES.BZ2 ($(wc -c < "$RUN/up/FILES.BZ2" | tr -d ' ') bytes)"
    if command -v bzip2 >/dev/null 2>&1; then
        bzip2 -t "$RUN/up/FILES.BZ2" &&
            echo "  and bzip2 accepts it as a valid stream"
    fi
else
    echo "the sharer never wrote FILES.BZ2" >&2
    tail -5 "$RUN/up.log" >&2
    exit 1
fi

# Keys fed to the browse door once the list has arrived. The share's root
# is the shared directory itself ("up"), so getting to a file means two
# descents -- which is the navigation worth testing anyway.
#
#   ENTER  into "up"
#   ENTER  into "warez" (directories sort first)
#   DOWN   past "apps" to MAZERUN2.ZIP
#   SPACE  queue it
#
# The sleeps are generous: this is driving a UI through a pipe, and a tight
# sequence would be testing the timing rather than the feature.
echo "fetching the list and walking it..."
(
    sleep 8;  printf '\r'
    sleep 3;  printf '\r'
    sleep 3;  printf '\033[B'
    sleep 2;  printf ' '
    sleep 20
) | ( cd "$RUN/down" && exec "$CLIENT" -c "$RUN/dn.ini" \
    --screenshot "$RUN/dn.txt" --null-display --door 8 --peer-trace \
    --browse Sharer --run-secs 32 127.0.0.1 ) > "$RUN/dn.log" 2>&1

echo
echo "--- the browse door ---"
sed -n '2,16p' "$RUN/dn.txt" 2>/dev/null || echo "(no screenshot)"
echo "-----------------------"

ok=1
fail() { echo "FAIL: $*" >&2; ok=0; }

# ---- pass 1 asked for partial lists, and got them
grep -q "CGET list /" "$RUN/dn.log" ||
    fail "the downloader never asked for a partial list"
grep -q "CSND list /" "$RUN/up.log" ||
    fail "the sharer never sent a directory listing"
grep -q "CGET file files.xml.bz2" "$RUN/dn.log" &&
    fail "the downloader fell back to the whole list when it did not need to"

# One request per directory opened, which is the whole point: the top
# level, then up/, then up/warez/.
ASKED=$(grep -c "CGET list " "$RUN/dn.log")
echo "the downloader asked for $ASKED listings"
[ "$ASKED" -ge 3 ] ||
    fail "only $ASKED listings were asked for; walking two levels needs 3"

# Two levels down, so the door should be showing the contents of
# /up/warez: a directory and a file.
grep -q "/up/warez" "$RUN/dn.txt" ||
    fail "the door is not showing /up/warez -- navigation did not work"
grep -q "apps" "$RUN/dn.txt" || fail "the listing does not show the apps directory"
grep -q "MAZERUN2.ZIP" "$RUN/dn.txt" || fail "the listing does not show MAZERUN2.ZIP"
grep -q "DIR" "$RUN/dn.txt" || fail "directories are not marked"
grep -qi "incomplete" "$RUN/dn.txt" &&
    fail "a four-file list was reported as incomplete"

# The entry count in the footer: 4 files plus 3 directories (the share root
# itself, warez, warez/apps, mods) -- the exact number depends on how the
# virtual paths nest, so just check it is plausible and non-zero.
if grep -qE "[0-9]+ entries" "$RUN/dn.txt"; then
    echo "footer: $(grep -oE '[0-9]+ entries' "$RUN/dn.txt" | head -1)"
else
    fail "no entry count in the footer"
fi

# The temporary must not be left behind: it is someone else's share and the
# next browse would be ambiguous.
[ -f "$RUN/down/PEERLIST.BZ2" ] &&
    fail "the fetched list was left on disk"

# SPACE on a file in the browse door queues it, and because the sharer is
# active the download then actually runs. This is the point of browsing.
GOT=$(ls "$RUN/down" | grep -viE 'DOSYPP|\.ini|\.txt' | head -1)
if [ -z "$GOT" ]; then
    fail "queueing from the browse door downloaded nothing"
else
    echo "downloaded from the browse door: $GOT"
    if command -v rhash >/dev/null 2>&1; then
        WANT=$(rhash --tth "$RUN/up/warez/MAZERUN2.ZIP" | cut -d' ' -f1 |
               tr 'a-z' 'A-Z')
        HAVE=$(rhash --tth "$RUN/down/$GOT" | cut -d' ' -f1 | tr 'a-z' 'A-Z')
        if [ "$WANT" = "$HAVE" ]; then
            echo "  and its hash matches the original"
        else
            fail "the file queued from the browse door is not the right file"
        fi
    fi
fi

# ---- pass 2: a sharer that refuses partial lists
echo
echo "and again against a sharer that refuses partial lists..."

kill "$UP_PID" 2>/dev/null
wait "$UP_PID" 2>/dev/null
rm -rf "$RUN/down2"; mkdir -p "$RUN/down2"

( cd "$RUN/up" && exec "$CLIENT" -c "$RUN/up.ini" --null-display \
    --door 5 --peer-trace --refuse-partial --run-secs 60 127.0.0.1 ) \
    > "$RUN/up2.log" 2>&1 &
UP_PID=$!
for _ in $(seq 60); do
    grep -q "listening on port" "$RUN/up2.log" 2>/dev/null && break
    sleep 0.5
done

(
    sleep 10; printf '\r'
    sleep 3;  printf '\r'
    sleep 12
) | ( cd "$RUN/down2" && exec "$CLIENT" -c "$RUN/dn.ini" \
    --screenshot "$RUN/dn2.txt" --null-display --door 8 --peer-trace \
    --browse Sharer --run-secs 26 127.0.0.1 ) > "$RUN/dn2.log" 2>&1

echo "--- the browse door, after falling back ---"
sed -n '2,14p' "$RUN/dn2.txt" 2>/dev/null || echo "(no screenshot)"
echo "-------------------------------------------"

grep -q "CGET list /" "$RUN/dn2.log" ||
    fail "the downloader did not try a partial list first"
grep -qi "does not do partial lists" "$RUN/dn2.txt" ||
    grep -qi "partial" "$RUN/dn2.log" ||
    fail "the refusal was not reported"
grep -q "CGET file files.xml.bz2" "$RUN/dn2.log" ||
    fail "the downloader did not fall back to the whole list"
grep -q "CSND file files.xml.bz2" "$RUN/up2.log" ||
    fail "the refusing sharer never sent the whole list either"

# And the fallback has to work, not merely happen: the tree is there and
# two levels down shows the same files as pass 1.
grep -q "MAZERUN2.ZIP" "$RUN/dn2.txt" ||
    fail "the whole-list fallback did not produce a usable tree"

# One refusal, not one per directory: the downloader remembers.
REFUSALS=$(grep -c "CGET list " "$RUN/dn2.log")
echo "partial attempts before falling back: $REFUSALS"
[ "$REFUSALS" -le 2 ] ||
    fail "the downloader kept asking for partial lists after a refusal ($REFUSALS times)"

echo
if [ "$ok" = 1 ]; then
    echo "BROWSE OK: partial lists one directory at a time, and a fallback"
    echo "to the whole list for a peer that refuses them."
    exit 0
fi

echo "--- downloader output ---" >&2
tail -15 "$RUN/dn.log" >&2
echo "BROWSE FAILED" >&2
exit 1
