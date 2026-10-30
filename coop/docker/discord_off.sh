#!/bin/bash
# Discord coming and going, one copy at the menus with the built-in
# application id (no GE_DISCORD_APPID):
#   0 s   Discord isn't running: logged once, the game plays on
#   20 s  Discord starts but turns the game away (the mock's "refuse": a bad
#         application id): logged once, the next tries wait 30 s, 60 s ...
#   50 s  Discord takes the id: connected, the activity goes through
#   80 s  Discord quits: disconnected, and it is looked for again
# Then a second copy with GE_NO_DISCORD=1 never opens the socket.
# Game logs to /out/off*.log, what the game sent Discord to /out/off.discord.log.
cd /home/dev/project/build_pc
d=/tmp/gediscord/off
rm -rf /tmp/gediscord; mkdir -p $d
Xvfb :1 -screen 0 960x540x24 >/dev/null 2>&1 &
sleep 1
export DISPLAY=:1 SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 \
       LD_LIBRARY_PATH=$PWD/mock XDG_RUNTIME_DIR=$d
timeout ${RUNSECS:-110} ./ProjectMidas > /out/off.log 2>&1 & game=$!
sleep 20
touch $d/refuse
python3 ../coop/mock/discord_mock.py $d & mock=$!
sleep 30
rm -f $d/refuse
sleep 30
import -window root /out/off_connected.png
kill $mock; rm -f $d/discord-ipc-0
wait $game
cp $d/discord.log /out/off.discord.log
rm -rf $d; mkdir -p $d
python3 ../coop/mock/discord_mock.py $d & mock=$!
GE_NO_DISCORD=1 timeout 30 ./ProjectMidas > /out/off_nodiscord.log 2>&1
kill $mock; cp $d/discord.log /out/off_nodiscord.discord.log
pkill Xvfb; exit 0
