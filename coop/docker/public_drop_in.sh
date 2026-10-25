#!/bin/bash
# Public drop-in: the host hosts a public game (GE_COOP_PUBLIC=1) and starts
# Dam with a joiner who found it under Public Games; at ${LATE:-60} s a third
# copy that is nobody's friend opens the lobby screen (GE_COOP_LOBBY=${THIRD:-wait}:
# it only looks; browse: it joins the first public game, so it drops in).
cd /home/dev/project/build_pc
rm -rf /tmp/gecoop-mock
for d in 1 2 3; do Xvfb :$d -screen 0 1280x720x24 >/dev/null 2>&1 & done
sleep 1
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 LD_LIBRARY_PATH=$PWD/mock GE_MENU=lobby ${BOTHENV}
A=76561190000000101; B=76561190000000202; C=76561190000000303
DISPLAY=:1 env GE_MOCK_STEAMID=$A GE_COOP_LOBBY=host GE_COOP_PUBLIC=1 GE_COOP_LOBBY_PLAYERS=2 ${HOSTENV} timeout ${RUNSECS:-150} ./ProjectMidas > /out/host.log 2>&1 &
sleep 2
DISPLAY=:2 env GE_MOCK_STEAMID=$B GE_COOP_LOBBY=browse ${JOINENV} timeout ${RUNSECS:-150} ./ProjectMidas > /out/join.log 2>&1 &
sleep $((${LATE:-60} - 2))
DISPLAY=:3 env GE_MOCK_STEAMID=$C GE_COOP_LOBBY=${THIRD:-wait} ${THIRDENV} timeout $((${RUNSECS:-150} - ${LATE:-60})) ./ProjectMidas > /out/third.log 2>&1 &
for t in ${SHOTS:-30 50 70}; do sleep $((t - ${prev:-0})); prev=$t; for d in 1 2 3; do DISPLAY=:$d import -window root /out/p${d}_$t.png; done; done
while pgrep -x ProjectMidas >/dev/null; do sleep 1; done; pkill Xvfb; exit 0
