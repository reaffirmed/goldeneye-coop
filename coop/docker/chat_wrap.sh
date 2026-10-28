#!/bin/bash
# Long text on the co-op screen: two copies over the mock Steam with long
# names (GE_MOCK_NAMESUFFIX), each sending a long chat line, then the host's
# CHAT, GAME SETUP (a row with a long description) and LOBBY tabs. Shots of
# the host's screen (in_*) and the joiner's (join_*).
cd /home/dev/project/build_pc
rm -rf /tmp/gecoop-mock
Xvfb :1 -screen 0 1280x720x24 >/dev/null 2>&1 &
Xvfb :2 -screen 0 1280x720x24 >/dev/null 2>&1 &
sleep 1
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 LD_LIBRARY_PATH=$PWD/mock GE_MENU=lobby
export GE_MOCK_NAMESUFFIX="${SUFFIX- Ti Super Long}"
A=76561190000000101; B=76561190000000202
DISPLAY=:1 env GE_MOCK_STEAMID=$A GE_MOCK_FRIEND=$B GE_COOP_LOBBY=host GE_COOP_LOBBY_DELAY=100000 timeout 110 ./ProjectMidas > /out/host.log 2>&1 &
sleep 2
DISPLAY=:2 env GE_MOCK_STEAMID=$B GE_MOCK_FRIEND=$A GE_COOP_LOBBY=join timeout 108 ./ProjectMidas > /out/join.log 2>&1 &
sleep 16
n=0
shot() { n=$((n+1)); sleep 0.8; import -window root /out/in_$(printf %02d $n)_$1.png; }
key() { for k in "$@"; do xdotool keydown $k; sleep 0.08; xdotool keyup $k; sleep 0.12; done; }
focus() { W=$(xdotool search --name . | tail -1); xdotool windowfocus $W 2>/dev/null; xdotool windowactivate $W 2>/dev/null; }
say() { key t; sleep 0.3; xdotool type --delay 40 "$1"; sleep 0.2; key Return; sleep 0.5; }
export DISPLAY=:2; focus
say "chequemos primero lo de las skins antes de empezar la mision"
export DISPLAY=:1; focus
say "prueba de mensaje largo con muchas palabras para ver si se corta"
say "corto"
export DISPLAY=:2; focus
say "a ver dale sin que pongas nada mas y escuchas perfecto el audio"
sleep 1; import -window root /out/join_chat.png
export DISPLAY=:1; focus
shot chat
key t; sleep 0.3; xdotool type --delay 30 "y ahora escribo una linea larga que todavia no mande"; shot chat_typing
key Return; sleep 0.5
key comma comma comma comma; shot lobby
key period; shot setup
key Down; shot setup_row1
key Down; shot setup_row2
key Down; shot setup_row3
for i in 1 2 3 4 5 6 7 8 9 10 11; do key Down; done; shot setup_last
sleep 3; shot setup_last_scrolled
key period; shot invite
key Down Down; shot invite_row
key period; shot controls
key Down Down Down; shot controls_row3
key Down Down Down; shot controls_row6
while pgrep -x ProjectMidas >/dev/null; do sleep 1; done; pkill Xvfb; exit 0
