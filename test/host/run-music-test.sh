#!/bin/bash
# The music, end to end: does the client actually play the shipped tune?
#
# "A WAV file appeared" is not the question -- a file full of silence, or of
# the wrong module, or mixed at the wrong rate, would pass that. So this
# runs the real client with music on, and compares the audio it produced
# with an independent render of the same module by the same mixer driven
# directly (test_mod --render). The bytes have to match, which means the
# client loaded that file, at that rate, and started it at the top.
#
# It also checks the info door says what is playing, and that /music
# stops it -- the two things a user interacts with.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLIENT="$ROOT/build/host/dosypp"
RENDER="$ROOT/build/host/test_mod"
MOD="$ROOT/art/DOSYPP.MOD"
RUN="$(mktemp -d)"
RATE=11025
HUB_PORT=${MUSIC_HUB_PORT:-24997}

ok=1
fail() { echo "FAIL: $*" >&2; ok=0; }

cleanup() {
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    pkill -f "dosypp -c $RUN/" 2>/dev/null
    if [ -n "${MUSIC_KEEP:-}" ] || [ "$ok" != 1 ]; then
        echo "run dir kept: $RUN"
        return
    fi
    rm -rf "$RUN"
}
trap cleanup EXIT

[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }
[ -x "$RENDER" ] || { echo "run: make test" >&2; exit 1; }
[ -f "$MOD" ]    || { echo "run: make modules" >&2; exit 1; }

cp "$MOD" "$RUN/DOSYPP.MOD"

# Relative names, and the client run from $RUN: the config's string fields
# are sized for DOS paths and a /var/folders temp path does not fit in one.
cat > "$RUN/node.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = Warlord
active = no
sound = no
music = yes
music.file = DOSYPP.MOD
music.rate = $RATE
music.wav = out.wav
EOF

# --no-motd so the welcome door falls back to our own bulletin, which is
# where the credits and the tune title are drawn.
"$ROOT/tools/stubhub.py" --port "$HUB_PORT" --no-motd \
    > "$RUN/hub.log" 2>&1 &
HUB_PID=$!
for _ in $(seq 20); do
    nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null && break
    sleep 0.25
done

echo "running the client with music on..."
(cd "$RUN" && exec "$CLIENT" -c "$RUN/node.ini" --run-secs 6 --door 7 \
    --null-display --screenshot "$RUN/info.txt" 127.0.0.1) \
    > "$RUN/client.log" 2>&1 < /dev/null

[ -f "$RUN/out.wav" ] || { fail "no WAV was written"; exit 1; }

# ---- the header, and enough audio to be a tune rather than a click
python3 - "$RUN/out.wav" "$RATE" <<'PY' || fail "WAV header or content wrong"
import struct, sys

path, rate = sys.argv[1], int(sys.argv[2])
d = open(path, 'rb').read()

if d[:4] != b'RIFF' or d[8:12] != b'WAVE':
    print("not a RIFF/WAVE file"); sys.exit(1)

riff, = struct.unpack('<I', d[4:8])
fmt, chans, srate, _, align, bits = struct.unpack('<HHIIHH', d[20:36])
dlen, = struct.unpack('<I', d[40:44])
body = d[44:]

bad = []
if (fmt, chans, bits) != (1, 1, 8):
    bad.append("format %d ch %d bits %d" % (fmt, chans, bits))
if srate != rate:
    bad.append("rate %d, wanted %d" % (srate, rate))
# The lengths are patched on close: if the client did not shut the device
# down cleanly they stay zero, and a player would see an empty file.
if dlen != len(body):
    bad.append("data length %d but %d bytes follow" % (dlen, len(body)))
if riff != 36 + len(body):
    bad.append("RIFF length %d, expected %d" % (riff, 36 + len(body)))

# At least a quarter second of audio, and it has to move: silence in 8-bit
# unsigned is a file full of 0x80.
if len(body) < rate // 4:
    bad.append("only %d frames of audio" % len(body))
else:
    lo, hi = min(body), max(body)
    if hi - lo < 32:
        bad.append("audio barely moves (%d..%d) -- silence?" % (lo, hi))

for b in bad:
    print(b)
sys.exit(1 if bad else 0)
PY

# ---- the bytes themselves, against the mixer driven directly
FRAMES=$(python3 -c "
import struct,sys
d=open('$RUN/out.wav','rb').read()
print(min(len(d)-44, $RATE*2))")

if [ "$FRAMES" -lt 2048 ]; then
    fail "too little audio to compare ($FRAMES frames)"
else
    "$RENDER" --render "$MOD" "$RATE" "$FRAMES" "$RUN/ref.raw" \
        > "$RUN/render.log" 2>&1 || fail "reference render failed"

    python3 - "$RUN/out.wav" "$RUN/ref.raw" "$FRAMES" <<'PY' \
        || fail "the client's audio is not this module"
import sys

wav, raw, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
got = open(wav, 'rb').read()[44:44 + n]
# The reference is signed; WAV 8-bit is unsigned. The bias is the only
# difference there should be.
want = bytes((b + 128) & 0xFF for b in open(raw, 'rb').read()[:n])

if len(want) < n:
    print("reference is short: %d of %d" % (len(want), n)); sys.exit(1)

for i in range(n):
    if got[i] != want[i]:
        print("differs at frame %d: got %d, mixer says %d"
              % (i, got[i], want[i]))
        sys.exit(1)

print("  %d frames identical to the mixer's own render" % n)
PY
fi

# ---- the info door (door 7), and the setup door (9) which is where the
# three music settings live
if ! grep -qi "MUSIC" "$RUN/info.txt" 2>/dev/null; then
    fail "the info door does not mention the music"
fi
if ! grep -qi "WAV file" "$RUN/info.txt" 2>/dev/null; then
    fail "the info door does not say what it is playing through"
fi
if ! grep -qi "DOSy++ attic" "$RUN/info.txt" 2>/dev/null; then
    fail "the info door does not show the module's title"
fi

(cd "$RUN" && exec "$CLIENT" -c "$RUN/node.ini" --run-secs 4 --door 9 \
    --null-display --screenshot "$RUN/setup.txt" 127.0.0.1) \
    > "$RUN/setup.log" 2>&1 < /dev/null

grep -qE "MUSIC FILE +DOSYPP.MOD" "$RUN/setup.txt" 2>/dev/null \
    || fail "the setup door does not show the module"
grep -qE "MUSIC RATE +$RATE" "$RUN/setup.txt" 2>/dev/null \
    || fail "the setup door does not show the rate"

# ---- the welcome door, with a hub that sent nothing: our own bulletin,
# which names the tune that is actually playing
(cd "$RUN" && exec "$CLIENT" -c "$RUN/node.ini" --run-secs 4 --door 6 \
    --null-display --screenshot "$RUN/welcome.txt" 127.0.0.1) \
    > "$RUN/welcome.log" 2>&1 < /dev/null

grep -qi "NO BULLETIN FROM THE HUB" "$RUN/welcome.txt" 2>/dev/null \
    || fail "a quiet hub left the welcome door blank"
grep -qi "DOSy++ attic" "$RUN/welcome.txt" 2>/dev/null \
    || fail "the bulletin does not name the tune that is playing"

# ---- a missing module is a message, not a crash
cat > "$RUN/nomod.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = Warlord
active = no
music = yes
music.file = ABSENT.MOD
music.wav = none.wav
EOF

(cd "$RUN" && exec "$CLIENT" -c "$RUN/nomod.ini" --run-secs 3 --door 1 \
    --null-display --screenshot "$RUN/nomod.txt" 127.0.0.1) \
    > "$RUN/nomod.log" 2>&1 < /dev/null
rc=$?
[ "$rc" = 0 ] || fail "a missing module stopped the client (exit $rc)"
grep -qi "no music" "$RUN/nomod.txt" 2>/dev/null \
    || fail "a missing module was not reported in the chat"

# ---- /music turns it off
(
    sleep 3
    printf '/music\r'
    sleep 3
) | (cd "$RUN" && exec "$CLIENT" -c "$RUN/node.ini" --run-secs 7 --door 1 \
      --null-display --screenshot "$RUN/off.txt" 127.0.0.1) \
      > "$RUN/off.log" 2>&1

grep -qi "music off" "$RUN/off.txt" 2>/dev/null \
    || fail "/music did not report turning the music off"

if [ "$ok" = 1 ]; then
    echo "MUSIC OK"
    exit 0
fi
echo "MUSIC FAILED"
exit 1
