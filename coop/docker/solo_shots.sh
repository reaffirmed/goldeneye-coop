#!/bin/bash
# One copy in Dam with screenshots at ${SHOTS} seconds; ${ENVS} go to the game.
cd /home/dev/project/build_pc
Xvfb :9 -screen 0 1280x720x24 >/dev/null 2>&1 &
export DISPLAY=:9 SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1
sleep 1
env GE_STAGE=${STAGE:-dam} GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 ${ENVS} timeout ${RUNSECS:-40} ./ProjectMidas > /out/run.log 2>&1 &
for t in ${SHOTS:-20}; do sleep $((t - ${prev:-0})); prev=$t; import -window root /out/shot_$t.png; done
while pgrep -x ProjectMidas >/dev/null; do sleep 1; done; pkill Xvfb; exit 0
