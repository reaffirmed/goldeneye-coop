#!/bin/bash
cd /home/dev/project/build_pc
Xvfb :9 -screen 0 1280x720x24 >/dev/null 2>&1 &
export DISPLAY=:9 SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1
sleep 1
GE_STAGE=dam GE_SKIPINTRO=1 GE_NOMSGBOX=1 timeout 120 ./ProjectMidas > /out/run.log 2>&1 &
for t in 20 40 60; do sleep 20; import -window root /out/shot_$t.png; done
wait
