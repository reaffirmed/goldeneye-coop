#!/bin/bash
# Two-player spot checks where the joiner is the one acting and the host stays
# at the start, over the mock Steam (coop/mock), for CI.
#
#   coop/ci/joiner_spots.sh <build dir> <out dir> [check ...]
#
# Checks (both players invulnerable, GE_CHEATS=invuln):
#   bungee   Dam: the joiner steps into the bungee room; the jump's objective
#            completes and both games debrief.
#   dronegun Bunker 2: the joiner stands in front of a drone gun; the host's
#            copy of the gun sees and fires at the joiner.
#   hostage  Frigate: the joiner stands by hostage 44 and aims at
#            hostage-taker 1; he turns on the joiner.
#   natalya  Statue: the helicopter trigger is set and the joiner goes to
#            Natalya; she gets up and objective D completes.
# Runs PAR checks at once (default 2). Exits 1 if any failed; out/summary.txt
# says which and why, out/<check>.host.log and .join.log hold the logs.
BUILD=$(cd "$1" && pwd); OUT=$(mkdir -p "$2" && cd "$2" && pwd); shift 2
ALL="bungee dronegun hostage natalya"
CHECKS=${*:-${CHECKS:-$ALL}}
PAR=${PAR:-2}; LIMIT=${LIMIT:-150}

[ -f "$BUILD/Data/baserom.u.z64" ] || { echo "no $BUILD/Data/baserom.u.z64"; exit 2; }
[ -f "$BUILD/mock/libsteam_api.so" ] || { echo "no $BUILD/mock/libsteam_api.so"; exit 2; }
cd "$BUILD"
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 LD_LIBRARY_PATH=$BUILD/mock \
    GE_CHEATS=invuln

# check <name>: stage, host env, joiner env, the log that must show it, the line
spec() {
    case $1 in
        bungee)   echo "dam|GE_COOP_TESTWIN=1|GE_COOP_TESTPAD=330:10|host|objective 3 (A) is now complete" ;;
        dronegun) echo "bunker2|GE_SNDDBG=1|GE_SPAWNXYZ=-3740,600,2750|host|agstat .*pos=(-3740 642 3087).* sees=1 los=1" ;;
        hostage)  echo "frigate||GE_COOP_TESTGOTO=44 GE_COOP_TESTAIMCHR=1:8|host|guard 1 \(now targets\|hit\) player 2" ;;
        natalya)  echo "statue|GE_COOP_TESTFLAG=8000:5|GE_COOP_TESTGOTO=0|host|objective 3 (D) is now complete" ;;
    esac
}

# run_check <slot> <name>: one pair; prints "<name> PASS|FAIL <why> <secs>"
run_check() {
    local slot=$1 ck=$2 hd=$((30 + 2 * $1)) jd=$((31 + 2 * $1))
    local a=$((76561190000003000 + 10 * slot + 1)) b=$((76561190000003000 + 10 * slot + 2))
    local mock=/tmp/gecoop-spot-$slot hl=$OUT/$ck.host.log jl=$OUT/$ck.join.log
    local t0=$SECONDS why="" hp jp xh xj st henv jenv log want
    IFS='|' read -r st henv jenv log want <<< "$(spec $ck)"
    [ -n "$st" ] || { echo "$ck FAIL: no such check 0s"; return; }
    rm -rf "$mock"
    Xvfb :$hd -screen 0 640x480x24 >/dev/null 2>&1 & xh=$!
    Xvfb :$jd -screen 0 640x480x24 >/dev/null 2>&1 & xj=$!
    sleep 1
    env DISPLAY=:$hd GE_MOCK_DIR=$mock GE_MOCK_STEAMID=$a GE_MOCK_FRIEND=$b GE_STAGE=$st $henv \
        timeout -s KILL $LIMIT ./ProjectMidas -coophost > "$hl" 2>&1 & hp=$!
    sleep 2
    env DISPLAY=:$jd GE_MOCK_DIR=$mock GE_MOCK_STEAMID=$b GE_MOCK_FRIEND=$a $jenv \
        timeout -s KILL $LIMIT ./ProjectMidas -coopjoin > "$jl" 2>&1 & jp=$!
    local wl=$hl
    [ "$log" = join ] && wl=$jl
    while kill -0 $hp 2>/dev/null && kill -0 $jp 2>/dev/null; do
        grep -q "$want" "$wl" && break
        sleep 1
    done
    if [ "$ck" = bungee ]; then
        while kill -0 $hp 2>/dev/null && kill -0 $jp 2>/dev/null && [ $((SECONDS - t0)) -lt $LIMIT ]; do
            grep -q "debrief: stage" "$hl" && grep -q "debrief: stage" "$jl" && break
            sleep 1
        done
        grep -q "debrief: stage" "$hl" || why="host did not debrief"
        grep -q "debrief: stage" "$jl" || why="${why:+$why; }joiner did not debrief"
    fi
    kill $hp $jp 2>/dev/null; wait $hp $jp 2>/dev/null
    kill $xh $xj 2>/dev/null
    grep -q "following the host into stage" "$jl" || why="${why:+$why; }joiner never followed the host in"
    grep -q "$want" "$wl" || why="${why:+$why; }no \"$want\" in the $log's log"
    if grep -q "unresolved desync" "$hl" "$jl"; then why="${why:+$why; }unresolved desync"; fi
    if grep -qi "segmentation\|signal 11\|assert\|crash" "$hl" "$jl"; then why="${why:+$why; }crash in log"; fi
    if [ -z "$why" ]; then
        echo "$ck PASS $((SECONDS - t0))s"
    else
        echo "$ck FAIL: $why $((SECONDS - t0))s"
    fi
}

: > "$OUT/summary.txt"
set -- $CHECKS
while [ $# -gt 0 ]; do
    for slot in $(seq 0 $((PAR - 1))); do
        [ $# -gt 0 ] || break
        run_check $slot $1 >> "$OUT/summary.txt" &
        shift
    done
    wait
done
cat "$OUT/summary.txt"
! grep -q " FAIL" "$OUT/summary.txt"
