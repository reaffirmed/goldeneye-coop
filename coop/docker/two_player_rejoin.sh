#!/bin/bash
# Lobby flow over the mock Steam, then the joiner's game is killed mid-mission
# and started again: it should find the host, follow it into the mission and
# pick the session back up.
cd /home/dev/project/build_pc
rm -rf /tmp/gecoop-mock
Xvfb :1 -screen 0 1280x720x24 >/dev/null 2>&1 &
Xvfb :2 -screen 0 1280x720x24 >/dev/null 2>&1 &
sleep 1
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 LD_LIBRARY_PATH=$PWD/mock GE_MENU=lobby GE_COOP_LOBBY_DELAY=30 ${BOTHENV}
A=76561190000000101; B=76561190000000202
DISPLAY=:1 env GE_MOCK_STEAMID=$A GE_MOCK_FRIEND=$B GE_COOP_LOBBY=host ${HOSTENV} timeout ${RUNSECS:-110} ./ProjectMidas > /out/host.log 2>&1 &
sleep 2
DISPLAY=:2 env GE_MOCK_STEAMID=$B GE_MOCK_FRIEND=$A GE_COOP_LOBBY=join ${JOINENV} timeout 40 ./ProjectMidas > /out/join.log 2>&1
sleep 3
DISPLAY=:2 env GE_MOCK_STEAMID=$B GE_MOCK_FRIEND=$A GE_COOP_LOBBY=join ${JOINENV} timeout 55 ./ProjectMidas > /out/join2.log 2>&1 &
sleep 45; DISPLAY=:1 import -window root /out/host_end.png; DISPLAY=:2 import -window root /out/join_end.png
while pgrep -x ProjectMidas >/dev/null; do sleep 1; done; pkill Xvfb; exit 0
