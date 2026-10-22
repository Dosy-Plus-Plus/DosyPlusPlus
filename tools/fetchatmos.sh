#!/bin/bash
# Fetch real DOS software for the demo share.
#
# The test rigs do not need this: a share of sparse filler exercises the
# same code. What it is for is the thing a person sees -- the share door,
# the browse door, the browser demo -- where "DEMO1.BIN, DEMO2.BIN" says
# nothing and FREEDOOM.ZIP, OPENCP.ZIP, ARACHNE.ZIP says what this client
# is for.
#
# Two of these earn their place twice. UNZIP is what the browser demo uses
# to open what it downloads, and Block Drop is a single 116KB executable --
# small enough to fetch over a hub in a moment and self-contained enough to
# run straight afterwards, which is the whole demo in two steps.
#
# Everything here is free software from the FreeDOS 1.3 repositories, and
# it is all period-correct in the way that matters: Open Cubic Player is
# the MOD player people actually used, Arachne is the DOS web browser,
# crynwr is the packet-driver collection this client talks to, and
# FreeDoom is a Doom-compatible game you may redistribute.
#
# What is NOT here, deliberately: Doom, Descent, Duke Nukem, PKZIP. The
# shareware episodes were licensed for redistribution and the full games
# never were, and "it was on a BBS in 1994" is not a licence. A warez
# client does not need to ship warez.
#
# Fetched, not committed, with every file pinned by hash -- the same
# arrangement as Watt-32, libbzip2 and the packet drivers.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ATMOS_DIR:-$ROOT/build/atmos}"
CACHE="${DOSYPP_CACHE:-$ROOT/.cache}"
BASE="${FREEDOS_BASE:-https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3}"

# Somewhere else to look when the first place does not have it. ibiblio is
# the only host for the per-package FreeDOS repositories -- there is no
# second mirror -- and it answers 503 for the whole subtree under load,
# which is enough to stop a CI run that has done nothing wrong. So a copy
# you control can go first and ibiblio can be the fallback:
#
#   FREEDOS_BASE=https://github.com/<org>/<repo>/releases/download/<tag> \
#   FREEDOS_BASE_ALT=https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3 \
#       ./tools/fetchatmos.sh
#
# tools/pubpackages.sh publishes such a copy. Every file is still checked
# against the sha256 pinned below, wherever it came from, so a mirror is
# not something that has to be trusted.
BASE_ALT="${FREEDOS_BASE_ALT:-}"

# repo/file  area-in-the-share  sha256  what it is
#
# The areas are where a 1994 board would have filed them, because that is
# the point of the exercise.
#
# Pinned by hash, like Watt-32, libbzip2 and the packet drivers: a mirror
# serving a different version, or an HTML error page with the right name,
# should stop the build rather than end up in a share and be offered to
# peers as software. Update a hash deliberately when you mean to update a
# package -- FREEDOS_ALLOW_UNPINNED=1 skips the check for a one-off.
SET="
https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip WAREZ/GAMES  3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59  as=FREEDOOM.ZIP Freedoom 0.13.0, from the project's own release
games/boom.zip         WAREZ/GAMES  fb54217f62483a0a4e10b5d19132b70ebdffddf64fae465e5d6eb4880da2f5c0  Boom, a Bomberman-style game
games/gnuchess.zip     WAREZ/GAMES  70a2c893b5bfae680b4b3181500ceb88ea2d501488488f38d4e83f1502c186e8  GNU Chess
games/eliza.zip        WAREZ/GAMES  fd4bbca1c1eacd97661e5880fedcc767aa7faea28e458c31b92cbf4765bcfad4  Eliza, the 1966 chatbot
sound/opencp.zip       MODS         5ac627083959fe46417f8b4e2350af31c30da8b23f2eb4b8913205d4f9db95e3  Open Cubic Player
sound/adplay.zip       MODS         628fdff0ad22fdcd8c7fe90fc6f77c313fdce84921608e10cbdd4026c41c5af5  AdLib player
net/arachne.zip        WAREZ/APPS   2a76fe4fcc764b0d872f51422f5c29462643b2bef013b51ef9beb4a2987d8305  Arachne, the DOS web browser
net/crynwr.zip         WAREZ/APPS   fd18758586d3efa7cf9cd9a69f3b61a79f2d5d5f46a97cc781a66bd28a5b5ec2  Crynwr packet drivers
net/gopherus.zip       WAREZ/APPS   488dc492d8fc5519c61bb1b3c2ae8fd96fa4e22ed8ef848d8d288c77b2e0b72f  Gopherus, a gopher client
archiver/arj.zip       WAREZ/APPS   376baacf9edc179ae17cc6229b72598e64284fd747f4c58952fdb1d841a61d18  open ARJ
archiver/gzip.zip      WAREZ/APPS   d9e130bb1dd9276955b51cc20214bcb8060e8414c927350b97a15cdb7e2e9d05  gzip for DOS
archiver/unzip.zip     WAREZ/APPS   2e22a2a9d4f9b44737d907fb77740e01bd3acc82ec18a4c4b3070eb3192a05fe  Info-ZIP UNZIP
games/blkdrop.zip      WAREZ/GAMES  199171a5124daaf266f823287a3d0c492069aa91ec3d32ea2c61dbe21dd38547  Block Drop, one 116KB EXE
base/ctmouse.zip       WAREZ/APPS   a891124cd5b13e8141778fcae718f3b2667b0a49727ce92b782ab11a8c4bb63a  CuteMouse, the int 33h a game expects
"

mkdir -p "$CACHE" "$DEST"

got=0
bad=0
netfail=0
total=0
printf '%-18s %-13s %s\n' FILE AREA SIZE

while read -r path area want_sha rest; do
    [ -z "$path" ] && continue
    name="$(basename "$path")"
    out="$CACHE/$name"

    # A row whose first field is a URL comes from its own project rather
    # than from the FreeDOS repositories, and says what to store it as:
    # "as=FREEDOOM.ZIP" keeps the share 8.3, which the demo's disk needs
    # because long filename support is off there on purpose.
    store="$(echo "$name" | tr 'a-z' 'A-Z')"
    case "$rest" in
        as=*) store="${rest%% *}"; store="${store#as=}" ;;
    esac

    if [ ! -f "$out" ]; then
        # --fail so a 404 does not leave an HTML error page named .zip,
        # which is the failure mode that wastes an afternoon: the file
        # exists, has the right name, and is not a zip.
        #
        # Three attempts, backing off: ibiblio is the only place these
        # come from -- FreeDOS has no second mirror for the per-package
        # repositories -- and it answers 503 under load. A single attempt
        # turns a busy minute into a failed build.
        # Two layouts at each base: the repository's own (games/boom.zip)
        # and a flat one (boom.zip), because a GitHub release asset has no
        # directories. A 404 on one is not an error, it is the other
        # layout; only running out of candidates is an error.
        case "$path" in
            http://*|https://*)
                # Its own project publishes it; there is nothing to mirror
                # and no second layout to guess.
                urls="$path" ;;
            *)
                urls="$BASE/$path $BASE/$name"
                [ -n "$BASE_ALT" ] &&
                    urls="$urls $BASE_ALT/$path $BASE_ALT/$name" ;;
        esac

        fetched=0
        for url in $urls; do
            for attempt in 1 2 3; do
                # The status code, not just success or failure: a 404 means
                # "wrong layout, try the next URL" and retrying it three
                # times with backoff wastes nine seconds per package. A 503
                # -- which is what ibiblio does under load -- means "ask
                # again", and a single attempt there turns a busy minute
                # into a failed build.
                code=$(curl -fsSL -m 300 -o "$out.part" \
                            -w '%{http_code}' "$url" 2>/dev/null) || true
                if [ "${code:-000}" = "200" ] && [ -s "$out.part" ]; then
                    fetched=1
                    break
                fi
                rm -f "$out.part"
                case "${code:-000}" in
                    404|403|410) break ;;    # not here; next candidate
                esac
                [ "$attempt" -lt 3 ] && sleep $((attempt * 3))
            done
            [ "$fetched" = 1 ] && break
        done
        if [ "$fetched" != 1 ]; then
            echo "could not fetch $name from any of:" >&2
            for url in $urls; do echo "    $url" >&2; done
            netfail=$((netfail + 1))
            continue
        fi
        mv "$out.part" "$out"
    fi

    # It has to be a zip, and it has to be THE zip. The magic check alone
    # would pass a different version of the same package.
    if [ "$(head -c 2 "$out")" != "PK" ]; then
        echo "$name is not a zip -- deleting" >&2
        rm -f "$out"
        continue
    fi

    got_sha=$(shasum -a 256 "$out" 2>/dev/null | cut -d' ' -f1)
    [ -z "$got_sha" ] && got_sha=$(sha256sum "$out" | cut -d' ' -f1)

    if [ "$got_sha" != "$want_sha" ]; then
        if [ -n "${FREEDOS_ALLOW_UNPINNED:-}" ]; then
            echo "$name: hash differs, allowed by FREEDOS_ALLOW_UNPINNED" >&2
            echo "  want $want_sha" >&2
            echo "  got  $got_sha" >&2
        else
            echo "$name: hash does not match what this script pins." >&2
            echo "  want $want_sha" >&2
            echo "  got  $got_sha" >&2
            echo "  The package may have been updated. Check, then update" >&2
            echo "  the hash in tools/fetchatmos.sh, or set" >&2
            echo "  FREEDOS_ALLOW_UNPINNED=1 for a one-off." >&2
            rm -f "$out"
            bad=$((bad + 1))
            continue
        fi
    fi

    mkdir -p "$DEST/$area"
    cp "$out" "$DEST/$area/$store"
    sz=$(wc -c < "$out" | tr -d ' ')
    total=$((total + sz))
    got=$((got + 1))
    printf '%-18s %-13s %s\n' "$name" "$area" \
        "$(echo "$sz" | awk '{printf "%.1f MB", $1/1048576}')"
done <<< "$SET"

echo
echo "$got files, $(echo $total | awk '{printf "%.1f MB", $1/1048576}') in $DEST"
if [ "$bad" -gt 0 ]; then
    echo "$bad package(s) refused on a hash mismatch -- see above" >&2
    exit 1
fi

# Nothing fetched is a failure, and it has to be said here. This used to
# print "cached in ..." and exit 0, leaving a directory containing only
# the README below -- so the first thing anybody saw was mkshare.sh three
# steps later saying "nothing small enough to share", which describes the
# symptom and not the cause. ibiblio was returning 503 for the whole
# FreeDOS subtree, and the CI log said nothing about it.
if [ "$got" -eq 0 ]; then
    echo >&2
    echo "no packages were fetched at all." >&2
    echo "  source: $BASE" >&2
    [ -n "$BASE_ALT" ] && echo "  fallback: $BASE_ALT" >&2
    if [ "$netfail" -gt 0 ]; then
        echo "  $netfail download(s) failed after three attempts each." >&2
        case "$BASE$BASE_ALT" in
            *ibiblio*)
                echo "  ibiblio hosts these and has no mirror for the" >&2
                echo "  per-package repositories, so an outage there stops" >&2
                echo "  this script. Try again later, or publish a copy" >&2
                echo "  with tools/pubpackages.sh and point FREEDOS_BASE" >&2
                echo "  at it." >&2
                ;;
            *)
                echo "  Nothing was reachable at the base(s) above. These" >&2
                echo "  are the FreeDOS 1.3 repositories; unset" >&2
                echo "  FREEDOS_BASE to use ibiblio directly." >&2
                ;;
        esac
    fi
    exit 1
fi

if [ "$netfail" -gt 0 ]; then
    echo "$netfail package(s) could not be downloaded -- see above" >&2
    exit 1
fi

echo "cached in $CACHE -- delete it to refetch"

# A note that travels with the files, because somebody will find this
# directory later and wonder.
cat > "$DEST/README.TXT" <<'EOF'
These are free software packages from the FreeDOS 1.3 repositories,
fetched by tools/fetchatmos.sh so that the demo share has real DOS
software in it rather than generated filler.

They are redistributable; see each archive for its own licence. Nothing
here is abandonware and nothing here is a cracked commercial game -- a
warez client does not need to ship warez.
EOF
