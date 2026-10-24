#!/bin/bash
# Assemble what somebody downloads: the DOS client, and the proxy that
# lets it reach a TLS hub.
#
# Two archives and a bare executable, because there are two audiences and
# one shortcut:
#
#   dosypp-<ver>-dos.zip        the client, CWSDPMI, a config and a README
#   dosypp-<ver>-adc-proxy.zip  the ADC/ADCS proxy, and the browser relay
#   DOSYPP.EXE                  for people who already have a DPMI host
#
# What is deliberately not in here: packet drivers. NE2000.COM and
# PCIPKT.COM are other people's work under their own terms -- see
# docs/PKTDRV.md and `make pktdrv` -- and a release that quietly
# redistributes them is the kind of thing docs/LICENSING.md exists to stop.
# CWSDPMI is in, because its own documentation permits redistribution
# without source provided its notice travels with it, which it does.
#
#   ./tools/mkrelease.sh              -> build/release/
#   ./tools/mkrelease.sh --no-dos-check   skip booting the packaged EXE
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-$ROOT/build/release}"
CACHE="${DOSYPP_CACHE:-$ROOT/.cache}"
EXE="$ROOT/build/dos/DOSYPP.EXE"
VER=$(sed -n 's/.*DOSYPP_VERSION "\([^"]*\)".*/\1/p' "$ROOT/src/common.h")
CHECK=1
[ "${1:-}" = "--no-dos-check" ] && CHECK=0

die() { echo "error: $*" >&2; exit 1; }

[ -n "$VER" ] || die "could not read DOSYPP_VERSION from src/common.h"
[ -f "$EXE" ] || die "no $EXE
       run: make dos-docker"
command -v zip >/dev/null 2>&1 || die "zip not found"

# The binary has to be newer than the source it claims to be built from,
# or the release ships yesterday's client with today's version number.
STALE=$(find "$ROOT/src" -newer "$EXE" -name '*.[ch]' 2>/dev/null | head -3)
[ -z "$STALE" ] || die "these are newer than $EXE:
$(echo "$STALE" | sed 's/^/       /')
       run: make dos-docker"

rm -rf "$OUT"; mkdir -p "$OUT/dos" "$OUT/proxy"

echo "DOSy++ $VER"
echo

# ---------------------------------------------------------------- the client
cp "$EXE" "$OUT/dos/DOSYPP.EXE"

if [ -f "$CACHE/csdpmi7b.zip" ]; then
    unzip -o -q -j "$CACHE/csdpmi7b.zip" "bin/CWSDPMI.EXE" "bin/cwsdpmi.doc" \
          -d "$OUT/dos"
else
    echo "  no $CACHE/csdpmi7b.zip -- run ./tools/fetchdos.sh first" >&2
    exit 1
fi

cat > "$OUT/dos/DOSYPP.INI" <<EOF
; DOSy++ $VER -- a configuration to start from.
; Everything here has a default and a row in the settings door (9).

hub       = adc://localhost:1511
nick      = Warlord
share     = C:\\DL
active    = no
; A port to listen on, if you set active = yes and can forward it.
;port.listen = 14141
sound     = yes
music     = yes
EOF

cp "$ROOT/LICENSE" "$OUT/dos/LICENSE.TXT"

cat > "$OUT/dos/README.TXT" <<EOF
DOSy++ $VER -- an ADC (Direct Connect) client for MS-DOS
=========================================================

What is here
------------
  DOSYPP.EXE    the client. 32-bit protected mode, built with DJGPP.
  CWSDPMI.EXE   the DPMI host it needs. Keep it in the same directory, or
                anywhere on the PATH.
  DOSYPP.INI    a configuration to start from.
  LICENSE.TXT   DOSy++ is MIT licensed.
  cwsdpmi.doc   CWSDPMI's own documentation and terms.

What you also need, and why it is not here
------------------------------------------
A packet driver for your network card. They are other people's work under
their own licences, so this archive does not carry them:

  * Crynwr's NE2000.COM for ISA NE2000 cards, and the rest of that family
  * the Realtek PCI driver (PCIPKT.COM) for RTL8029 and friends

See docs/PKTDRV.md in the repository, or run \`make pktdrv\`, which fetches
the right one for the target you are using.

Running it
----------
  1. Load your packet driver, however it wants to be loaded.
  2. Edit DOSYPP.INI -- at least the hub address and your handle.
  3. DOSYPP

TLS hubs (adcs://) need the proxy from dosypp-$VER-adc-proxy.zip: there is
no TLS in the DOS client, so something on the network side terminates it.

Try it without a DOS machine
----------------------------
  https://dosy-plus-plus.github.io/DosyPlusPlus/

CWSDPMI V0.90+ (r7) Copyright (C) 2010 CW Sandmann  ABSOLUTELY NO WARRANTY
Redistributed under the terms in cwsdpmi.doc, which permit it without
source provided this notice travels with it.
EOF

# ----------------------------------------------------------------- the proxy
cp "$ROOT/tools/tlsrelay.py" "$ROOT/tools/wisprelay.py" "$OUT/proxy/"
cp "$ROOT/LICENSE" "$OUT/proxy/LICENSE.TXT"

cat > "$OUT/proxy/README.TXT" <<EOF
DOSy++ $VER -- the proxies
===========================

The DOS client speaks plaintext ADC. Two things here put it in front of
networks that want something else. Both are single Python 3 files with no
dependencies.

tlsrelay.py -- ADC to ADCS
--------------------------
Most public hubs are TLS-only now, and there is no TLS in the DOS client
(see the entry about a native stack in the repository's notes). This
listens in plaintext, connects out over TLS, and passes ADC through:

  ./tlsrelay.py --listen 0.0.0.0:1511 --connect hub.example.org:2780

Then point DOSYPP.INI at the machine running it. Bind it to the LAN
address rather than loopback if the DOS box is a different machine --
and note that the LAN leg is plaintext, so the password handshake is
visible on that network. On a trusted home LAN that is a fair trade; on
anything shared it is not.

wisprelay.py -- for the browser host
------------------------------------
What the in-browser demo uses: a Wisp relay and a static file server, so
a machine emulated in a tab gets a TCP socket. Only needed if you are
hosting the browser version yourself; the published demo needs no relay
at all, because the hub runs in the page.

  ./wisprelay.py --map 10.77.0.1=127.0.0.1:1511

Both are MIT, like the rest of DOSy++. See LICENSE.TXT.
EOF

# ---------------------------------------------------------------- the archives
( cd "$OUT/dos"   && zip -q -X -r "$OUT/dosypp-$VER-dos.zip" . )
( cd "$OUT/proxy" && zip -q -X -r "$OUT/dosypp-$VER-adc-proxy.zip" . )
cp "$EXE" "$OUT/DOSYPP.EXE"

# ---------------------------------------------------------------- does it run?
#
# A zip that unpacks is not a client that starts. This boots the packaged
# executable on DOSBox-X and reads its version back, which is the cheapest
# check that what went into the archive is a working DOS program and not,
# say, a host binary that happened to be in build/dos.
if [ "$CHECK" = 1 ] && command -v dosbox-x >/dev/null 2>&1; then
    RUN="$OUT/.check"; rm -rf "$RUN"; mkdir -p "$RUN/c"
    unzip -q "$OUT/dosypp-$VER-dos.zip" -d "$RUN/c"
    # --vgacheck prints the banner -- "DOSy++ <ver>" and the CID -- then
    # exits on its own, which is the shortest path to "this is a working
    # DOS program". There is no --version flag; the banner is printed at
    # startup before anything takes the screen.
    cat > "$RUN/c/GO.BAT" <<'BAT'
@echo off
DOSYPP.EXE --vgacheck > VER.TXT
BAT
    cat > "$RUN/dosbox.conf" <<EOF
[sdl]
output = surface
[dosbox]
machine = svga_s3
memsize = 16
[cpu]
core = normal
cputype = 486
[autoexec]
mount c "$RUN/c"
c:
call GO.BAT
exit
EOF
    # Bounded and headless, the same way the DOS rigs do it: macOS has no
    # timeout(1), and an executable that is not a DOS program does not
    # reach the "exit" in the autoexec -- it sits there, and without this
    # the release build hangs instead of failing. Found by feeding it a
    # host binary on purpose.
    #
    # The autoexec says `call GO.BAT`, not `GO.BAT`: without CALL the
    # batch does not return, the `exit` never runs, and a successful check
    # still sat here until the alarm went off.
    SDL_VIDEODRIVER=dummy perl -e 'alarm 120; exec @ARGV' \
        dosbox-x -conf "$RUN/dosbox.conf" -nogui >/dev/null 2>&1 || true
    if [ -f "$RUN/c/VER.TXT" ] && grep -qa "DOSy++ $VER" "$RUN/c/VER.TXT"; then
        echo "  the packaged EXE runs on DOS and reports:" \
             "$(grep -a 'DOSy++' "$RUN/c/VER.TXT" | head -1 | tr -d '\r')"
    else
        die "the packaged DOSYPP.EXE did not report its version on DOS.
       What it wrote: $( [ -f "$RUN/c/VER.TXT" ] && head -c 120 "$RUN/c/VER.TXT" || echo '(nothing)')"
    fi
    rm -rf "$RUN"
elif [ "$CHECK" = 1 ]; then
    echo "  (no dosbox-x, so the packaged EXE was not booted)" >&2
fi

echo
for f in "$OUT"/dosypp-*.zip "$OUT/DOSYPP.EXE"; do
    printf '  %-32s %6s KB\n' "$(basename "$f")" "$(( $(wc -c < "$f") / 1024 ))"
done
echo
echo "sha256:"
( cd "$OUT" && shasum -a 256 dosypp-*.zip DOSYPP.EXE 2>/dev/null ||
  sha256sum dosypp-*.zip DOSYPP.EXE ) | sed 's/^/  /'
