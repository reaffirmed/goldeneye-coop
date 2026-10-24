GoldenEye 007 story co-op (private build)
=========================================

Two to four players play the solo missions together over Steam. Nobody has to open
ports or type IP addresses.

You need
--------
- Steam running and logged in on every PC. Each joiner must be a Steam
  friend of the host. Nobody has to own anything: the game connects as Valve's free
  "Spacewar" test app, so Steam will show you both as "playing Spacewar".
- Your own GoldenEye 007 (USA) cart image in the Data folder (see
  Data\README if present). The image is not included.

Playing
-------
1. Everyone starts ProjectMidas.exe, pick a folder and choose
   "3. CO-OP".
2. The host picks "Host a Game". Your online Steam friends are listed under
   "Invite a Friend"; pick one to invite them (or use "Steam Invite Window").
   Invite up to three friends.
3. Each friend either accepts the Steam invite, or picks the host's name
   (marked "Join") on their own CO-OP screen, then picks "Ready Up".
4. The host can set the rules in the lobby before starting:
   - Continue: jumps to the next mission you have not beaten together.
   - When Down: Revive or Timer, Revive only, or Timer only.
   - Guards: Normal, Hard (tougher, more of them) or Brutal.
5. When every friend shows as Ready, the host picks "Select Mission" (or
   Continue), then the mission and difficulty as usual. The friends' games
   follow into the mission by themselves.
6. After each mission everyone sees a stats page with a column per player,
   then returns to the lobby, and the host picks the next one.

"Host co-op.bat" and "Join co-op.bat" still work as a shortcut: the joiner
waits on the title screen and follows the host into each mission.

Rules
-----
- The host's game runs the guards and the level. Both players see the same
  guards, doors, alarms, destroyed objects and pickups.
- Objectives are shared. Whoever completes the mission finishes it for both.
- G gives the partner you face ammo for the gun in your hand: half your spare
  rounds (at least one magazine's worth), and the gun too if they don't have
  it. Stand next to them and face them. You keep your gun. The key can be
  changed in the controls menu (Give To Partner).
- When you go down, what happens depends on the host's When Down rule.
  Revive: a partner stands over you for a moment to bring you back. Timer:
  you come back next to a partner once they have been out of combat for 4
  seconds, and after 20 seconds at most. If everyone is down at the same
  time, the mission fails.
- Keys 1 to 6 open the weapon menu at the top of the screen (the game keeps
  running): 1 melee, 2 pistols, 3 rifles, 4 heavy, 5 explosives, 6 mission
  items such as the covert modem, keys and the camera. Press the key again to
  move down the list and fire to take the highlighted one.
- Each player has a colour (yellow, cyan, pink, green) on name tags, the
  radar and the partner status at the side of the screen.

If something goes wrong
-----------------------
Press ~ (tilde) in game to open the console. The co-op lines start with
"ge: steam:" and "ge: coop:". Send those lines along with what happened.

Known gaps in this build
------------------------
- Cutscene cameras follow the host's script; tell me if one looks wrong.
- With four players the stats page cuts names to about five letters.
- Three and four players are tested on a fake Steam only so far.
