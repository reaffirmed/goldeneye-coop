#!/bin/bash
# A far-away player drops in mid-mission: the host launches ${STAGE:-dam} with a
# nearby partner (NEARSIM, default 30:10:0), and at ${LATE:-60} s a third copy on
# a long, lossy line (FARSIM, default 300:80:3, GE_COOP_NETSIM) joins the mission
# under way. HUNT=<s> (default 25) has the far joiner go after guards <s> s after
# it arrives, firing a KF7 (item 8) (GE_COOP_TESTHUNT, GE_COOP_TESTFIRE). Logs in /out:
# host.log, near.log, far.log; shots p1/p2/p3_<t>.png at ${SHOTS} s.
cd ${BUILD:-/home/dev/project/build_pc}
mock=/tmp/gecoop-far; rm -rf $mock
for d in 1 2 3; do Xvfb :$d -screen 0 640x480x24 >/dev/null 2>&1 & done
sleep 1
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 LD_LIBRARY_PATH=$PWD/mock GE_MOCK_DIR=$mock ${BOTHENV}
A=76561190000000101; B=76561190000000202; C=76561190000000303
RUN=${RUNSECS:-240}; LATE=${LATE:-60}; HUNT=${HUNT:-25}
DISPLAY=:1 env GE_MOCK_STEAMID=$A GE_MOCK_FRIEND=$B,$C GE_STAGE=${STAGE:-dam} ${HOSTENV} timeout -s KILL $RUN ./ProjectMidas -coophost > /out/host.log 2>&1 &
sleep 2
DISPLAY=:2 env GE_MOCK_STEAMID=$B GE_MOCK_FRIEND=$A GE_COOP_NETSIM=${NEARSIM:-30:10:0} ${NEARENV} timeout -s KILL $((RUN - 2)) ./ProjectMidas -coopjoin > /out/near.log 2>&1 &
sleep $((LATE - 2))
DISPLAY=:3 env GE_MOCK_STEAMID=$C GE_MOCK_FRIEND=$A GE_COOP_NETSIM=${FARSIM:-300:80:3} GE_COOP_TESTHUNT=$HUNT GE_COOP_TESTFIRE=${FIRE:-8}:$HUNT:600 ${FARENV} timeout -s KILL $((RUN - LATE)) ./ProjectMidas -coopjoin > /out/far.log 2>&1 &
for t in ${SHOTS:-90 120 150}; do sleep $((t - ${prev:-$LATE})); prev=$t; for d in 1 2 3; do DISPLAY=:$d import -window root /out/p${d}_$t.png; done; done
while pgrep -x ProjectMidas >/dev/null; do sleep 1; done; pkill Xvfb; exit 0
