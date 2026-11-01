#!/bin/bash
# Network regression scenarios: 2..4 copies over the mock Steam (coop/mock),
# through the lobby into Dam (as coop/docker/host_drop.sh), each copy behind
# its own simulated line (GE_COOP_NETSIM, both directions when every copy
# sets one), with an optional cut line, killed copy and rejoin. The host
# walks (GE_AUTOSTICK) and every joiner fires (GE_COOP_TESTFIRE) so there is
# movement to smooth and events to deliver. Prints one summary block of what
# the logs say; the logs stay in $OUT.
#
# This is a simulation on one machine: the "line" is a delay, jitter and loss
# applied to packets that really arrive in microseconds, over the mock Steam
# (no relay, no NAT, no real congestion). It finds logic faults under those
# conditions; it is not a measurement of internet play.
#
# Env:
#   NAME      the scenario's name (summary heading)
#   N         players, 2..4 (default 2)
#   NET       every copy's line, <ms>:<jitter ms>:<loss %> (default 0:0:0, none)
#   P<i>NET   copy i's line instead (1 is the host)
#   CUT       <copy>:<at s>:<len s>: that copy's line goes quiet (GE_COOP_NETCUT)
#   KILL      <copy>:<at s>: SIGKILL that copy (a crash or a pulled plug)
#   REJOIN    <s>: start the killed copy again (it joins from the friend list)
#   RUNSECS   how long each copy runs (default 150)
#   HOSTENV, P<i>ENV, ALLENV  more env for the host / copy i / every copy
#   BUILD     build_pc folder (default /home/dev/project/build_pc), OUT (/out)
# Exit 1 when a check fails (a crash, an unresolved desync, a joiner that
# never got the join snapshot, events dropped from a full queue, or, with
# FAILON_MIGRATE=1, no migration after a host kill).
BUILD=${BUILD:-/home/dev/project/build_pc}
OUT=${OUT:-/out}
N=${N:-2}
NAME=${NAME:-scenario}
RUNSECS=${RUNSECS:-150}
cd "$BUILD" || exit 2
mkdir -p "$OUT"
rm -rf /tmp/gecoop-mock
IDS=(76561190000000101 76561190000000202 76561190000000303 76561190000000404)
for i in $(seq 1 $N); do Xvfb :$((i + 10)) -screen 0 960x540x24 >/dev/null 2>&1 & done
sleep 1
# No GE_SKIPINTRO: its A taps (every 40 polls for the first ~15 s, input_pc.c)
# land on the lobby screen and press Leave Lobby or open character select, so
# the copies left and rejoined the lobby over and over before the mission.
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 \
    LD_LIBRARY_PATH=$PWD/mock GE_MENU=lobby GE_COOP_MOTIONLOG=1 ${ALLENV}
declare -A PID
start() {   # start <i> <host|join> <log> <secs>
    local i=$1 mode=$2 log=$3 secs=$4 me=${IDS[$(($1-1))]} friends envv v net cut=""
    friends=$(for j in $(seq 1 $N); do [ $j != $i ] && printf '%s,' ${IDS[$((j-1))]}; done)
    v=P${i}NET; net=${!v:-${NET:-0:0:0}}
    if [ -n "$CUT" ] && [ "${CUT%%:*}" = "$i" ]; then cut="GE_COOP_NETCUT=${CUT#*:}"; fi
    if [ $mode = host ]; then
        envv="GE_COOP_LOBBY_PLAYERS=$N GE_AUTOSTICK=0,70,1500,2400000 ${HOSTENV}"
    else
        v=P${i}ENV; envv="${JOINFIRE-GE_COOP_TESTFIRE=1:25:6} ${!v}"
    fi
    [ "$net" != "0:0:0" ] && envv="GE_COOP_NETSIM=$net $envv"
    DISPLAY=:$((i + 10)) env GE_MOCK_STEAMID=$me GE_MOCK_FRIEND=${friends%,} GE_COOP_LOBBY=$mode $cut $envv \
        timeout -s KILL $secs ./ProjectMidas > "$OUT/$log" 2>&1 &
    PID[$i]=$!
}
for i in $(seq 1 $N); do
    if [ $i = 1 ]; then start 1 host p1.log $RUNSECS; else start $i join p$i.log $RUNSECS; fi
    sleep 2
done
KC=${KILL%%:*}; KT=${KILL#*:}
events=$( ( [ -n "$KILL" ] && echo "$KT kill"; [ -n "$REJOIN" ] && echo "$REJOIN rejoin") | sort -n)
prev=0
while read -r t what; do
    [ -z "$t" ] && continue
    sleep $((t - prev)); prev=$t
    case $what in
        kill) echo "killing copy $KC at $t s"; pkill -9 -P ${PID[$KC]} ProjectMidas ;;
        rejoin) echo "starting copy $KC again at $t s"; start $KC join p${KC}b.log $((RUNSECS - t)) ;;
    esac
done <<< "$events"
while pgrep -x ProjectMidas >/dev/null; do sleep 1; done
pkill Xvfb

# ---- summary ----
fail=""
cnt() { grep -h "$1" "$OUT"/p*.log 2>/dev/null | wc -l; }
avg() { awk '{ s += $1; n++ } END { if (n) printf "%.1f", s / n; else printf "-" }'; }
{
    echo "== $NAME (N=$N, NET=${NET:-0:0:0}${CUT:+, CUT=$CUT}${KILL:+, KILL=$KILL}${REJOIN:+, REJOIN=$REJOIN})"
    for f in "$OUT"/p*.log; do
        b=$(basename $f .log)
        up=$(grep -h "net: to player" $f | sed -n 's/.* \([0-9.]*\) KB\/s.*/\1/p' | tail -n +2 | avg)
        rtt=$(grep -h "net: player [0-9]* round trip" $f | sed -n 's/.*round trip \([0-9]*\) ms.*/\1/p' | tail -n +2 | avg)
        behind=$(grep -h "drawn [0-9]* ms behind" $f | sed -n 's/.*drawn \([0-9]*\) ms behind.*/\1/p' | tail -n +2 | avg)
        cv=$(grep -h "coop: motion:" $f | sed -n 's/.* cv \([0-9.]*\) .*/\1/p' | avg)
        still=$(grep -h "coop: motion:" $f | sed -n 's/.* still \([0-9]*\) .*/\1/p' | avg)
        jumps=$(grep -h "coop: motion:" $f | sed -n 's/.* jumps \([0-9]*\) of.*/\1/p' | avg)
        snap=$(grep -c "repl: join snapshot applied" $f)
        printf '%-5s up %s KB/s  rtt %s ms  drawn %s ms behind  motion cv %s still %s jumps %s  snapshots %s\n' \
            $b "$up" "$rtt" "$behind" "$cv" "$still" "$jumps" "$snap"
    done
    echo "connected:        $(cnt 'connected to player')"
    echo "migrations:       $(cnt 'took over [0-9]* chrs')   new-host connects: $(cnt 'connected to the new host')"
    echo "lost/back/gone:   $(cnt 'lost connection')/$(cnt ' is back')/$(cnt 'disconnected\.')"
    echo "rejoins:          $(cnt 'rejoined (new session)') ($(for f in "$OUT"/p*.log; do awk '/first state from player/ { m = 1 } m && /rejoined \(new session\)/ { n++ } END { print n + 0 }' $f; done | awk '{ s += $1 } END { print s + 0 }') in a mission)"
    echo "events dropped:   $(cnt 'event queue to player .* full')"
    echo "refused events:   $(cnt 'refused event kind\|dropped event kind')   stale-session pkts: $(cnt "earlier session came late")"
    echo "desyncs fixed:    $(cnt 'repl: desync')   unresolved: $(cnt 'unresolved desync')"
    echo "stale guard ents: $(grep -h 'guard snapshot entries came after' "$OUT"/p*.log | awk '{s+=$4} END {print s+0}')"
    echo "slow-line toasts: $(cnt 'slow connection')"
} > "$OUT/summary.txt"
grep -qi "segmentation\|signal 11\|assert" "$OUT"/p*.log && fail="$fail crash"
[ "$(cnt 'unresolved desync')" -gt 0 ] && fail="$fail unresolved-desync"
[ "$(cnt 'event queue to player .* full')" -gt 0 ] && fail="$fail events-dropped"
for i in $(seq 2 $N); do
    [ -f "$OUT/p$i.log" ] && ! grep -q "repl: join snapshot applied" "$OUT/p$i.log" && fail="$fail p$i-never-joined"
done
if [ "$FAILON_MIGRATE" = 1 ] && [ "$(cnt 'took over [0-9]* chrs')" -eq 0 ]; then fail="$fail no-migration"; fi
echo "result: ${fail:-PASS}" >> "$OUT/summary.txt"
cat "$OUT/summary.txt"
[ -z "$fail" ]
