#!/bin/bash
# Co-op warnings ratchet, for CI.
#
#   coop/ci/warnings_check.sh <configured build dir> [--update]
#
# Compiles the co-op files again with warnings on (the coop_warnings target in
# CMakeLists.txt; the game itself builds with -w and is not touched) and keeps
# the warnings located in a co-op file (src/game/coop*, port/src/coop*,
# coop/*). Each is keyed as "<file>: <message> [-Wflag]", without line
# numbers, and counted. Fails when any key is new or more frequent than in
# coop/ci/warnings-baseline.txt, so new co-op code can't add warnings, while
# the ones already there are worked down. A key that dropped below its
# baseline is reported, not failed: run with --update to lower the baseline.
# Also fails if the target doesn't build.
set -o pipefail
BUILD=$(cd "$1" && pwd) || exit 2
SRC=$(cd "$(dirname "$0")/../.." && pwd)
BASE=$SRC/coop/ci/warnings-baseline.txt
LOG=$BUILD/coop_warnings.log
NOW=$BUILD/coop_warnings.now

# objects are cached: force every co-op file to compile so every warning shows
find "$BUILD/CMakeFiles/coop_warnings.dir" -name '*.o' -delete 2>/dev/null
if ! cmake --build "$BUILD" --target coop_warnings -j"$(nproc)" > "$LOG" 2>&1; then
    grep -E "error:" "$LOG" | head -20
    echo "coop_warnings: the co-op files did not compile (full log: $LOG)"
    exit 1
fi

# "<path>:<line>:<col>: warning: <message> [-Wflag]" in a co-op file -> key
grep -E "^[^ :]+:[0-9]+:[0-9]+: warning: " "$LOG" \
    | sed -E "s|^$SRC/||; s|^([^:]+):[0-9]+:[0-9]+: warning: |\1: |; s|[‘’]|'|g" \
    | grep -E "^(src/game/coop[^/:]*|port/src/coop[^/:]*|coop/[^:]*): " \
    | sort | uniq -c | sed -E 's/^ *([0-9]+) (.*)$/\1\t\2/' > "$NOW" || : > "$NOW"

if [ "$2" = "--update" ]; then
    { echo "# coop/ci/warnings_check.sh baseline: <count><TAB><file>: <message> [-Wflag]"
      echo "# Lower it with --update after fixing warnings; never raise it by hand."
      cat "$NOW"; } > "$BASE"
    echo "coop_warnings: baseline written, $(awk -F'\t' '{n+=$1} END {print n+0}' "$NOW") warnings in $(wc -l < "$NOW") keys"
    exit 0
fi
[ -f "$BASE" ] || { echo "no $BASE: run with --update first"; exit 2; }

# compare by key: new or higher counts fail, lower ones are reported
awk -F'\t' '
    FNR == NR { if ($0 !~ /^#/ && NF == 2) base[$2] = $1; next }
    { now[$2] = $1 }
    END {
        for (k in now) {
            b = (k in base) ? base[k] : 0
            if (now[k] > b) { printf "NEW\t%d (was %d)\t%s\n", now[k], b, k; bad++ }
        }
        for (k in base) {
            n = (k in now) ? now[k] : 0
            if (n < base[k]) printf "FIXED\t%d (was %d)\t%s\n", n, base[k], k
        }
        exit bad > 0
    }' "$BASE" "$NOW" > "$BUILD/coop_warnings.diff"
rc=$?
total=$(awk -F'\t' '{n+=$1} END {print n+0}' "$NOW")
if grep -q "^FIXED" "$BUILD/coop_warnings.diff"; then
    grep "^FIXED" "$BUILD/coop_warnings.diff" | sed 's/^/  /'
    echo "coop_warnings: some warnings are gone; lower the baseline with: coop/ci/warnings_check.sh <build dir> --update"
fi
if [ $rc -ne 0 ]; then
    grep "^NEW" "$BUILD/coop_warnings.diff" | sed 's/^/  /'
    echo "coop_warnings: FAILED: co-op code has warnings that aren't in coop/ci/warnings-baseline.txt."
    echo "  Fix them (the lines are in $LOG). An 'implicit declaration' of a function"
    echo "  taking or returning a float is a real bug in this build, not noise."
    exit 1
fi
echo "coop_warnings: ok, $total warnings, none new"
