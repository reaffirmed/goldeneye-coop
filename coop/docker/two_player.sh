#!/bin/bash
cd /home/dev/project/build_pc
Xvfb :1 -screen 0 1280x720x24 >/dev/null 2>&1 &
Xvfb :2 -screen 0 1280x720x24 >/dev/null 2>&1 &
sleep 1
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_STAGE=dam GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 ${BOTHENV}
DISPLAY=:1 GE_COOP=host:27015 timeout ${RUNSECS:-150} ./ProjectMidas > /out/host.log 2>&1 &
sleep 2
DISPLAY=:2 env ${JOINENV} GE_COOP=join:127.0.0.1:27015 timeout ${RUNSECS:-150} ./ProjectMidas > /out/join.log 2>&1 &
for t in ${SHOTS:-30 45 55 65 80}; do sleep $((t - ${prev:-0})); prev=$t; DISPLAY=:1 import -window root /out/host_$t.png; DISPLAY=:2 import -window root /out/join_$t.png; done
while pgrep -x ProjectMidas >/dev/null; do sleep 1; done; pkill Xvfb; exit 0
