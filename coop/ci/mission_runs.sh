#!/bin/bash
# Two-player runs of every mission over the mock Steam (coop/mock), for CI.
#
#   coop/ci/mission_runs.sh <build dir> <out dir> [stage ...]
#
# For each stage the host launches straight into it (GE_STAGE) and the joiner
# follows it in from the menus over a lossy, jittery link (GE_COOP_NETSIM).
# GE_COOP_TESTEND ends the mission on the host once the joiner is in, and
# GE_COOP_TESTWIN counts that as a win. A stage passes when the joiner's log
# shows it followed the host in, applied the join snapshot and completed the
# mission with the host, and neither copy logged an unresolved desync or died.
# Runs PAR stages at once (default 2), each pair on its own displays, mock
# folder and ids. The build dir needs Data/baserom.u.z64 and
# mock/libsteam_api.so. Exits 1 if any stage failed; out/summary.txt says
# which and why, out/<stage>.host.log and .join.log hold the logs.
#
# Env: STAGES (instead of arguments), PAR, TESTEND (mission ticks before the
# end, default 900), NETSIM (joiner link, default 60:20:2), LIMIT (seconds a
# stage may take, default 240), DIFF (0..3, default 0).
BUILD=$(cd "$1" && pwd); OUT=$(mkdir -p "$2" && cd "$2" && pwd); shift 2
ALL="dam facility runway surface bunker1 silo frigate surface2 bunker2 statue archives streets depot train jungle control caverns cradle aztec egypt"
STAGES=${*:-${STAGES:-$ALL}}
PAR=${PAR:-2}; TESTEND=${TESTEND:-900}; NETSIM=${NETSIM:-60:20:2}; LIMIT=${LIMIT:-240}; DIFF=${DIFF:-0}

[ -f "$BUILD/Data/baserom.u.z64" ] || { echo "no $BUILD/Data/baserom.u.z64"; exit 2; }
[ -f "$BUILD/mock/libsteam_api.so" ] || { echo "no $BUILD/mock/libsteam_api.so"; exit 2; }
cd "$BUILD"
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 LD_LIBRARY_PATH=$BUILD/mock

# run_stage <slot> <stage>: one pair; prints "<stage> PASS|FAIL <why> <secs>"
run_stage() {
    local slot=$1 st=$2 hd=$((10 + 2 * $1)) jd=$((11 + 2 * $1))
    local a=$((76561190000001000 + 10 * slot + 1)) b=$((76561190000001000 + 10 * slot + 2))
    local mock=/tmp/gecoop-ci-$slot hl=$OUT/$st.host.log jl=$OUT/$st.join.log
    local t0=$SECONDS why="" hp jp xh xj
    rm -rf "$mock"
    Xvfb :$hd -screen 0 640x480x24 >/dev/null 2>&1 & xh=$!
    Xvfb :$jd -screen 0 640x480x24 >/dev/null 2>&1 & xj=$!
    sleep 1
    DISPLAY=:$hd GE_MOCK_DIR=$mock GE_MOCK_STEAMID=$a GE_MOCK_FRIEND=$b GE_STAGE=$st GE_DIFF=$DIFF \
        GE_COOP_TESTEND=$TESTEND GE_COOP_TESTWIN=1 timeout -s KILL $LIMIT ./ProjectMidas -coophost > "$hl" 2>&1 & hp=$!
    sleep 2
    DISPLAY=:$jd GE_MOCK_DIR=$mock GE_MOCK_STEAMID=$b GE_MOCK_FRIEND=$a GE_COOP_NETSIM=$NETSIM \
        timeout -s KILL $LIMIT ./ProjectMidas -coopjoin > "$jl" 2>&1 & jp=$!
    # done once the joiner completed the mission (and had a few seconds after), or a copy is gone
    while kill -0 $hp 2>/dev/null && kill -0 $jp 2>/dev/null; do
        if grep -q "peer completed the mission\|mission complete, telling the peer" "$jl"; then
            sleep 5; break
        fi
        sleep 1
    done
    if [ $((SECONDS - t0)) -ge $LIMIT ]; then why="timed out after ${LIMIT}s"
    else
        kill -0 $hp 2>/dev/null || why="host exited early"
        kill -0 $jp 2>/dev/null || why="${why:-joiner exited early}"
    fi
    kill $hp $jp 2>/dev/null; wait $hp $jp 2>/dev/null
    kill $xh $xj 2>/dev/null
    grep -q "following the host into stage" "$jl" || why="${why:+$why; }joiner never followed the host in"
    grep -q "repl: join snapshot applied" "$jl" || why="${why:+$why; }no join snapshot"
    grep -q "peer completed the mission\|mission complete, telling the peer" "$jl" || why="${why:+$why; }joiner did not complete the mission"
    if grep -q "unresolved desync" "$hl" "$jl"; then why="${why:+$why; }unresolved desync"; fi
    if grep -qi "segmentation\|signal 11\|assert\|crash" "$hl" "$jl"; then why="${why:+$why; }crash in log"; fi
    local fixes=$(grep -h "repl: desync" "$hl" "$jl" | wc -l)
    if [ -z "$why" ]; then
        echo "$st PASS ($fixes corrected desyncs) $((SECONDS - t0))s"
    else
        echo "$st FAIL: $why $((SECONDS - t0))s"
    fi
}

: > "$OUT/summary.txt"
set -- $STAGES
while [ $# -gt 0 ]; do
    for slot in $(seq 0 $((PAR - 1))); do
        [ $# -gt 0 ] || break
        run_stage $slot $1 >> "$OUT/summary.txt" &
        shift
    done
    wait
done
cat "$OUT/summary.txt"
! grep -q " FAIL" "$OUT/summary.txt"
