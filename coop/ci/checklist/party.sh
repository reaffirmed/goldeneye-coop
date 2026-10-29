#!/bin/bash
# party.sh <outdir>: host + (PLAYERS-1) joiners on mock steam, -coophost/-coopjoin. env: STAGE DIFF P1ENV..P4ENV ALLENV SECS SLOT
OUT=$(mkdir -p "$1" && cd "$1" && pwd)
BUILD=${BUILD:-$PWD/build_pc}; SLOT=${SLOT:-0}; N=${PLAYERS:-3}
cd $BUILD
M=/tmp/gemockp$SLOT; rm -rf $M
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 LD_LIBRARY_PATH=$PWD/mock GE_MOCK_DIR=$M
base=$((76561190000005000 + 100*SLOT)); xs=(); ps=()
for i in $(seq 1 $N); do d=$((40+10*SLOT+i)); Xvfb :$d -screen 0 640x480x24 >/dev/null 2>&1 & xs+=($!); done
sleep 1
for i in $(seq 1 $N); do
  d=$((40+10*SLOT+i)); me=$((base+i)); v=P${i}ENV
  if [ $i = 1 ]; then friends=$(for j in $(seq 2 $N); do printf '%s,' $((base+j)); done); args="-coophost"; extra="GE_STAGE=${STAGE:-dam} GE_DIFF=${DIFF:-0}"
  else friends="$((base+1)),"; args="-coopjoin"; extra=""; fi
  DISPLAY=:$d env GE_MOCK_STEAMID=$me GE_MOCK_FRIEND=${friends%,} $extra $ALLENV ${!v} timeout -s KILL ${SECS:-200} ./ProjectMidas $args > $OUT/p$i.log 2>&1 & ps+=($!)
  sleep 2
done
( while kill -0 ${ps[0]} 2>/dev/null; do grep -q "steps: finished" $OUT/p*.log 2>/dev/null && { sleep 2; kill ${ps[@]} 2>/dev/null; break; }; sleep 1; done ) &
wait ${ps[@]}; kill ${xs[@]} 2>/dev/null; exit 0
