#!/bin/bash
# Run every gate in the repository and report what passed.
#
# There are thirty-odd make targets that test something, in four tiers with
# different prerequisites, and "did I run them all after that change?" was
# being answered from memory. It was answered wrongly at least once: a
# sweep reported the tree green while dos-bench-run had never run, because
# its binaries were not built.
#
# Tiers, cheapest first, so a failure usually arrives in the first minute:
#
#   host    needs a C compiler and Python 3. Minutes.
#   big     the 5GB share rigs. Slow: hashing, mostly.
#   others  other people's implementations. Needs Docker.
#   dos     DOSBox-X, headless. Each a couple of minutes.
#   web     v86 in Node.
#
#   tools/sweep.sh              everything
#   tools/sweep.sh host dos     those tiers only
#   SWEEP_KEEP=1 tools/sweep.sh keep the logs of passing runs too
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOGS="${SWEEP_LOGS:-$ROOT/build/sweep}"
cd "$ROOT"

HOST_TARGETS="test test-relay test-transfer test-hash-mode test-browse
              test-resume test-segments test-setup test-hublist test-favs
              test-music test-commands test-attic test-selfbrowse
              test-oracle bench"
BIG_TARGETS="test-bigshare dos-bigshare-check"
OTHER_TARGETS="test-uhub test-adcs test-airdcpp"
DOS_TARGETS="dos-test-run dos-xfer-check dos-browse-check dos-share-check
             dos-hublist-check dos-music-check dos-midi-check dos-vga-check
             dos-rows-check dos-kbd-check dos-bench-run"
WEB_TARGETS="test-v86 test-v86-xfer test-v86-dl test-v86-state
             test-v86-sound test-v86-midi test-v86-doom test-v86-hublist
             test-v86-inpage test-v86-play test-pages"

tiers="${*:-host big others dos web}"

mkdir -p "$LOGS"
pass=0
fail=0
failed=

run_one() {
    local t="$1"
    local log="$LOGS/$t.log"
    local start=$(date +%s)

    printf '  %-22s ' "$t"
    if make "$t" > "$log" 2>&1; then
        printf 'ok   %3ds\n' $(( $(date +%s) - start ))
        pass=$((pass + 1))
        [ -z "${SWEEP_KEEP:-}" ] && rm -f "$log"
    else
        printf 'FAIL %3ds  %s\n' $(( $(date +%s) - start )) "$log"
        fail=$((fail + 1))
        failed="$failed $t"
        # What broke, now, because a sweep is long and scrolling back to
        # find out is time nobody has.
        #
        # The rigs collect their failed checks and print them under
        # "--- what failed ---" just before their verdict, so prefer that
        # over the tail: a rig that kills background processes on the way
        # out ends its log with four lines of the shell saying
        # "Terminated", which pushed the useful part out of view.
        if grep -q -- "--- what failed ---" "$log"; then
            sed -n '/--- what failed ---/,$p' "$log" |
                grep -vE "Terminated|run dir kept|^make(\[|:)" |
                head -8 | sed 's/^/      /'
        else
            sed 's/^/      /' <<< "$(tail -4 "$log")"
        fi
    fi
}

SWEEP_START=$(date +%s)

for tier in $tiers; do
    case "$tier" in
        host)   targets="$HOST_TARGETS" ;;
        big)    targets="$BIG_TARGETS" ;;
        others) targets="$OTHER_TARGETS" ;;
        dos)    targets="$DOS_TARGETS" ;;
        web)    targets="$WEB_TARGETS" ;;
        *)      echo "unknown tier: $tier" >&2; exit 2 ;;
    esac

    echo
    echo "--- $tier"
    for t in $targets; do
        run_one "$t"
    done
done

echo
printf 'swept %d targets in %dm%02ds: %d passed, %d failed\n' \
    $((pass + fail)) $(( ($(date +%s) - SWEEP_START) / 60 )) \
    $(( ($(date +%s) - SWEEP_START) % 60 )) "$pass" "$fail"

if [ "$fail" -gt 0 ]; then
    echo "failed:$failed" >&2
    exit 1
fi
