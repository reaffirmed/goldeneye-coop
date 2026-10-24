#!/bin/bash
# two_player_lobby.sh with the joiner run from its own copy of the game
# (build_p2, refreshed from build_pc), so each player has its own files:
# coop_campaign.txt, saves, midas.ini. HOSTFILE / JOINFILE=<src>:<name> put
# a file in that player's folder first (e.g. a campaign record).
cd /home/dev/project
mkdir -p build_p2/Data build_p2/mock
cp build_pc/ProjectMidas build_pc/*.txt build_p2/ && cp build_pc/Data/*.z64 build_pc/Data/*.txt build_p2/Data/ 2>/dev/null
cp build_pc/mock/libsteam_api.so build_p2/mock/
rm -f build_pc/coop_campaign.txt build_p2/coop_campaign.txt
[ -n "$HOSTFILE" ] && cp "${HOSTFILE%%:*}" "build_pc/${HOSTFILE##*:}"
[ -n "$JOINFILE" ] && cp "${JOINFILE%%:*}" "build_p2/${JOINFILE##*:}"
rm -rf /tmp/gecoop-mock
Xvfb :1 -screen 0 1280x720x24 >/dev/null 2>&1 &
Xvfb :2 -screen 0 1280x720x24 >/dev/null 2>&1 &
sleep 1
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 GE_MENU=lobby ${BOTHENV}
A=76561190000000101; B=76561190000000202
(cd build_pc && DISPLAY=:1 env LD_LIBRARY_PATH=$PWD/mock GE_MOCK_STEAMID=$A GE_MOCK_FRIEND=$B GE_COOP_LOBBY=${HOSTMODE:-host} ${HOSTENV} timeout ${RUNSECS:-90} ./ProjectMidas > /out/host.log 2>&1 &)
sleep 2
(cd build_p2 && DISPLAY=:2 env LD_LIBRARY_PATH=$PWD/mock GE_MOCK_STEAMID=$B GE_MOCK_FRIEND=$A GE_COOP_LOBBY=${JOINMODE:-join} ${JOINENV} timeout ${RUNSECS:-90} ./ProjectMidas > /out/join.log 2>&1 &)
for t in ${SHOTS:-15 25 60}; do sleep $((t - ${prev:-0})); prev=$t; DISPLAY=:1 import -window root /out/host_$t.png; DISPLAY=:2 import -window root /out/join_$t.png; done
while pgrep -x ProjectMidas >/dev/null; do sleep 1; done; pkill Xvfb
cp build_pc/coop_campaign.txt /out/host_campaign.txt 2>/dev/null; cp build_p2/coop_campaign.txt /out/join_campaign.txt 2>/dev/null
exit 0
