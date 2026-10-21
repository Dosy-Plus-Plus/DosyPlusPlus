#!/bin/bash
# Run a build command inside the DJGPP container, with the repo mounted.
#
#   tools/dosmake.sh              -> make dos
#   tools/dosmake.sh make clean
#   tools/dosmake.sh bash         -> poke around
#
# Build the image first:
#   docker build -f tools/docker/Dockerfile.djgpp -t dosypp-djgpp .
set -euo pipefail

IMAGE="${DOSYPP_IMAGE:-dosypp-djgpp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The daemon first: `docker image inspect` fails when it is down, so
# checking the image first blames the wrong thing.
if ! docker info >/dev/null 2>&1; then
    echo "error: Docker is not running, and this machine has no local" >&2
    echo "       DJGPP. Start Docker Desktop, or see docs/TOOLCHAIN.md" >&2
    echo "       for building the toolchain natively." >&2
    exit 1
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "error: image '$IMAGE' not found. Build it with:" >&2
    echo "  make djgpp-image" >&2
    echo "(about ten minutes: it builds GCC 12.2 for i586-pc-msdosdjgpp)" >&2
    exit 1
fi

# -t only when stdout is a terminal: with it unconditionally, any
# non-interactive caller (a test script, CI, an agent) fails with
# "cannot attach stdin to a TTY-enabled container".
TTY_FLAGS=
[ -t 0 ] && [ -t 1 ] && TTY_FLAGS=-it

exec docker run --rm $TTY_FLAGS \
    -v "$ROOT:/src" \
    -w /src \
    -u "$(id -u):$(id -g)" \
    "$IMAGE" \
    "${@:-make dos}"
