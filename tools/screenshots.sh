#!/bin/bash
# Photograph every door against a session worth photographing.
#
# tools/screenshot.sh does one door against a bare hub, which is enough to
# check a layout and useless as a picture: an empty who list, an empty
# share, no transfers, no files. This stages a real session -- a hub with
# an ANSI welcome, other nodes logged in, a peer sharing real DOS software,
# our own share hashed, a search done, a queue with transfers in it -- and
# captures all ten doors plus the splash and the sign-off.
#
# The output is docs/screens/*.txt and docs/SCREENS.md, which is a gallery
# rather than a test. The rigs check layouts; this is for people.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLIENT="$ROOT/build/host/dosypp"
OUT="$ROOT/docs/screens"
. "$ROOT/test/host/hublib.sh"

# Short, because a share path is limited to 127 characters -- it has to fit
# a DOS path and the client says so if it does not.
RUN="$(mktemp -d /tmp/dsy.XXXX)"
HUB_PORT=$(free_port)
PEER_PORT=$(free_port)

[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }

cleanup() {
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    for p in ${PEERS:-}; do kill "$p" 2>/dev/null; done
    rm -rf "$RUN"
}
trap cleanup EXIT

mkdir -p "$OUT" "$RUN/dn"

# ---- the share: real software if it has been fetched
ATMOS="${ATMOS_DIR:-$ROOT/build/atmos}"
if [ -d "$ATMOS" ]; then
    "$ROOT/tools/mkbigshare.py" "$RUN/DL" 120M --real "$ATMOS" --quiet \
        > /dev/null
    echo "share: real software from $ATMOS, padded to 120M"
else
    "$ROOT/tools/mkbigshare.py" "$RUN/DL" 120M --quiet > /dev/null
    echo "share: generated filler (run 'make atmos' for real software)"
fi

# ---- the hub, with an ANSI welcome so the MOTD door has something to draw
start_stub_hub "$HUB_PORT" "$RUN/hub.log" --motd-ansi

# ---- a peer that shares the same software, so searches and browsing find
#      real names, and transfers have somewhere to come from
cat > "$RUN/razor.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = Razor
active = yes
port.listen = $PEER_PORT
sound = no
music = no
share = $RUN/DL
EOF

( cd "$RUN" && exec "$CLIENT" -c "$RUN/razor.ini" --null-display --door 5 \
    --run-secs 900 127.0.0.1 ) > "$RUN/razor.log" 2>&1 &
PEERS="$!"

# ---- and a few nodes to fill the who list and say something in the chat
for n in Phantom Acid Nitro; do
    cat > "$RUN/$n.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = $n
active = no
sound = no
music = no
EOF
    ( sleep 6
      printf 'anyone got the OpenCP docs?\r'; sleep 3
      printf '/me is uploading\r'; sleep 600
    ) | ( cd "$RUN" && exec "$CLIENT" -c "$RUN/$n.ini" --null-display \
            --door 1 --run-secs 900 127.0.0.1 ) > "$RUN/$n.log" 2>&1 &
    PEERS="$PEERS $!"
    sleep 1
done

echo "waiting for Razor to finish hashing..."
for _ in $(seq 120); do
    grep -qaE "BINF .* SS[1-9][0-9]* SF[1-9]" "$RUN/hub.log" && break
    sleep 2
done
echo "sharing: $(grep -aoE 'SS[0-9]+ SF[0-9]+' "$RUN/hub.log" | tail -1)"

# ---- our own config: the same share, and some favourites for door 0
cat > "$RUN/dn/DOSYPP.INI" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = Warlord
description = calling from 1994
active = no
sound = no
share = $RUN/DL
slots = 3

; On, so the info door says what it is playing and through what. The host
; backend writes a WAV, paced by the clock like a card.
music = yes
music.file = $ROOT/art/DOSYPP.MOD
music.rate = 11025
music.wav = $RUN/dn/shoot.wav
EOF

# Favourites, in the format the HUBS door writes: [hub] sections with
# key = value lines. The first version of this invented a pipe-separated
# format and the door quite correctly showed NO FAVOURITE HUBS YET.
cat > "$RUN/dn/DOSYPP.FAV" <<'EOF'
; DOSy++ favourite hubs. Written by the HUBS door.
;
; The passwords here are in the clear. There is nowhere better on a DOS
; machine -- no keychain, no file permissions -- and hiding them would
; only misrepresent how safe they are.

[hub]
name     = The Attic
address  = adc://attic.example.org:1511
nick     = Warlord
password = hunter2
auto     = yes

[hub]
name     = Riverside BBS
address  = adc://riverside.example.net:1511
nick     = Warlord
password =
auto     = no

[hub]
name     = Pitfall
address  = adcs://pitfall.example.org:2780
nick     =
password =
auto     = no

[hub]
name     = DOSyPP Test Hub
address  = adc://127.0.0.1:1511
nick     = Warlord
password =
auto     = no
EOF

shoot() {
    local file="$1" door="$2" secs="$3"; shift 3
    printf '  %-22s door %-2s ' "$file" "$door"
    ( cd "$RUN/dn" && exec "$CLIENT" -c "$RUN/dn/DOSYPP.INI" \
        --null-display --door "$door" --screenshot "$OUT/$file" \
        --run-secs "$secs" "$@" 127.0.0.1 ) > "$RUN/shoot.log" 2>&1
    if [ -f "$OUT/$file" ]; then
        echo "ok"
    else
        echo "FAILED"
        tail -3 "$RUN/shoot.log"
    fi
}

echo
echo "capturing:"

# The chat, after the other nodes have said their piece. Typed lines of our
# own, because a message base with only joins in it is not a message base.
( sleep 8;  printf 'evening all\r'
  sleep 2;  printf '/me is looking for OpenCP\r'
  sleep 10 ) | shoot 01-msgs.txt 1 22

shoot 02-who.txt 2 10
shoot 03-files.txt 3 14 --search ZIP
# Somebody searching for what we have, so the share door's reply counter
# is not a flat zero.
#
# Each searcher gets its own config: the first version pointed three of
# them at another node's file, which means three clients claiming one
# identity on one hub, and the hub was right to ignore them. And they fire
# seventy seconds in, not eight, because a search that arrives before our
# own share is hashed is a search we cannot answer.
delay=70
for term in ELIZA ZIP OPENCP; do
    cat > "$RUN/seek$term.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = Seek$term
active = no
sound = no
music = no
EOF
    ( sleep "$delay"
      cd "$RUN" && exec "$CLIENT" -c "$RUN/seek$term.ini" --null-display \
          --search "$term" --run-secs 6 127.0.0.1 ) > /dev/null 2>&1 &
    PEERS="$PEERS $!"
    delay=$((delay + 4))
done

shoot 05-share.txt 5 95
shoot 06-motd.txt 6 10
shoot 07-info.txt 7 10

# Transfers. Searched for something only Razor has, because Razor is the
# one peer that is really there and really active: the invented peers the
# stub hub answers for are passive, and two passive clients cannot connect
# -- the first version of this shot photographed an honest FAILED row and
# called it a showcase.
#
# FreeDoom is 26MB, which on loopback is over in a moment, so the picture
# is of a finished transfer with its rate rather than of a moving gauge.
shoot 04-xfers.txt 4 20 --search FREEDOOM --queue

shoot 08-browse.txt 8 26 --browse Razor --browse-dir /DL/WAREZ/GAMES/
shoot 09-setup.txt 9 10
shoot 10-hubs.txt 10 10

# The tall screen, which is a setting rather than a different programme:
# the same chat door with forty-three lines instead of eighteen.
( sleep 8;  printf 'evening all\r'
  sleep 2;  printf '/me is looking for OpenCP\r'
  sleep 8 ) | shoot 12-rows50.txt 1 20 --rows 50

# The splash and the sign-off need no hub at all.
printf '  %-22s ' "00-splash.txt / 11-logoff.txt"
( cd "$RUN/dn" && "$CLIENT" --art "$RUN/art.txt" -c "$RUN/dn/DOSYPP.INI" \
    -n Warlord ) > /dev/null 2>&1
if [ -f "$RUN/art.txt" ]; then
    awk '/^=== SPLASH/{f="s";next} /^=== LOGOFF/{f="l";next}
         f=="s"{print > "'"$OUT"'/00-splash.txt"}
         f=="l"{print > "'"$OUT"'/11-logoff.txt"}' "$RUN/art.txt"
    echo "ok"
else
    echo "FAILED"
fi

# ---- the gallery
GALLERY="$ROOT/docs/SCREENS.md"
{
    echo "# The doors"
    echo
    echo "Captured by \`tools/screenshots.sh\` against a live session: a hub"
    echo "with four nodes logged in, a peer sharing real DOS software (\`make"
    echo "atmos\`), our own share hashed, a search done and a queue with"
    echo "transfers in it."
    echo
    echo "These are the cell buffer written out as text -- the same bytes that"
    echo "go to 0xB8000, with CP437 translated for your browser. On DOS they"
    echo "are in colour; the layout is exactly this."
    echo

    for f in "$OUT"/*.txt; do
        name=$(basename "$f" .txt)
        case "$name" in
            00-splash) title="The splash" ;;
            01-msgs)   title="1 — MESSAGE BASE" ;;
            02-who)    title="2 — WHO'S ONLINE" ;;
            03-files)  title="3 — FILE AREAS (a search for ZIP)" ;;
            04-xfers)  title="4 — TRANSFERS" ;;
            05-share)  title="5 — MY SHARE" ;;
            06-motd)   title="6 — WELCOME (the hub's ANSI)" ;;
            07-info)   title="7 — NODE INFO" ;;
            08-browse) title="8 — BROWSE (Razor's games area)" ;;
            09-setup)  title="9 — NODE SETUP" ;;
            10-hubs)   title="0 — HUBS (favourites)" ;;
            11-logoff) title="The sign-off" ;;
            12-rows50) title="1 — MESSAGE BASE at 80x50 (rows = 50)" ;;
            *)         title="$name" ;;
        esac
        echo "## $title"
        echo
        echo '```'
        cat "$f"
        echo '```'
        echo
    done
} > "$GALLERY"

echo
echo "wrote $GALLERY and $(ls "$OUT"/*.txt | wc -l | tr -d ' ') screens"
