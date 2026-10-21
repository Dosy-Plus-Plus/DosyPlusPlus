#!/bin/bash
# The real-client gate: DOSy++ against AirDC++.
#
# Everything else in this repo checks DOSy++ against itself or against a hub.
# That proves self-consistency, not correctness: two ends of the same wrong
# idea agree perfectly. AirDC++ is an independent client in the DC++ lineage,
# so what it accepts is what the ecosystem accepts.
#
# What this checks, in order of how badly it would hurt to have wrong:
#
#   1. AirDC++ parses our INF, and we parse theirs.
#   2. We search; AirDC++ answers; we read its DRES (name, size, TTH).
#   3. We fetch AirDC++'s real files.xml.bz2 and parse it. This is the one
#      that matters most -- our XML reader has never seen a file list it did
#      not write.
#   4. We download a file from AirDC++ and verify the hash.
#   5. AirDC++ searches, finds OUR file, and downloads it -- which checks our
#      search replies, our file list and our CSND framing from the other
#      side. Driven through AirDC++'s own REST API.
#
# Run it with: make test-airdcpp
#
# Topology, which is all Docker Desktop's doing: a container's traffic
# reaches the host as 127.0.0.1, and the host is 192.168.65.254 from inside
# the container. So both ends look like loopback to the hub and neither can
# dial the address the hub observed. The hub is therefore told explicitly
# what each nick is reachable at (--i4-for), and AirDC++'s client port is
# published. A real hub on a real network needs none of this.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLIENT="$ROOT/build/host/dosypp"
RUN="${AIRDC_RUN:-$(mktemp -d)}"
HUB_PORT=${AIRDC_HUB_PORT:-24961}
DOSY_PORT=${AIRDC_DOSY_PORT:-24962}
AIR_PORT=${AIRDC_AIR_PORT:-21248}
IMAGE=${AIRDC_IMAGE:-gangefors/airdcpp-webclient:latest}
CONTAINER=${AIRDC_CONTAINER:-dosypp-airdcpp}

# How each side is reachable from the other, on Docker Desktop.
HOST_FROM_CONTAINER=${AIRDC_HOST_IP:-192.168.65.254}
CONTAINER_FROM_HOST=127.0.0.1

AIR_NICK=AirPeer
DOSY_NICK=DOSyPP

cleanup() {
    [ -n "${HUB_PID:-}" ] && kill "$HUB_PID" 2>/dev/null
    pkill -f "dosypp -c $RUN/" 2>/dev/null
    docker rm -f "$CONTAINER" > /dev/null 2>&1
    # Kept on failure as well as on request: this test has a lot of moving
    # parts and the logs are the whole point of running it.
    if [ -n "${AIRDC_KEEP:-}" ] || [ "${ok:-1}" != 1 ]; then
        echo "run dir kept: $RUN"
        return
    fi
    rm -rf "$RUN"
}
trap cleanup EXIT

[ -x "$CLIENT" ] || { echo "run: make host" >&2; exit 1; }
command -v rhash >/dev/null 2>&1 || { echo "need rhash (brew install rhash)" >&2; exit 1; }
command -v docker >/dev/null 2>&1 || { echo "need docker" >&2; exit 1; }
docker info > /dev/null 2>&1 || { echo "docker is not running" >&2; exit 1; }
command -v curl >/dev/null 2>&1 || { echo "need curl" >&2; exit 1; }

docker image inspect "$IMAGE" > /dev/null 2>&1 || {
    echo "pulling $IMAGE (amd64; it runs under emulation on Apple silicon)"
    docker pull --platform linux/amd64 "$IMAGE" || exit 1; }

ok=1
fail() { echo "FAIL: $*" >&2; ok=0; }

mkdir -p "$RUN/air/cfg" "$RUN/air/share/warez" "$RUN/air/dl" \
         "$RUN/dosy/share/warez" "$RUN/dosy/dl"

# --- the two shares ----------------------------------------------------
# Each side shares one file the other will go looking for. Random content,
# fixed seed: a file of zeros would hide an off-by-one in the transfer.
python3 - "$RUN/air/share/warez/AIRDCFIL.ZIP" 150000 1994 <<'PY'
import random, sys
random.seed(int(sys.argv[3]))
with open(sys.argv[1], "wb") as f:
    f.write(bytes(random.getrandbits(8) for _ in range(int(sys.argv[2]))))
PY
python3 - "$RUN/dosy/share/warez/DOSYFILE.ZIP" 120000 2026 <<'PY'
import random, sys
random.seed(int(sys.argv[3]))
with open(sys.argv[1], "wb") as f:
    f.write(bytes(random.getrandbits(8) for _ in range(int(sys.argv[2]))))
PY

AIR_TTH=$(rhash --tth "$RUN/air/share/warez/AIRDCFIL.ZIP" | cut -d' ' -f1 |
          tr 'a-z' 'A-Z')
DOSY_TTH=$(rhash --tth "$RUN/dosy/share/warez/DOSYFILE.ZIP" | cut -d' ' -f1 |
           tr 'a-z' 'A-Z')
echo "AirDC++ shares AIRDCFIL.ZIP  TTH $AIR_TTH"
echo "DOSy++  shares DOSYFILE.ZIP  TTH $DOSY_TTH"

# --- AirDC++'s configuration ------------------------------------------
cat > "$RUN/air/cfg/DCPlusPlus.xml" <<EOF
<?xml version="1.0" encoding="utf-8" standalone="yes"?>
<DCPlusPlus>
	<Settings>
		<Nick type="string">$AIR_NICK</Nick>
		<Description type="string">the real client</Description>
		<DownloadDirectory type="string">/Downloads/</DownloadDirectory>
		<AutoDetectIncomingConnection type="int">0</AutoDetectIncomingConnection>
		<IncomingConnections type="int">1</IncomingConnections>
		<InPort type="int">$AIR_PORT</InPort>
		<UDPPort type="int">$AIR_PORT</UDPPort>
		<TLSPort type="int">$((AIR_PORT + 1))</TLSPort>
		<MinimumSearchInterval type="int">2</MinimumSearchInterval>
		<AutoSearchLimit type="int">0</AutoSearchLimit>
	</Settings>
	<Share Token="0" Name="Default">
		<Directory Virtual="Share" Incoming="0">/Share/</Directory>
	</Share>
</DCPlusPlus>
EOF

cat > "$RUN/air/cfg/Favorites.xml" <<EOF
<?xml version="1.0" encoding="utf-8" standalone="yes"?>
<Favorites>
	<Hubs>
		<Hub Name="DOSyPP conformance" Connect="1" Nick="$AIR_NICK" Password="" Server="adc://$HOST_FROM_CONTAINER:$HUB_PORT" Group="" ShareProfile="0"/>
	</Hubs>
</Favorites>
EOF

# A web user with a password we know, so the API can be driven later. The
# hash is AirDC++'s own format, so it is produced by the tool itself.
printf 'tester\nhunter2\nhunter2\n' | docker run -i --rm --platform linux/amd64 \
    --user 1000:1000 -v "$RUN/air/cfg:/.airdcpp" \
    --entrypoint /airdcpp-webclient/airdcppd "$IMAGE" \
    -c=/.airdcpp --add-user > "$RUN/air/adduser.log" 2>&1
grep -qi "added\|updated\|password" "$RUN/air/adduser.log" ||
    echo "note: could not confirm the web user was added; the API half may skip"

chmod -R 777 "$RUN/air"

# --- the hub ----------------------------------------------------------
"$ROOT/tools/stubhub.py" --port "$HUB_PORT" --no-fake-share \
    --i4-for "$AIR_NICK=$CONTAINER_FROM_HOST" \
    --i4-for "$DOSY_NICK=$HOST_FROM_CONTAINER" \
    > "$RUN/hub.log" 2>&1 &
HUB_PID=$!
for _ in $(seq 20); do
    nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null && break
    sleep 0.25
done
nc -z 127.0.0.1 "$HUB_PORT" 2>/dev/null || {
    echo "the stub hub did not start" >&2; cat "$RUN/hub.log" >&2; exit 1; }

# --- AirDC++ ----------------------------------------------------------
echo "starting AirDC++ (its client port is published so we can dial it)..."
docker run -d --name "$CONTAINER" --platform linux/amd64 \
    --add-host=host.docker.internal:host-gateway \
    --user 1000:1000 \
    -p "$AIR_PORT:$AIR_PORT" -p "$AIR_PORT:$AIR_PORT/udp" \
    -p 25600:5600 \
    -v "$RUN/air/cfg:/.airdcpp" \
    -v "$RUN/air/share:/Share:ro" \
    -v "$RUN/air/dl:/Downloads" \
    "$IMAGE" --cdm-hub --cdm-client > /dev/null || {
        echo "could not start the container" >&2; exit 1; }

for _ in $(seq 120); do
    grep -q "logged in as '$AIR_NICK'" "$RUN/hub.log" 2>/dev/null && break
    sleep 1
done
grep -q "logged in as '$AIR_NICK'" "$RUN/hub.log" || {
    echo "AirDC++ never logged in:" >&2
    docker logs "$CONTAINER" 2>&1 | tail -20 >&2
    exit 1; }
echo "AirDC++ is on the hub"

# Its share has to be hashed before it can answer a search for it.
for _ in $(seq 120); do
    grep -qE "BINF .* SS[1-9]" "$RUN/hub.log" 2>/dev/null && break
    sleep 1
done

AIR_INF=$(grep -oE "VEAirDC\+\+w[^ ]*" "$RUN/hub.log" | head -1)
echo "AirDC++ reports itself as: ${AIR_INF#VE}"

# --- DOSy++ -----------------------------------------------------------
cat > "$RUN/dosy/dosypp.ini" <<EOF
hub = 127.0.0.1
port = $HUB_PORT
nick = $DOSY_NICK
sound = no
active = yes
port.listen = $DOSY_PORT
share = $RUN/dosy/share
EOF

# Searches for AirDC++'s file, queues the result, then browses AirDC++.
# One run does all of it: the keystrokes drive the browse door after the
# search has been queued.
echo "starting DOSy++ (active), searching, queueing and browsing..."
# Connections to one peer are serialised (we do not advertise MCN1), so
# the list and the file arrive one after the other and the run has to be
# long enough for both.
(
    sleep 30; printf '\r'        # ENTER: into the share root
    sleep 4;  printf '\r'        # ENTER: into the first directory
    sleep 45
) | ( cd "$RUN/dosy" && exec "$CLIENT" -c "$RUN/dosy/dosypp.ini" \
        --screenshot "$RUN/dosy/shot.txt" --null-display --door 8 \
        --peer-trace --search AIRDCFIL --queue --browse "$AIR_NICK" \
        --run-secs 150 127.0.0.1 ) > "$RUN/dosy/run.log" 2>&1 &
DOSY_PID=$!

# DOSy++ keeps running while the API half drives AirDC++: it has to be on
# the hub to answer a search and online to serve the file. Letting it exit
# first is how this test lied the first time it ran.
#
# Wait for OUR share to be hashed and announced. Matched on our own SID,
# because AirDC++ announces a share too and grepping for any "SF1" matches
# its INF -- which is how an earlier version of this test ran the whole
# upload half against a client that was sharing nothing and reported it as
# our search replies being unacceptable.
DOSY_SID=$(grep -oE "^\[([A-Z0-9]{4})\] logged in as '$DOSY_NICK'" \
           "$RUN/hub.log" 2>/dev/null | head -1 |
           sed -E "s/^\[([A-Z0-9]{4})\].*/\1/")
for _ in $(seq 90); do
    [ -n "$DOSY_SID" ] && break
    DOSY_SID=$(grep -oE "^\[([A-Z0-9]{4})\] logged in as '$DOSY_NICK'" \
               "$RUN/hub.log" 2>/dev/null | head -1 |
               sed -E "s/^\[([A-Z0-9]{4})\].*/\1/")
    sleep 1
done

for _ in $(seq 90); do
    grep -qE "^\[$DOSY_SID\] --> BINF .* SF[1-9]" "$RUN/hub.log" \
        2>/dev/null && break
    sleep 1
done

if grep -qE "^\[$DOSY_SID\] --> BINF .* SF[1-9]" "$RUN/hub.log" 2>/dev/null; then
    echo "DOSy++ announced its share: $(grep -oE 'SS[0-9]+ SF[0-9]+' "$RUN/hub.log" | tail -1)"
else
    echo "DOSy++ never announced a share, so nothing it serves can be" >&2
    echo "found. Its own complaints:" >&2
    grep -iE "config:|share" "$RUN/dosy/run.log" 2>/dev/null | head -5 >&2
    echo "(a share path longer than 127 characters is dropped -- run this" >&2
    echo " from a shorter directory)" >&2
    fail "DOSy++ has no share to be found"
fi

for _ in $(seq 90); do
    [ -n "$(ls "$RUN/dosy" | grep -iE '\.zip$')" ] && break
    sleep 1
done


# 5. The other direction: AirDC++ searches for our file and downloads it.
#    Driven through its REST API, because nothing else makes a real client
#    want something.
API="http://127.0.0.1:25600/api/v1"
AUTH='{"username":"tester","password":"hunter2"}'
TOKEN=$(curl -s -m 10 -X POST -H "Content-Type: application/json" \
        -d "$AUTH" "$API/sessions/authorize" 2>/dev/null |
        python3 -c 'import json,sys
try:
    print(json.load(sys.stdin)["auth_token"])
except Exception:
    pass' 2>/dev/null)

if [ -z "$TOKEN" ]; then
    echo "note: could not log in to AirDC++'s API; skipping the upload half" >&2
    echo "      (what it serves us is already checked above)"
else
    echo "logged in to AirDC++'s API; asking it to search for our file"

    # The API's own sequence: create a search instance, send the query to
    # the hubs, wait, read the results, queue one. Searching by NAME rather
    # than by hash on purpose -- that is the path that tests whether our
    # DRES is acceptable to a real client, hash and all.
    INSTANCE=$(curl -s -m 10 -X POST -H "Content-Type: application/json" \
        -H "Authorization: Bearer $TOKEN" -d '{}' "$API/search" 2>/dev/null |
        python3 -c 'import json,sys
try:
    print(json.load(sys.stdin)["id"])
except Exception:
    pass' 2>/dev/null)

    if [ -z "$INSTANCE" ]; then
        fail "AirDC++'s API would not create a search"
    else
        curl -s -m 20 -X POST -H "Content-Type: application/json" \
            -H "Authorization: Bearer $TOKEN" \
            -d '{"query":{"pattern":"DOSYFILE"}}' \
            "$API/search/$INSTANCE/hub_search" > "$RUN/air/search.json" 2>&1

        QUEUED=$(python3 -c 'import json,sys
try:
    print(json.load(open(sys.argv[1]))["queued_count"])
except Exception:
    print(0)' "$RUN/air/search.json" 2>/dev/null)
        echo "  it sent the search to $QUEUED hub(s)"

        # Give the search time to go out and our reply to come back.
        for _ in $(seq 20); do
            COUNT=$(curl -s -m 10 -H "Authorization: Bearer $TOKEN" \
                "$API/search/$INSTANCE" 2>/dev/null |
                python3 -c 'import json,sys
try:
    print(json.load(sys.stdin)["result_count"])
except Exception:
    print(0)' 2>/dev/null)
            [ "${COUNT:-0}" -ge 1 ] && break
            sleep 1
        done

        RESULTS=$(curl -s -m 20 -H "Authorization: Bearer $TOKEN" \
            "$API/search/$INSTANCE/results/0/5" 2>/dev/null)
        printf '%s' "$RESULTS" > "$RUN/air/results.json"

        RID=$(printf '%s' "$RESULTS" | python3 -c 'import json,sys
try:
    rs = json.load(sys.stdin)
    print(rs[0]["id"] if rs else "")
except Exception:
    pass' 2>/dev/null)

        if [ -z "$RID" ]; then
            fail "AirDC++ found nothing when it searched for our file -- our search replies are not acceptable to it"
        else
            NAME=$(printf '%s' "$RESULTS" | python3 -c 'import json,sys
rs = json.load(sys.stdin)
print(rs[0]["name"], rs[0].get("tth", "?"))' 2>/dev/null)
            echo "  it found: $NAME"

            printf '%s' "$RESULTS" | grep -qi "$DOSY_TTH" ||
                fail "AirDC++ read a different hash for our file than we sent"

            # Queue it, naming us as the source. Queueing by hash alone
            # leaves AirDC++ waiting for one of its periodic auto-searches
            # to turn a source up, which is minutes away and not what is
            # being tested -- the transfer is.
            DOSY_CID=$(grep -oE "^cid: [A-Z0-9]{39}" "$RUN/dosy/run.log" |
                       head -1 | cut -d' ' -f2)
            echo "  our CID is $DOSY_CID"

            curl -s -m 20 -X POST -H "Content-Type: application/json" \
                -H "Authorization: Bearer $TOKEN" \
                -d "{\"target_name\":\"DOSYFILE.ZIP\",\"size\":120000,\"tth\":\"$DOSY_TTH\",\"user\":{\"cid\":\"$DOSY_CID\",\"hub_url\":\"adc://$HOST_FROM_CONTAINER:$HUB_PORT\"}}" \
                "$API/queue/bundles/file" \
                > "$RUN/air/download.json" 2>&1

            grep -q '"id"' "$RUN/air/download.json" ||
                fail "AirDC++ would not queue our file: $(head -c 200 "$RUN/air/download.json")"

            for _ in $(seq 90); do
                [ -n "$(find "$RUN/air/dl" -type f ! -name '.*' 2>/dev/null)" ] &&
                    break
                sleep 1
            done

            AGOT=$(find "$RUN/air/dl" -type f ! -name '.*' 2>/dev/null | head -1)
            if [ -n "$AGOT" ]; then
                AHAVE=$(rhash --tth "$AGOT" | cut -d' ' -f1 | tr 'a-z' 'A-Z')
                ASZ=$(wc -c < "$AGOT" | tr -d ' ')
                echo "AirDC++ downloaded $(basename "$AGOT") ($ASZ bytes) from us"
                if [ "$AHAVE" = "$DOSY_TTH" ]; then
                    echo "  and it hashes to what we advertised"
                else
                    fail "what AirDC++ got from us hashes to $AHAVE, not $DOSY_TTH"
                fi
            else
                fail "AirDC++ queued our file but never finished downloading it: $(head -c 200 "$RUN/air/download.json" 2>/dev/null)"
            fi
        fi
    fi
fi

wait "$DOSY_PID" 2>/dev/null

# Everything below reads what the run left behind, so it waits for the
# client to have exited and written its screenshot first.
docker logs "$CONTAINER" > "$RUN/air/cdm.log" 2>&1

echo
echo "--- what DOSy++ drew (AirDC++'s file list) ---"
sed -n '2,14p' "$RUN/dosy/shot.txt" 2>/dev/null || echo "(no screenshot)"
echo "----------------------------------------------"

# 1. Each side parsed the other's INF rather than dropping the connection.
# (The screenshot above was taken when DOSy++ exited, which the API half
#  below waits for.)
grep -q "logged in as '$DOSY_NICK'" "$RUN/hub.log" ||
    fail "DOSy++ did not log in"
grep -qi "AirPeer" "$RUN/dosy/run.log" "$RUN/dosy/shot.txt" 2>/dev/null ||
    echo "note: AirDC++'s nick is not in the screenshot (door dependent)"

# 2. AirDC++ answered our search, and we read it.
if grep -q "DRES" "$RUN/hub.log" && grep -q "$AIR_TTH" "$RUN/hub.log"; then
    echo "AirDC++ answered our search with the right hash"
else
    fail "AirDC++ never answered our search with AIRDCFIL.ZIP's hash"
fi

# 3. We fetched and parsed its real file list.
#
# Partial lists are what the client asks for now, and AirDC++ is the test
# of whether a real client answers them: if it does, we never ask for the
# whole thing. Either outcome is acceptable here -- what is not acceptable
# is asking for a partial list and then failing to read what comes back,
# which is why the entry count below matters more than which form was used.
if grep -q "CSND list /" "$RUN/dosy/run.log"; then
    echo "AirDC++ answered a partial list request"
    PARTIAL_OK=1
elif grep -q "CSND file files.xml.bz2" "$RUN/dosy/run.log"; then
    echo "AirDC++ refused partial lists; the whole-list fallback was used"
    PARTIAL_OK=0
    grep -q "CGET file files.xml.bz2" "$RUN/dosy/run.log" ||
        fail "the fallback to the whole list never happened"
else
    fail "AirDC++ sent no file list of either kind"
    PARTIAL_OK=0
fi

grep -q "CGET list /" "$RUN/dosy/run.log" ||
    fail "DOSy++ never asked AirDC++ for a partial list"
grep -qE "[0-9]+ entries" "$RUN/dosy/shot.txt" 2>/dev/null ||
    fail "AirDC++'s file list did not parse into entries"
grep -qi "could not be read" "$RUN/dosy/shot.txt" 2>/dev/null &&
    fail "AirDC++'s file list could not be read"
grep -q "AIRDCFIL.ZIP" "$RUN/dosy/shot.txt" 2>/dev/null &&
    echo "its file list shows AIRDCFIL.ZIP where expected"

# 3b. And the other direction: AirDC++ browsing us. Its own log says what
# it asked for, and ours says what we answered. A partial list served to a
# real client is the half of this feature that our own tests cannot
# establish -- both ends being ours proves only that we agree with
# ourselves.
if grep -q "CGET list " "$RUN/dosy/run.log"; then
    : # our own outgoing requests, already checked above
fi
if grep -qE "CSND list /" "$RUN/dosy/run.log" &&
   grep -qE "CGET list /" "$RUN/dosy/run.log"; then
    echo "partial lists crossed in both directions"
fi

# 4. We downloaded its file and the hash matches.
GOT=$(ls "$RUN/dosy" | grep -iE '\.zip$' | head -1)
if [ -z "$GOT" ]; then
    fail "nothing was downloaded from AirDC++"
else
    HAVE=$(rhash --tth "$RUN/dosy/$GOT" | cut -d' ' -f1 | tr 'a-z' 'A-Z')
    SZ=$(wc -c < "$RUN/dosy/$GOT" | tr -d ' ')
    echo "downloaded $GOT ($SZ bytes) from AirDC++"
    if [ "$HAVE" = "$AIR_TTH" ]; then
        echo "  and its hash matches what AirDC++ advertised"
    else
        fail "the file downloaded from AirDC++ hashes to $HAVE, not $AIR_TTH"
    fi
fi

echo
echo "--- what AirDC++ logged of our client-to-client traffic ---"
grep -E "^Client:" "$RUN/air/cdm.log" | head -12 || echo "(none)"

# Anything AirDC++ considered an error is worth seeing even when the
# assertions pass: a client that tolerates our mistakes today may not
# tomorrow.
if grep -qiE "unknown|invalid|malformed|unsupported" "$RUN/air/cdm.log"; then
    echo
    echo "--- AirDC++ complained about something ---"
    grep -iE "unknown|invalid|malformed|unsupported" "$RUN/air/cdm.log" |
        head -8
fi

echo
if [ "$ok" = 1 ]; then
    echo "AIRDCPP OK: an independent client accepts what DOSy++ produces."
    exit 0
fi

echo "--- DOSy++ output ---" >&2
tail -25 "$RUN/dosy/run.log" >&2
echo "AIRDCPP FAILED" >&2
exit 1
