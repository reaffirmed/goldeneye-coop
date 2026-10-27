#!/bin/bash
# Online versus over the mock Steam: PLAYERS copies (2 by default) meet in the
# lobby, the host sets the versus rows from VERSUS=<arena>:<scenario>:<length>:<weapons>
# (default 1:0:4:1: Temple, Normal, First to 5, weapon set 1; see coop/NOTES.md) and
# starts the match. The host's bot steps up to the next player and fires an AK
# (GE_VS_HUNT, GE_COOP_TESTFIRE); everyone presses Start to respawn and to
# leave the end screen (GE_VS_RESPAWN); KILLS shortens the kill limit. The menu
# crosshair is parked off the lobby's rows (GE_CURSOR), where a stray press
# would land on Leave Lobby. Logs
# and screenshots go to /out as party_lobby.sh writes them.
#   docker run ... gerun bash coop/docker/versus.sh            (with PLAYERS=4 KILLS=2 ...)
export PLAYERS=${PLAYERS:-2}
export HOSTENV="GE_COOP_VERSUS=${VERSUS:-1:0:4:1} GE_VS_HUNT=8 ${HOSTENV}"
export ALLENV="GE_CURSOR=20,225 GE_COOP_TESTFIRE=8:12:400 GE_VS_RESPAWN=1 ${KILLS:+GE_VS_KILLS=$KILLS} ${ALLENV}"
export RUNSECS=${RUNSECS:-150}
export SHOTS=${SHOTS:-"20 40 60"}
exec bash "$(dirname "$0")/party_lobby.sh"
