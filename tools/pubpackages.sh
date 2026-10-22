#!/bin/bash
# Publish the FreeDOS packages the demo shares as release assets.
#
# Why: the per-package FreeDOS 1.3 repositories exist in exactly one place,
# ibiblio, and it answers 503 for the whole subtree under load. When it
# does, tools/fetchatmos.sh cannot fetch anything, `make warez` has nothing
# to share, and CI goes red for a reason that has nothing to do with this
# project. A copy on a release this repository controls removes that
# dependency.
#
# What this is allowed to publish: free software, redistributable under its
# own licence, which is the same position docs/LICENSING.md sets out for
# the demo itself. Each file is verified against the sha256 that
# fetchatmos.sh pins before it is uploaded, so what goes up is what the
# pins describe -- and because fetchatmos.sh checks the same hashes on the
# way back down, a release is not something anybody has to trust.
#
#   ./tools/pubpackages.sh                  publish to the default tag
#   ./tools/pubpackages.sh --dry-run        say what it would do
#   PKG_TAG=freedos-1.3-packages ./tools/pubpackages.sh
#
# Then point the build at it:
#
#   FREEDOS_BASE=https://github.com/<org>/<repo>/releases/download/<tag>
#   FREEDOS_BASE_ALT=https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CACHE="${DOSYPP_CACHE:-$ROOT/.cache}"
TAG="${PKG_TAG:-freedos-1.3-packages}"
DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1

command -v gh >/dev/null 2>&1 || {
    echo "need the gh CLI (brew install gh), and gh auth login" >&2; exit 1; }

# The package list and its hashes live in one place: the fetcher. Parsed
# from it rather than copied, so the two cannot drift.
LIST=$(grep -E '^[a-z0-9]+/[a-z0-9]+\.zip[[:space:]]' "$ROOT/tools/fetchatmos.sh")
[ -n "$LIST" ] || { echo "could not read the package list from tools/fetchatmos.sh" >&2; exit 1; }

missing=0
files=()
while read -r path _area want_sha _rest; do
    [ -z "$path" ] && continue
    name="$(basename "$path")"
    f="$CACHE/$name"
    if [ ! -f "$f" ]; then
        echo "  $name: not in $CACHE -- run ./tools/fetchatmos.sh first" >&2
        missing=$((missing + 1))
        continue
    fi
    got=$(shasum -a 256 "$f" 2>/dev/null | cut -d' ' -f1)
    [ -z "$got" ] && got=$(sha256sum "$f" | cut -d' ' -f1)
    if [ "$got" != "$want_sha" ]; then
        echo "  $name: hash does not match the pin -- refusing to publish it" >&2
        echo "      want $want_sha" >&2
        echo "      got  $got" >&2
        missing=$((missing + 1))
        continue
    fi
    printf '  %-16s %8s KB  sha256 ok\n' "$name" "$(( $(wc -c < "$f") / 1024 ))"
    files+=("$f")
done <<< "$LIST"

[ "$missing" -eq 0 ] || { echo "$missing package(s) not publishable -- stopping" >&2; exit 1; }
echo "${#files[@]} packages verified against the pins in tools/fetchatmos.sh"

REPO=$(gh repo view --json nameWithOwner -q .nameWithOwner)
echo "repository: $REPO    tag: $TAG"

if [ "$DRY" = 1 ]; then
    echo "(dry run: nothing uploaded)"
    echo "FREEDOS_BASE would be https://github.com/$REPO/releases/download/$TAG"
    exit 0
fi

if ! gh release view "$TAG" >/dev/null 2>&1; then
    gh release create "$TAG" --title "FreeDOS 1.3 packages (mirror)" --notes \
"The free DOS software the demo shares, copied from the FreeDOS 1.3
repositories so that a build does not depend on one host being up.

Every file here is redistributable under its own licence; CREDITS.txt in
the published demo lists each one with the licence its own authors
recorded in it. Nothing here is modified -- each file matches the sha256
pinned in tools/fetchatmos.sh, which checks them again on the way down.

Upstream: https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3"
fi

gh release upload "$TAG" "${files[@]}" --clobber
echo
echo "done. Point the build at it with:"
echo "  FREEDOS_BASE=https://github.com/$REPO/releases/download/$TAG"
