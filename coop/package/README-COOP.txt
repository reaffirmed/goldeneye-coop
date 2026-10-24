GoldenEye 007 story co-op (private build)
=========================================

Two players play the solo missions together over Steam. Nobody has to open
ports or type IP addresses.

You need
--------
- Steam running and logged in on both PCs. The two accounts must be Steam
  friends. Nobody has to own anything: the game connects as Valve's free
  "Spacewar" test app, so Steam will show you both as "playing Spacewar".
- Your own GoldenEye 007 (USA) cart image in the Data folder (see
  Data\README if present). The image is not included.

Playing
-------
1. Both players start ProjectMidas.exe, pick a folder and choose
   "3. CO-OP".
2. The host picks "Host a Game". Your online Steam friends are listed under
   "Invite a Friend"; pick one to invite them (or use "Steam Invite Window").
3. The friend either accepts the Steam invite, or picks the host's name
   (marked "Join") on their own CO-OP screen, then picks "Ready Up".
4. When the friend shows as Ready, the host picks "Select Mission", then
   the mission and difficulty as usual. The friend's game follows into the
   mission by itself.
5. After each mission both games return to the lobby, and the host picks the
   next one.

"Host co-op.bat" and "Join co-op.bat" still work as a shortcut: the joiner
waits on the title screen and follows the host into each mission.

Rules
-----
- The host's game runs the guards and the level. Both players see the same
  guards, doors, alarms, destroyed objects and pickups.
- Objectives are shared. Whoever completes the mission finishes it for both.
- G gives your partner ammo for the gun in your hand: half your spare
  rounds (at least one magazine's worth), and the gun too if they don't have
  it. Stand next to them and face them. You keep your gun. The key can be
  changed in the controls menu (Give To Partner).
- When you die, you come back next to your partner once they have been out
  of combat for 4 seconds, and after 20 seconds at most. If you are both dead
  at the same time, the mission fails.

If something goes wrong
-----------------------
Press ~ (tilde) in game to open the console. The co-op lines start with
"ge: steam:" and "ge: coop:". Send those lines along with what happened.

Known gaps in this build
------------------------
- Cutscene cameras follow the host's script; tell me if one looks wrong.
