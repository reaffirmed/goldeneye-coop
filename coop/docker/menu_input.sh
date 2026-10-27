#!/bin/bash
# The co-op lobby driven by the keyboard (xdotool), two copies over the mock
# Steam: the host tabs with , and . (watch_prev / watch_next), walks the rows
# with the arrow keys, steps a value with Left / Right and opens the
# character select. Shots of the host's screen after each step.
# CLICK=x,y instead parks the crosshair there (GE_CURSOR, menu units) and
# clicks once: a tab, or a value's arrow. The game's mouse is relative, so
# xdotool can't aim it.
cd /home/dev/project/build_pc
rm -rf /tmp/gecoop-mock
Xvfb :1 -screen 0 1280x720x24 >/dev/null 2>&1 &
Xvfb :2 -screen 0 1280x720x24 >/dev/null 2>&1 &
sleep 1
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 LD_LIBRARY_PATH=$PWD/mock GE_MENU=lobby
A=76561190000000101; B=76561190000000202
DISPLAY=:1 env GE_MOCK_STEAMID=$A GE_MOCK_FRIEND=$B GE_COOP_LOBBY=host GE_COOP_LOBBY_DELAY=100000 ${CLICK:+GE_CURSOR=$CLICK} timeout 75 ./ProjectMidas > /out/host.log 2>&1 &
sleep 2
DISPLAY=:2 env GE_MOCK_STEAMID=$B GE_MOCK_FRIEND=$A GE_COOP_LOBBY=join timeout 73 ./ProjectMidas > /out/join.log 2>&1 &
export DISPLAY=:1
sleep 16
W=$(xdotool search --name . | tail -1); xdotool windowfocus $W 2>/dev/null; xdotool windowactivate $W 2>/dev/null
n=0
shot() { n=$((n+1)); sleep 0.6; import -window root /out/in_$(printf %02d $n)_$1.png; }
key() { for k in "$@"; do xdotool keydown $k; sleep 0.08; xdotool keyup $k; sleep 0.12; done; }
click() { xdotool mousedown 1; sleep 0.08; xdotool mouseup 1; }
if [ -n "$CLICK" ]; then
    shot before_click; click; shot after_click
else
    shot open
    key Down; shot down
    key period; shot tab_setup
    key Down Down; shot setup_row
    key Right Right; shot stepped_right
    key Left; shot stepped_left
    key period period; shot tab_controls
    key comma comma comma; shot tab_lobby
    key Up Up Up Up Down; shot character_row
    key Return; shot charsel
    key Right Right Right; shot charsel_right
    key Return; shot charsel_taken
    key Escape; shot back_to_menus
fi
while pgrep -x ProjectMidas >/dev/null; do sleep 1; done; pkill Xvfb; exit 0
