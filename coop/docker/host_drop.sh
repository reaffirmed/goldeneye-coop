#!/bin/bash
# Host migration test: N copies (PLAYERS=2..4) over the mock Steam, through
# the lobby into Dam like party_lobby.sh; then copy KILL (1 = the host) is
# killed with SIGKILL at KILLAT seconds, as a crash would (KILLAT=none:
# nobody is killed, for the GE_COOP_TESTABORT runs). With REJOIN=<t>
# that copy starts again at t and joins from the friend list. Logs and
# screenshots go to $OUT as p1.log ... and p<N>_<t>.png; a rejoined copy's
# log is p<KILL>b.log.
#   BUILD (build_pc folder), OUT (results), ALLENV / HOSTENV / P2ENV.. as in
#   party_lobby.sh, SHOTS (seconds), RUNSECS.
BUILD=${BUILD:-/home/dev/project/build_pc}
OUT=${OUT:-/out}
cd "$BUILD"
rm -rf /tmp/gecoop-mock
N=${PLAYERS:-3}
K=${KILL:-1}
IDS=(76561190000000101 76561190000000202 76561190000000303 76561190000000404)
for i in $(seq 1 $N); do Xvfb :$((i + 10)) -screen 0 960x540x24 >/dev/null 2>&1 & done
sleep 1
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 LD_LIBRARY_PATH=$PWD/mock GE_MENU=lobby ${ALLENV}
declare -A PID
start() {   # start <i> <host|join> <log>
    local i=$1 mode=$2 log=$3 me=${IDS[$(($1-1))]} friends envv v
    friends=$(for j in $(seq 1 $N); do [ $j != $i ] && printf '%s,' ${IDS[$((j-1))]}; done)
    if [ $mode = host ]; then envv="GE_COOP_LOBBY_PLAYERS=$N ${HOSTENV}"; else v=P${i}ENV; envv="${!v}"; fi
    DISPLAY=:$((i + 10)) env GE_MOCK_STEAMID=$me GE_MOCK_FRIEND=${friends%,} GE_COOP_LOBBY=$mode $envv \
        timeout ${RUNSECS:-150} ./ProjectMidas > "$OUT/$log" 2>&1 &
    PID[$i]=$!
}
for i in $(seq 1 $N); do
    if [ $i = 1 ]; then start 1 host p1.log; else start $i join p$i.log; fi
    sleep 2
done
events=$( (for t in ${SHOTS:-40 70 100}; do echo "$t shot"; done; [ "${KILLAT:-50}" != none ] && echo "${KILLAT:-50} kill"; [ -n "$REJOIN" ] && echo "$REJOIN rejoin") | sort -n)
prev=0
while read -r t what; do
    sleep $((t - prev)); prev=$t
    case $what in
        shot) for i in $(seq 1 $N); do DISPLAY=:$((i + 10)) import -window root "$OUT/p${i}_$t.png" 2>/dev/null; done ;;
        kill) echo "killing copy $K at $t s"; pkill -9 -P ${PID[$K]} ProjectMidas ;;
        rejoin) echo "starting copy $K again at $t s"; RUNSECS=$((${RUNSECS:-150} - t)) start $K join p${K}b.log ;;
    esac
done <<< "$events"
while pgrep -x ProjectMidas >/dev/null; do sleep 1; done; pkill Xvfb; exit 0
