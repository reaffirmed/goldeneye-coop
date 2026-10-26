#!/bin/bash
# Two copies over the mock Steam that are not Steam friends: the host hosts
# a public lobby (GE_COOP_PUBLIC=1) with HOSTPW as its password (none if
# empty); the joiner finds it under Public Games (GE_COOP_LOBBY=browse) and
# joins with JOINPW, then readies; the host starts Dam once it is in.
cd /home/dev/project/build_pc
rm -rf /tmp/gecoop-mock
Xvfb :1 -screen 0 1280x720x24 >/dev/null 2>&1 &
Xvfb :2 -screen 0 1280x720x24 >/dev/null 2>&1 &
sleep 1
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 LD_LIBRARY_PATH=$PWD/mock GE_MENU=lobby ${BOTHENV}
A=76561190000000101; B=76561190000000202
DISPLAY=:1 env GE_MOCK_STEAMID=$A GE_COOP_LOBBY=host GE_COOP_PUBLIC=1 ${HOSTPW:+GE_COOP_PASSWORD=$HOSTPW} ${HOSTENV} timeout ${RUNSECS:-90} ./ProjectMidas > /out/host.log 2>&1 &
sleep 2
DISPLAY=:2 env GE_MOCK_STEAMID=$B GE_COOP_LOBBY=browse ${JOINPW:+GE_COOP_PASSWORD=$JOINPW} ${JOINENV} timeout ${RUNSECS:-90} ./ProjectMidas > /out/join.log 2>&1 &
for t in ${SHOTS:-15 25 60}; do sleep $((t - ${prev:-0})); prev=$t; DISPLAY=:1 import -window root /out/host_$t.png; DISPLAY=:2 import -window root /out/join_$t.png; done
while pgrep -x ProjectMidas >/dev/null; do sleep 1; done; pkill Xvfb; exit 0
