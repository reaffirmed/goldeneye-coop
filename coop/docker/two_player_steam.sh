#!/bin/bash
# Two copies over the mock Steam (coop/mock): the host starts Dam, the joiner
# boots to the menus, finds the host through the "friend list" and follows it.
cd /home/dev/project/build_pc
rm -rf /tmp/gecoop-mock
Xvfb :1 -screen 0 1280x720x24 >/dev/null 2>&1 &
Xvfb :2 -screen 0 1280x720x24 >/dev/null 2>&1 &
sleep 1
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 LD_LIBRARY_PATH=$PWD/mock ${BOTHENV}
A=76561190000000101; B=76561190000000202
DISPLAY=:1 env GE_STAGE=dam GE_MOCK_STEAMID=$A GE_MOCK_FRIEND=$B ${HOSTENV} timeout ${RUNSECS:-90} ./ProjectMidas -coophost > /out/host.log 2>&1 &
sleep 2
DISPLAY=:2 env GE_MOCK_STEAMID=$B GE_MOCK_FRIEND=$A ${JOINENV} timeout ${RUNSECS:-90} ./ProjectMidas -coopjoin > /out/join.log 2>&1 &
for t in ${SHOTS:-40 60}; do sleep $((t - ${prev:-0})); prev=$t; DISPLAY=:1 import -window root /out/host_$t.png; DISPLAY=:2 import -window root /out/join_$t.png; done
while pgrep -x ProjectMidas >/dev/null; do sleep 1; done; pkill Xvfb; exit 0
