#!/bin/bash
# pair.sh <outdir>: host+joiner on mock steam, native. env: BUILD STAGE HOSTENV JOINENV SECS SHOTS SLOT JOINAFTER DIFF
OUT=$(mkdir -p "$1" && cd "$1" && pwd)
BUILD=${BUILD:-$PWD/build_pc}; SLOT=${SLOT:-0}
cd $BUILD
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 LD_LIBRARY_PATH=$PWD/mock
hd=$((20+2*SLOT)); jd=$((21+2*SLOT))
Xvfb :$hd -screen 0 640x480x24 >/dev/null 2>&1 & xh=$!
Xvfb :$jd -screen 0 640x480x24 >/dev/null 2>&1 & xj=$!
sleep 1
A=$((76561190000002001+10*SLOT)); B=$((A+1)); M=/tmp/gemock$SLOT; rm -rf $M
DISPLAY=:$hd env GE_MOCK_DIR=$M GE_MOCK_STEAMID=$A GE_MOCK_FRIEND=$B GE_STAGE=${STAGE:-dam} GE_DIFF=${DIFF:-0} $HOSTENV timeout -s KILL ${SECS:-120} ./ProjectMidas -coophost > $OUT/host.log 2>&1 & hp=$!
sleep ${JOINAFTER:-2}
DISPLAY=:$jd env GE_MOCK_DIR=$M GE_MOCK_STEAMID=$B GE_MOCK_FRIEND=$A $JOINENV timeout -s KILL ${SECS:-120} ./ProjectMidas -coopjoin > $OUT/join.log 2>&1 & jp=$!
( while kill -0 $hp 2>/dev/null && kill -0 $jp 2>/dev/null; do grep -q "steps: finished" $OUT/host.log $OUT/join.log 2>/dev/null && { sleep 2; kill $hp $jp 2>/dev/null; break; }; sleep 1; done ) &
prev=0
for t in $SHOTS; do sleep $((t - prev)); prev=$t; DISPLAY=:$hd import -window root $OUT/host_$t.png 2>/dev/null; DISPLAY=:$jd import -window root $OUT/join_$t.png 2>/dev/null; done
wait $hp $jp; kill $xh $xj 2>/dev/null; exit 0
