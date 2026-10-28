GoldenEye 007 story co-op (private build)
=========================================

Two to four players play the solo missions together over Steam, or play
multiplayer against each other (see Versus). Nobody has to open ports or type
IP addresses.

You need
--------
- Steam running and logged in on every PC. Joiners are the host's Steam
  friends, or anyone who finds a Public game (see step 2). Nobody has to own anything: the game connects as Valve's free
  "Spacewar" test app, so Steam will show you both as "playing Spacewar".
- Your own GoldenEye 007 (USA) cart image in the Data folder (see
  Data\README if present). The image is not included.

Playing
-------
1. Everyone starts ProjectMidas.exe, pick a folder and choose
   "3. CO-OP".
   The CO-OP screen is laid out like Modern Warfare 2's lobby: tabs along
   the top (PLAY / LOBBY, GAME SETUP, FIND GAMES / INVITE, CONTROLS, CHAT),
   the tab's rows down the left, your party and the next mission (or the
   arena) on the right, a line along the foot saying what the row under the
   cursor does, and the buttons for the pad (or keys) you used last under
   that. Change tab with LB / RB (L1 / R1) on a pad, , and . on the keyboard
   (the watch's page keys), or a click on the tab. Up / Down (D-pad, arrow
   keys) pick a row; a row with arrows either side of its value steps with
   Left / Right or a click on an arrow; A / Enter selects; B / Esc goes back.
2. The host picks "Host a Game". Your online Steam friends are listed on
   the INVITE tab; pick one to invite them (or use "Steam Invite Window").
   Invite up to three friends.
   Before or after you host you can set "Lobby" to Public, so anyone with
   this build sees your game on FIND GAMES, and set a "Password"
   (type it and press Enter; Enter on an empty line removes it). Your Steam
   friends never need the password; it keeps strangers out.
   On the "Character" row (PLAY or LOBBY tab)
   pick who you play as: the multiplayer cast (Bond in each outfit,
   Natalya, Trevelyan, Xenia, Ourumov, Boris, Valentin, Mishkin, Jaws,
   Mayday, Oddjob, Baron Samedi, the guards, scientists and the rest), or
   "Mission Bond", Bond in the mission's own outfit. Selecting the row
   opens the character select: every character on a film strip along the
   top, the one you are on large in the middle, turning, with its name and
   file beside it. Step with the arrow keys, the mouse wheel, a click on the
   strip, or LB / RB or the d-pad on a pad; Enter / A (or a click
   on the photo) takes it, Esc / B leaves your pick as it was. On the row
   itself Left / Right still step back and forth without opening it. The
   pick is looks only: everyone plays the same. The others see your pick in
   the party list, and you look like it on their screens. Your pick is
   kept for next time.
3. Each friend either accepts the Steam invite, or picks the host's name
   (marked "Join") on their own CO-OP screen, then picks "Ready Up".
   Public games are listed on the FIND GAMES tab with the host's name and
   how many are in; one marked "Password" asks for it before you join.
   If the CO-OP screen says "Wrong password.", try again.
   The CONTROLS tab has your profile, the controller layout, Field of View,
   Mouse Look, Mouse Speed and voice chat; Left / Right step each one
   (the same settings as in the Options menus).
4. The host can set the rules on the GAME SETUP tab before starting (the
   others see them there):
   - Continue: jumps to the next mission you have not beaten together.
   - When Down: Revive or Timer, Revive only, or Timer only.
   - Respawn (with a timer): 3-20 s (default, sooner when it is quiet) or
     a fixed wait from 5 s to 90 s.
   - Bleed Out (Revive only): how long a partner has to revive you, from
     15 s to 3 min (default 60 s).
   - Lives: Unlimited (default), or 1, 2, 3, 5 or 10 each per mission.
     Every time you go down costs one; when you go down on your last life
     you are out until the mission ends.
   - Guards: Normal, Hard (tougher, more of them) or Brutal.
   - Player Scaling: Auto (default) makes guards tougher, sharper and more
     numerous with 3 or 4 players, and follows players who leave or drop
     in. Off keeps the two-player fight; 3 Players or 4 Players fixes the
     level whoever is playing.
   - Guard AI: Classic (default, the original guards) or Smarter: guards
     look into noises, call nearby guards for help, take cover, flank and
     spread out over the players. It is the host's Port Option of the same
     name, so changing it here changes it there too.
   - Friendly Fire: Off (default: your shots, grenades, mines and rockets
     pass your partners by), Reduced (half damage) or Full. A partner who
     is already down can't be hurt. The stats page counts friendly hits.
   - Host Leaves: New Host (default) or Back to Lobby. See "When
     someone leaves" below.
5. When every friend shows as Ready, the host picks "Select Mission" (or
   Continue), then the mission and difficulty as usual. The friends' games
   follow into the mission by themselves.
6. After each mission everyone sees a stats page with a column per player,
   then your own stats, then WHAT NEXT, where everyone votes: Replay the
   mission, the Next mission (after a win) or Back to the lobby. Pick one
   with Up / Down (or the cursor) and press A / Enter; you can change your
   vote until it is decided. It is decided when everyone has voted, or 30
   seconds after the first vote. Most votes win; a tie goes the host's way
   if the host is in it, otherwise to the lobby. The mission
   time is the team's (the host's clock), so beating a mission's target time
   together unlocks its cheat for everyone, as in solo.
7. Your campaign is the host's: a joiner's lobby shows "Next:" with the
   host's next mission, and it is saved for that joiner too, so Continue
   works for them next time they host with you.

"Host co-op.bat" and "Join co-op.bat" still work as a shortcut: the joiner
waits on the title screen and follows the host into each mission.

Rules
-----
- The host's game runs the guards and the level. Both players see the same
  guards, doors, alarms, destroyed objects and pickups, the mines, grenades
  and gadgets anyone throws or plants, the guns guards drop, and every
  explosion (which hurts whoever is close, as in solo).
- Pickups are shared: a gun, ammo, key or mission item one player takes,
  everyone gets, and the same goes for a gun a guard dropped.
- Either player can use consoles, switches and the Runway plane.
- Objectives are shared. Whoever completes the mission finishes it for both.
- G gives the partner you face ammo for the gun in your hand: half your spare
  rounds (at least one magazine's worth), and the gun too if they don't have
  it. Stand next to them and face them. You keep your gun. The key can be
  changed in the controls menu (Give To Partner).
- The middle mouse button pings: aim at a spot and press it, and a diamond
  in your colour marks that spot for everyone, with your name and how far
  away it is, plus a square on the radar. Aim at a guard and it is marked
  in red as ENEMY and the marker follows that guard. A ping lasts 10
  seconds; your next ping moves it. When it is off screen the diamond
  sits at the edge of the screen, on the side to turn to. The key can be
  changed in the controls menu (Ping).
- When you go down, what happens depends on the host's When Down rule.
  Revive: a partner stands over you for a moment to bring you back. Timer:
  you come back next to a partner once they have been out of combat for 4
  seconds, and after 20 seconds at most (or after the host's fixed
  Respawn wait). With Lives set, the screen shows how many you have left
  while you are down, and a partner who has none left shows OUT. If
  everyone is down at the same time, the mission fails.
- T opens a chat line (in a mission and in the lobby). Type and press Enter
  to send it to everyone, or Escape to close it. Lines show at the bottom
  left in each player's colour. The key can be changed (Chat).
- Voice chat goes through Steam Voice. Hold V to talk (Push to Talk in the
  controls), or set Voice Chat to Open Mic or Off in Options > Audio. You
  hear partners from where they stand in a mission: quieter far away, and
  from the left or right with Stereo Pan on. A speaker icon shows next to
  whoever is talking, and TALK at the top while your mic is open. Voice
  Volume sets how loud partners are, and each partner can be muted in the
  lobby (the Voice row under their name) or on the watch's Audio page.
- When a partner completes an objective, a note across the middle of the
  screen says who did it: "MOCK COMPLETED OBJECTIVE A" and the objective.
- While you are down (or out of lives), the screen fades back in through a
  partner's eyes ("WATCHING <NAME>"); fire switches to the next partner and
  aim to the one before. Use frees the camera: look around with the mouse
  and fly with WASD, as in photo mode but without the 8 metre limit; fire
  or Use goes back to a partner's eyes. Your own view comes back when you
  are revived or respawn.
- Skipping a cutscene (the mission intro, or an ending that waits for a
  press) is a vote: "SKIP 1/2 - WAITING FOR <NAME>" shows in the corner and
  it skips for everyone once more than half of you have pressed (both of
  two, two of three, three of four). Otherwise it plays out as usual.
- P is photo mode, in solo too: the HUD and gun disappear and the camera
  comes loose. Look around with the mouse, fly with WASD (it goes the way you
  look, stays above the floor and within about 8 metres of Bond), and press
  fire to save a picture in the Photos folder next to the game. P or Escape
  goes back. The world keeps going (guards can still shoot you) and Bond
  stands still meanwhile. The stats page after a co-op mission says how many
  photos you took. Your own Bond is not in the photos, your partners are.
  The key can be changed (Photo Mode).
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

When someone leaves
-------------------
- If the host's game quits or crashes, the next player in the lobby list
  becomes the host straight away. If the host just loses its connection,
  everyone else sees "HOST NOT RESPONDING" with a countdown, so a host
  whose connection only hiccuped can come back; after about 5 seconds
  (15 while a mission is still loading) the next player takes over. Either
  way the mission carries
  on from where it was: guards, doors, objectives and everyone's position
  stay as they were. The others connect to the new host by themselves.
- With the Host Leaves rule on Back to Lobby, everyone returns to the lobby
  instead, with the next player as host.
- The old host can come back: start co-op again and join the lobby, or
  accept an invite, and you drop in to the mission beside the others.
- When the host wants to quit, the watch's Abort asks first:
  HAND OVER: you leave, the next player becomes host and the others play on.
  END ALL: the mission ends for everyone and you all go back to the lobby.
  "Leave Lobby" as host also hands the lobby to the next player.
- If a joiner drops, the others see "<name> lost connection" and their Bond
  stands still (guards ignore it). If they are not back within 30 seconds
  the Bond is removed and "<name> disconnected" shows. They can join again
  like a late joiner.

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
     the player count suits them: 2 v 1 with three players, 2 v 2 and
     3 v 1 with four. In a team game team-mates can hurt each other, and a
     team-mate killed takes a point off your team, as in the original.
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
- Everyone plays by the original rules: your own Multiplayer Options for
  Friendly Fire and LTK Health don't apply to an online match.
- Chat (T) works in versus too. A host leaving in the middle of a versus
  match has not been tested yet.

Steam and Discord status, invites
---------------------------------
Your Steam friends see what you are doing, in your profile's game info and
on hover in the friends list: "In the co-op lobby (2/4)", "Co-op: Dam,
00 Agent (2/4)", "Versus: Temple (3/4)", "Solo: Dam, Agent" or "In the
menus". Friends in one lobby are grouped together in the list. (Because the
game runs as Spacewar, the line under your name in the friends list itself
only says Spacewar's own "At the main menu" / "Waiting for match".)

Invites and joining work from anywhere in the game, not only the CO-OP
screen:
- Accept a Steam invite, or right-click a friend in the lobby and pick
  "Join Game", while you are in the menus: the game opens the CO-OP screen
  in their lobby. If you are hosting your own lobby you leave it first.
- If you are in a mission at the time, a message says you will join when
  the mission ends (abort it from the watch to go now).
- Have the game running before you accept: while it runs as Spacewar,
  Steam would start Spacewar itself, not this game. (Discord does start
  this game for an accepted join; it learns where the game is the first
  time the game runs with Discord open.)
- "Join Game" is offered while the lobby has room, also during co-op
  missions (you join beside the others), but not during a versus match.

Discord (optional): with Discord open on the same PC, your Discord friends
see the same status with a timer, the party size, and an "Ask to Join"
button while there is room. When someone asks, the host's CO-OP screen shows
"<name> asks to join" with Let In / Turn Down (requests go away after 30 s).
Once let in, their game joins your lobby by itself. If your lobby has a
password, a Discord join skips it: you let them in yourself. If they are not
your Steam friend, set your Lobby to Public first; Steam keeps everyone else
out of a Friends Only lobby (their screen says "Couldn't join that game.").
Discord needs this build's Discord application id in discord_appid.txt next
to ProjectMidas.exe. Without the file, or without Discord running, the game
plays as before. Steam starts with the game now so the status shows from
the first menu; put GE_NO_PRESENCE=1 in the environment to turn both off.

Dropping in
-----------
You can join a game that is already in a mission: from an invite, Join a
Friend's Game, or Public Games (the row says "Drop In"). You watch a
partner for a moment and come in beside them once things are quiet (at
most 20 seconds).

Accessibility
-------------
Options > Gameplay > Accessibility:
- Sound Captions: gunfire, explosions, doors opening and alarms show as a
  line such as "[ GUNFIRE - BEHIND ]", with the side they came from. (The
  game has no spoken dialogue to subtitle; briefings are already text.)
- Colour Blind: Red-Green or Blue-Yellow player colours for name tags, the
  radar and the chat feed.
- Text Size: 100% to 200% for co-op text, captions and mission messages.
- Screen Shake: Off stops explosions shaking the view.
- Crouch Key and Walk Key: Hold or Toggle (press once on, again off).

Updates
-------
When the game starts it asks GitHub whether a newer release is out (one
small request; nothing downloads by itself). If there is one, the PLAY tab
of the CO-OP screen shows "Update to v..." at the top. Pick it and the game
downloads the new zip, checks it against the checksum GitHub lists for it,
and replaces its own files (ProjectMidas.exe, the DLLs, the .bat files and
these notes). Then pick "Restart Now". Your cart image, saves, profiles,
reports and midas.ini are never touched, and the download is thrown away if
it doesn't match. If a file can't be replaced, everything is put back.
- A host turned you away with "Update needed to join": the game looks for
  the update at once and offers it, so you can install it and join again.
  If the host is the one on the old version, it says so.
- CONTROLS tab: "Update Check" On / Off (midas.ini [display] auto_update),
  and "Check Now", which shows your version and asks GitHub again.
- Behind a firewall that blocks GitHub, download the zip from the releases
  page instead and unzip it over this folder.

If something goes wrong
-----------------------
- Something looks wrong: press F8 (anywhere, in a mission or the menus). The
  game saves a bug report zip on your Desktop with its log, the co-op state,
  your settings and a screenshot, and copies it, so Ctrl+V in Discord
  attaches it. Send it with a line on what happened. (Typing "bugreport" in
  the ~ console does the same.)
- The game crashes: it saves a crash report (log, co-op state, settings and
  a memory dump) in Data\Crashes and asks whether to put it on your Desktop
  as a zip. Say yes and paste it into Discord: it is on the clipboard too.
  If you say no, or the game froze instead, it asks again the next time you
  start it.
- Press ~ (tilde) in game to open the console. The co-op lines start with
  "ge: steam:", "ge: coop:", "ge: versus:", "ge: presence:",
  "ge: discord:" and "ge: update:".

Known gaps in this build
------------------------
- Cutscene cameras follow the host's script; tell me if one looks wrong.
- With four players the stats page cuts names to about five letters.
- Three and four players are tested on a fake Steam only so far.
- Everyone needs this build: it cannot play with the earlier co-op builds.
