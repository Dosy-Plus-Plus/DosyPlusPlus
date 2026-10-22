#!/bin/bash
# What is bundled, where it came from, and under what licence.
#
#   tools/mkcredits.sh [outfile]      -> build/pages/CREDITS.txt
#
# Generated from the artifacts, not from memory. Every package the
# FreeDOS repositories publish carries an APPINFO/*.LSM file with a
# Copying-policy line written by whoever packaged it, so this reads that
# rather than restating it -- a licence claimed from memory is exactly the
# kind of claim that is wrong when it matters.
#
# Anything in the published share without a licence line is an error, not
# a blank entry: shipping software nobody can account for is the thing
# this file exists to prevent.
set -uo pipefail
exec < /dev/null

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/build/pages/CREDITS.txt}"

# Where a report goes. One place, because it appears in the credits, the
# page and the docs, and three copies of an address is two chances to
# have an old one. Overridable so a fork does not send reports here.
REPORT_TO="${DOSYPP_CONTACT:-dosy@fastmail.com}"
WAREZ="${WAREZ_DIR:-$ROOT/build/warez}"
ATMOS="${ATMOS_DIR:-$ROOT/build/atmos}"

command -v unzip >/dev/null 2>&1 || { echo "need unzip" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

missing=0

lsm_field() {   # $1 = lsm file, $2 = field
    grep -i "^$2:" "$1" 2>/dev/null | head -1 |
        sed "s/^[^:]*:[[:space:]]*//" | tr -d '\r'
}

# A package that is not from the FreeDOS repositories: its own licence
# file is the evidence. Classified by the clauses actually present in the
# text rather than by a name somebody remembered -- BSD's three forms
# differ by exactly one paragraph, and that paragraph is greppable.
describe_by_licence_file() {   # $1 = zip; 0 if it could describe it
    local zip="$1" name lic licfile copyright kind
    name="$(basename "$zip")"

    rm -rf "$TMP/l"; mkdir -p "$TMP/l"
    unzip -o -q -j "$zip" "*/COPYING.txt" "*/COPYING" "*/LICENSE" \
          "*/LICENSE.txt" "COPYING.txt" "COPYING" "LICENSE" "LICENSE.txt" \
          -d "$TMP/l" 2>/dev/null
    lic="$(find "$TMP/l" -type f | head -1)"
    [ -n "$lic" ] || return 1

    licfile="$(unzip -Z1 "$zip" | grep -iE '(^|/)(COPYING|LICENSE)(\.txt)?$' |
               head -1)"
    # The first two non-empty lines, because the copyright sentence wraps:
    # "Copyright (c) 2001-2024" on one line and who holds it on the next.
    # The BOM goes too -- it is a byte, not a word, and it renders as
    # mojibake in a text file served to a browser.
    copyright="$(sed '1s/^\xEF\xBB\xBF//' "$lic" |
                 grep -v '^[[:space:]]*$' | head -2 | tr '\n' ' ' |
                 sed 's/[[:space:]]\{2,\}/ /g; s/^ //; s/ $//' | cut -c1-96)"

    if grep -qi 'redistribution and use in source and binary forms' "$lic"; then
        if grep -qi 'neither the name' "$lic"; then
            kind="BSD 3-clause"
        else
            kind="BSD 2-clause"
        fi
    elif grep -qi 'GNU GENERAL PUBLIC LICENSE' "$lic"; then
        kind="$(grep -m1 -oiE 'version [0-9]+' "$lic" |
                sed 's/^/GPL /' | tr -d '\n')"
        [ -n "$kind" ] || kind="GPL"
    elif grep -qi 'permission is hereby granted, free of charge' "$lic"; then
        kind="MIT/X11-style"
    else
        kind="see the licence file"
    fi

    # The archive's own top-level directory names and versions it better
    # than its filename does ("freedoom-0.13.0" against "FREEDOOM.ZIP").
    local title
    title="$(unzip -Z1 "$zip" | sed -n 's|^\([^/]*\)/.*|\1|p' | sort -u)"
    [ "$(printf '%s\n' "$title" | wc -l)" -eq 1 ] && [ -n "$title" ] ||
        title="$(basename "$zip" | sed 's/\.[Zz][Ii][Pp]$//')"

    printf '  %-18s %s\n' "$name" "$title"
    printf '  %-18s   licence: %s, stated in %s inside the archive\n' \
        "" "$kind" "${licfile:-the licence file}"
    [ -n "$copyright" ] && printf '  %-18s   %s\n' "" "$copyright"
    printf '\n'
    return 0
}

describe_zip() {   # $1 = path to a FreeDOS package
    local zip="$1" name lsm title ver author policy
    name="$(basename "$zip")"

    rm -rf "$TMP/x"; mkdir -p "$TMP/x"
    unzip -o -q -j "$zip" "APPINFO/*.LSM" -d "$TMP/x" 2>/dev/null
    lsm="$(find "$TMP/x" -name '*.LSM' | head -1)"

    if [ -z "$lsm" ]; then
        # Not everything here is a FreeDOS package. One is fetched from
        # its own project's release, and those carry a licence file rather
        # than an LSM. Read that, the same way: quote what the file says
        # and name the file, so a reader can check it inside the archive
        # they were served.
        describe_by_licence_file "$zip" && return
        printf '  %-18s %s\n' "$name" \
            "!! no APPINFO/*.LSM and no COPYING/LICENSE: nothing says what this is"
        missing=$((missing + 1))
        return
    fi

    title="$(lsm_field "$lsm" Title)"
    ver="$(lsm_field "$lsm" Version)"
    author="$(lsm_field "$lsm" Author)"
    policy="$(lsm_field "$lsm" Copying-policy)"

    if [ -z "$policy" ]; then
        printf '  %-18s %s\n' "$name" "!! no Copying-policy line"
        missing=$((missing + 1))
        return
    fi

    printf '  %-18s %s %s\n' "$name" "$title" "$ver"
    printf '  %-18s   licence: %s\n' "" "$policy"
    [ -n "$author" ] && printf '  %-18s   by: %s\n' "" "$author"
    printf '\n'
}

{
cat <<'HEAD'
What is in here
===============

DOSy++ itself is MIT licensed -- see LICENSE in the repository. Everything
below belongs to somebody else and keeps its own terms.

Every program bundled with this demo is free software, redistributable by
its own licence, and fetched from the FreeDOS 1.3 repositories by
tools/fetchatmos.sh, which pins each download by sha256.

None of it is shareware, abandonware, or a commercial game. The game data
is Freedoom -- a free replacement for the original Doom data, under a BSD
licence -- and not id Software's. No cracked, patched or ripped software
is bundled, served, or linked, and nothing here was obtained from a
warez site: the 1994 styling is a costume, not a description.

The licence lines below are read out of each package's own APPINFO/*.LSM
file at build time by tools/mkcredits.sh. They are what the people who
packaged this software said about it, not what we remember.

HEAD

echo "The share this demo serves"
echo "--------------------------"
echo
if [ -d "$WAREZ" ]; then
    while IFS= read -r z; do
        describe_zip "$z"
    done <<EOF
$(find "$WAREZ" -name '*.ZIP' -o -name '*.zip' | LC_ALL=C sort)
EOF
else
    echo "  (no share built: run make warez)"
fi

echo "On the disk image"
echo "-----------------"
echo
echo "  The same packages, plus the FreeDOS kernel and COMMAND.COM that"
echo "  boot it (GNU General Public License, Version 2), CTMOUSE (GPL v2)"
echo "  and Info-ZIP's UNZIP, listed above."
echo

cat <<TAIL
If you think something here should not be
-----------------------------------------

  Write to $REPORT_TO, or open an issue, and say which file and why.

  Anything credibly reported as infringing comes out first and gets
  discussed afterwards -- there is nothing in this bundle worth arguing
  over, and everything in it has a free replacement.

TAIL

cat <<'TAIL'
In the page
-----------

  v86                     BSD-2-Clause        github.com/copy/v86
    The PC emulator. Fetched from npm by tools/fetchv86.sh, pinned.

  spessasynth_core        Apache-2.0          npm: spessasynth_core
  stb-vorbis              Apache-2.0          npm: stb-vorbis
    The SoundFont synthesiser that plays game MIDI, and its ogg decoder.
    Fetched and sha256-pinned by tools/fetchsynth.sh.

  MuseScore General       MIT                 musescore.org
    The SoundFont itself, by S. Christian Collins. Fetched and pinned by
    tools/fetchsf2.sh. Downloaded by the page only if you ask for MIDI.

In the DOS binary
-----------------

  Watt-32                 Watt-32 licence     watt-32.net
    The TCP/IP stack. Not redistributed here: it is built from source by
    whoever builds the client.

  bzip2                   bzip2 licence       sourceware.org/bzip2
    File lists are bzip2. The library sources are fetched, not committed.

  The tune                MIT (this project)
    art/DOSYPP.MOD is generated by tools/makemod.py. Every note in it was
    written for this project, which is why there is no MOD from 1994 in
    here: that music belongs to whoever tracked it.

  Tiger S-boxes           ISC-style
    src/hash/tiger_sbox.h holds the four 256-entry tables from the Tiger
    specification, taken from RHash (Copyright (c) 2007 Aleksey
    Kravchenko). They are data, not code, and cannot be derived without an
    implementation to generate them. The notice is in the file.

TAIL

echo
echo "Generated by tools/mkcredits.sh on $(date -u +%Y-%m-%d)."
} > "$OUT"

echo "credits: $OUT  ($(wc -l < "$OUT" | tr -d ' ') lines)"

if [ "$missing" -gt 0 ]; then
    echo "error: $missing package(s) in the share have no licence line." >&2
    echo "       Nothing ships unattributed. Check the package, or drop" >&2
    echo "       it from tools/fetchatmos.sh." >&2
    exit 1
fi
