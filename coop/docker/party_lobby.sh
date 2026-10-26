#!/bin/bash
# N copies (PLAYERS=3 or 4) over the mock Steam, through the in-game lobby:
# the first hosts and waits for everyone, the others find it in their friend
# lists, join and ready; the host starts GE_STAGE (Dam) and they follow.
# Logs and screenshots go to /out as p1.log, p2.log, ... and p<N>_<t>.png.
cd /home/dev/project/build_pc
rm -rf /tmp/gecoop-mock
N=${PLAYERS:-3}
IDS=(76561190000000101 76561190000000202 76561190000000303 76561190000000404)
for i in $(seq 1 $N); do Xvfb :$i -screen 0 960x540x24 >/dev/null 2>&1 & done
sleep 1
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 LD_LIBRARY_PATH=$PWD/mock GE_MENU=lobby ${ALLENV}
for i in $(seq 1 $N); do
    me=${IDS[$((i-1))]}
    friends=$(for j in $(seq 1 $N); do [ $j != $i ] && printf '%s,' ${IDS[$((j-1))]}; done)
    if [ $i = 1 ]; then mode=host; envv="GE_COOP_LOBBY_PLAYERS=$N ${HOSTENV}"; else mode=join; v=P${i}ENV; envv="${!v}"; fi
    DISPLAY=:$i env GE_MOCK_STEAMID=$me GE_MOCK_FRIEND=${friends%,} GE_COOP_LOBBY=$mode $envv timeout ${RUNSECS:-120} ./ProjectMidas > /out/p$i.log 2>&1 &
    sleep 2
done
prev=0
# BURST=<t>:<n> instead takes n back-to-back shots of p1 at t (p1_<t>_<k>.png),
# to catch a muzzle flash or tracer
for t in ${SHOTS:-30 70}; do sleep $((t - prev)); prev=$t; for i in $(seq 1 $N); do DISPLAY=:$i import -window root /out/p${i}_$t.png; done
    if [ "${BURST%%:*}" = "$t" ]; then for k in $(seq 1 ${BURST##*:}); do DISPLAY=:1 import -window root /out/p1_${t}_$k.png; done; fi; done
while pgrep -x ProjectMidas >/dev/null; do sleep 1; done; pkill Xvfb; exit 0
