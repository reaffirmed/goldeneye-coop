#!/bin/bash
# solo.sh <outdir>: a host on its own (no joiner). env: STAGE HOSTENV SECS SLOT DIFF
OUT=$(mkdir -p "$1" && cd "$1" && pwd)
BUILD=${BUILD:-$PWD/build_pc}; SLOT=${SLOT:-0}
cd $BUILD
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 LD_LIBRARY_PATH=$PWD/mock
hd=$((40+SLOT)); Xvfb :$hd -screen 0 640x480x24 >/dev/null 2>&1 & xh=$!; sleep 1
A=$((76561190000003001+10*SLOT)); M=/tmp/gemocks$SLOT; rm -rf $M
DISPLAY=:$hd env GE_MOCK_DIR=$M GE_MOCK_STEAMID=$A GE_MOCK_FRIEND=$((A+1)) GE_STAGE=${STAGE:-dam} GE_DIFF=${DIFF:-0} GE_COOP_SOLO=1 $HOSTENV timeout -s KILL ${SECS:-120} ./ProjectMidas -coophost > $OUT/host.log 2>&1 & hp=$!
( while kill -0 $hp 2>/dev/null; do grep -q "steps: finished" $OUT/host.log && { sleep 2; kill $hp; break; }; sleep 1; done ) &
wait $hp; kill $xh 2>/dev/null; exit 0
