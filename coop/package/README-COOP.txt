GoldenEye 007 story co-op (private build)
=========================================

Two to four players play the solo missions together over Steam, or play
multiplayer against each other (see Versus). Nobody has to open ports or type
IP addresses.

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
   Under "Your Character" (and under your name once you are in a lobby)
   pick who you play as: the multiplayer cast (Bond in each outfit,
   Natalya, Trevelyan, Xenia, Ourumov, Boris, Valentin, Mishkin, Jaws,
   Mayday, Oddjob, Baron Samedi, the guards, scientists and the rest), or
   "Mission Bond", Bond in the mission's own outfit. A / fire steps forward,
   L / R (or left / right on the d-pad) step back and forth. The others see
   your pick under your name, and you look like it on their screens. Your
   pick is kept for next time.
3. Each friend either accepts the Steam invite, or picks the host's name
   (marked "Join") on their own CO-OP screen, then picks "Ready Up".
4. The host can set the rules in the lobby before starting:
   - Continue: jumps to the next mission you have not beaten together.
   - When Down: Revive or Timer, Revive only, or Timer only.
   - Guards: Normal, Hard (tougher, more of them) or Brutal.
   - Friendly Fire: Off (default, your shots pass your partners by) or On.
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
- You see what your partners hold: their gun is in their hands (both, with
  a pair), and they move like multiplayer Bonds, aiming up and down where
  they look, crouching and sidestepping. When they shoot you see the muzzle
  flash and the tracers, hear the gun, and see the bullets hit: sparks,
  bullet holes and hit sounds on walls, puffs on whoever they hit. Only
  what you see is copied; damage works as before.
- Each player has a colour (yellow, cyan, pink, green) on name tags, the
  radar and the partner status at the side of the screen.
- Name tags over partners are small, shrink with distance, fade out when a
  partner is right beside you, and fade (but stay) when they are far away.

Versus (multiplayer)
--------------------
The original multiplayer, online: two to four players, each on their own PC
with the whole screen to themselves.
1. Meet in the lobby as for co-op (the host hosts, friends join and Ready Up).
2. The host sets Mode to Versus under Rules, then picks:
   - Arena: the multiplayer arenas (Temple, Complex, Caves, Library,
     Basement, Stack, Facility, Bunker, Archives, Caverns, Egyptian, plus
     Statue and Cradle), or Random. Arenas too small for everyone (Egyptian
     takes two, Bunker three) are skipped.
   - Scenario: Normal, You Only Live Twice, The Living Daylights (Flag Tag),
     The Man with the Golden Gun, Licence to Kill, and the team games when
     the player count suits them.
   - Game Length: no limit, a time limit or a kill limit.
   - Weapons: the original weapon sets (Slappers Only, Pistols, ... Random).
   Everyone's character is the one picked under their name.
3. The host picks Start Match. Everyone's game loads the arena.
4. The match plays by the original rules: scores, the radar, Start to come
   back after dying, the scenario's flag or golden gun. When it ends,
   everyone sees the results; press Start to go back to the lobby, where the
   host can start another.
- Pausing is off in versus (the others keep playing); the watch still opens.
- Each PC has its own copy of the ordinary weapons and armour lying around,
  so two players can both take the same gun. The Flag Tag flag and the
  Golden Gun are shared: only one player can hold them.

If something goes wrong
-----------------------
Press ~ (tilde) in game to open the console. The co-op lines start with
"ge: steam:", "ge: coop:" and "ge: versus:". Send those lines along with what happened.

Known gaps in this build
------------------------
- Cutscene cameras follow the host's script; tell me if one looks wrong.
- With four players the stats page cuts names to about five letters.
- Three and four players are tested on a fake Steam only so far.
- Everyone needs this build: it cannot play with the earlier co-op builds.
