#!/bin/bash
# The co-op lobby on one copy, not hosting (the PLAY / GAME SETUP / FIND
# GAMES / CONTROLS tabs), over the mock Steam with one friend online.
# GE_COOP_LOBBYTAB=c steps through the tabs, 4 s apiece; SHOTS are seconds.
cd /home/dev/project/build_pc
rm -rf /tmp/gecoop-mock
Xvfb :1 -screen 0 1280x720x24 >/dev/null 2>&1 &
sleep 1
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 LD_LIBRARY_PATH=$PWD/mock GE_MENU=lobby ${BOTHENV}
DISPLAY=:1 env GE_MOCK_STEAMID=76561190000000101 GE_MOCK_FRIEND=76561190000000202 ${HOSTENV} timeout ${RUNSECS:-30} ./ProjectMidas > /out/idle.log 2>&1 &
for t in ${SHOTS:-8 12 16 20}; do sleep $((t - ${prev:-0})); prev=$t; DISPLAY=:1 import -window root /out/idle_$t.png; done
while pgrep -x ProjectMidas >/dev/null; do sleep 1; done; pkill Xvfb; exit 0
