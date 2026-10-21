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
games/freedoom.zip     WAREZ/GAMES  b0352ccc4e7f85b39651c38f8656396bd537bdcd3cca1127dbf06e3e871ed720  FreeDoom, a free Doom-compatible game
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
total=0
printf '%-18s %-13s %s\n' FILE AREA SIZE

while read -r path area want_sha _rest; do
    [ -z "$path" ] && continue
    name="$(basename "$path")"
    out="$CACHE/$name"

    if [ ! -f "$out" ]; then
        # --fail so a 404 does not leave an HTML error page named .zip,
        # which is the failure mode that wastes an afternoon: the file
        # exists, has the right name, and is not a zip.
        curl -fsSL -m 300 -o "$out.part" "$BASE/$path" || {
            echo "could not fetch $BASE/$path" >&2
            rm -f "$out.part"
            continue
        }
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
    cp "$out" "$DEST/$area/$(echo "$name" | tr 'a-z' 'A-Z')"
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
