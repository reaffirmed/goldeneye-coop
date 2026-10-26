#!/bin/bash
# Rich presence and joins from outside the lobby: three copies over the mock
# Steam, each with its own mock Discord (coop/mock/discord_mock.py).
#   p1  hosts a public lobby with a password (GE_COOP_PUBLIC, GE_COOP_PASSWORD)
#       and invites its Steam friend p3 (GE_COOP_LOBBY=invite)
#   p2  isn't p1's Steam friend; it sits in the menus. Its player "asks to
#       join" on Discord; p1's lobby lists the request and the harness lets it
#       in after ASKWAIT seconds (GE_DISCORD_AUTOACCEPT); "Discord" then hands
#       p2 the host's join secret and p2's game joins the lobby, past the
#       password, without being told it
#   p3  p1's Steam friend, also in the menus: the invite pulls it into the lobby
# Once all three are in and ready the host starts Dam. Logs, Discord logs
# (p<i>.discord.log: what each game sent Discord) and screenshots go to /out.
# FRIENDSONLY=1 keeps the lobby friends-only: Steam then keeps p2 out and its
# lobby screen says so. P3ENV=GE_STAGE=dam has p3 in a solo mission when the
# invite comes (a toast; it joins once the mission ends; P1DELAY=20 gives it
# time to get there).
cd /home/dev/project/build_pc
rm -rf /tmp/gecoop-mock /tmp/gediscord
IDS=(76561190000000101 76561190000000202 76561190000000303)
for i in 1 2 3; do Xvfb :$i -screen 0 960x540x24 >/dev/null 2>&1 & done
for i in 1 2 3; do mkdir -p /tmp/gediscord/p$i; python3 ../coop/mock/discord_mock.py /tmp/gediscord/p$i & done
sleep 1
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 LD_LIBRARY_PATH=$PWD/mock \
       GE_DISCORD_APPID=${APPID:-1300000000000000000} ${ALLENV}
DISPLAY=:2 env XDG_RUNTIME_DIR=/tmp/gediscord/p2 GE_MOCK_STEAMID=${IDS[1]} GE_COOP_LOBBY=wait ${P2ENV} timeout ${RUNSECS:-150} ./ProjectMidas > /out/p2.log 2>&1 &
DISPLAY=:3 env XDG_RUNTIME_DIR=/tmp/gediscord/p3 GE_MOCK_STEAMID=${IDS[2]} GE_MOCK_FRIEND=${IDS[0]} GE_COOP_LOBBY=wait ${P3ENV} timeout ${RUNSECS:-150} ./ProjectMidas > /out/p3.log 2>&1 &

sleep ${P1DELAY:-2}
DISPLAY=:1 env XDG_RUNTIME_DIR=/tmp/gediscord/p1 GE_MOCK_STEAMID=${IDS[0]} GE_MOCK_FRIEND=${IDS[2]} GE_MENU=lobby \
    GE_COOP_LOBBY=invite GE_COOP_LOBBY_PLAYERS=${PLAYERS:-3} GE_COOP_PUBLIC=$((1 - ${FRIENDSONLY:-0})) GE_COOP_PASSWORD=${HOSTPW:-bond007} \
    GE_DISCORD_AUTOACCEPT=${ASKWAIT:-8} GE_COOP_LOBBY_DELAY=${LOBBYDELAY:-900} ${HOSTENV} timeout ${RUNSECS:-150} ./ProjectMidas > /out/p1.log 2>&1 &

shot() { for i in 1 2 3; do DISPLAY=:$i import -window root /out/p${i}_$1.png; done; }
waitfor() { for k in $(seq 1 ${3:-120}); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 1; done; echo "presence.sh: no '$2' in $1"; return 1; }

waitfor /tmp/gediscord/p1/discord.log '"secrets"' && shot hosting
waitfor /out/p3.log 'joining lobby\|toast: JOINING' 60 && sleep 4 && shot invited
echo "2000 Natalya" > /tmp/gediscord/p1/ask
sleep 4; shot asked
waitfor /tmp/gediscord/p1/discord.log SEND_ACTIVITY_JOIN_INVITE 60
# what Discord does once the host accepts: hand the asker the host's join secret
secret=$(grep SET_ACTIVITY /tmp/gediscord/p1/discord.log | tail -n 1 | python3 -c 'import json,sys; print(json.loads(sys.stdin.read())["msg"]["args"]["activity"]["secrets"]["join"])')
echo "presence.sh: the host's join secret is $secret"
echo "$secret" > /tmp/gediscord/p2/join
waitfor /out/p2.log 'ge: steam: in lobby\|could not join' 30; sleep 3; shot alljoined
for t in ${SHOTS:-40}; do sleep $t; shot mission_$t; done
while pgrep -x ProjectMidas >/dev/null; do sleep 1; done
for i in 1 2 3; do cp /tmp/gediscord/p$i/discord.log /out/p$i.discord.log; done
pkill -f discord_mock.py; pkill Xvfb; exit 0
