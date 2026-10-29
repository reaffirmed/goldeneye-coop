#!/bin/bash
# batch.sh <outroot> <name>|<stage>|<hostenv>|<joinenv>|<secs>[|<joinafter>] ... ; PAR (default 4)
ROOT=$1; shift; PAR=${PAR:-4}; i=0
for spec in "$@"; do
  IFS='|' read -r name st he je secs ja <<< "$spec"
  slot=$((i % PAR))
  ( SLOT=$slot STAGE=$st HOSTENV="$he" JOINENV="$je" SECS=${secs:-90} JOINAFTER=${ja:-2} SHOTS="${SHOTS_FOR:-}" $(dirname "$0")/pair.sh $ROOT/$name >/dev/null 2>&1 ) &
  i=$((i+1)); [ $((i % PAR)) -eq 0 ] && wait
done; wait
