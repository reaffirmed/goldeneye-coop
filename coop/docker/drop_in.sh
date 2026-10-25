#!/bin/bash
# Drop-in: the host starts Dam through the lobby with the joiner, the joiner's
# game is killed at 40 s and started again at 45 s; it follows the host into
# the mission under way, watches a partner until it is quiet and arrives
# beside them. Screenshots of the rejoined copy at ${SHOTS} seconds after it
# starts again (join2_<t>.png, host_<t>.png).
cd /home/dev/project/build_pc
rm -rf /tmp/gecoop-mock
Xvfb :1 -screen 0 1280x720x24 >/dev/null 2>&1 &
Xvfb :2 -screen 0 1280x720x24 >/dev/null 2>&1 &
sleep 1
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 LD_LIBRARY_PATH=$PWD/mock GE_MENU=lobby GE_COOP_LOBBY_DELAY=20 ${BOTHENV}
A=76561190000000101; B=76561190000000202
DISPLAY=:1 env GE_MOCK_STEAMID=$A GE_MOCK_FRIEND=$B GE_COOP_LOBBY=host ${HOSTENV} timeout ${RUNSECS:-160} ./ProjectMidas > /out/host.log 2>&1 &
sleep 2
DISPLAY=:2 env GE_MOCK_STEAMID=$B GE_MOCK_FRIEND=$A GE_COOP_LOBBY=join ${JOINENV} timeout ${KILLAT:-60} ./ProjectMidas > /out/join.log 2>&1
sleep 3
DISPLAY=:2 env GE_MOCK_STEAMID=$B GE_MOCK_FRIEND=$A GE_COOP_LOBBY=join ${JOINENV} timeout $((${RUNSECS:-160} - ${KILLAT:-60} - 5)) ./ProjectMidas > /out/join2.log 2>&1 &
for t in ${SHOTS:-40 60 80}; do sleep $((t - ${prev:-0})); prev=$t; DISPLAY=:1 import -window root /out/host_$t.png; DISPLAY=:2 import -window root /out/join2_$t.png; done
while pgrep -x ProjectMidas >/dev/null; do sleep 1; done; pkill Xvfb; exit 0
