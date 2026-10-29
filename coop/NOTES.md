# Story co-op notes

Working notes for online story co-op (Steam, Spacewar AppID 480), following the coop-anything playbook.

## Game identity

- Base: Project Midas 1.0.2b (main @ 9062f9f), PC port of the GoldenEye N64 decomp.
- Cart: GE (USA), sha1 abe01e4aeb033b6c0836819f549c791b26cfde83.
- Builds: Windows i686 (mingw) and Linux i686, 32-bit. Needs the 32-bit steam_api.dll / libsteam_api.so.

## Observations (checked in source)

- Engine supports 4 players in multiplayer (g_playerPointers, set_cur_player, split viewports).
- Guard targeting goes through getCurrentPlayerProp() and TARGET_BOND (src/game/chraction.c, chrCanSeeBond).
- ~3100 references to g_CurrentPlayer / getCurrentPlayerProp in src/game.
- Solo-only paths gated on getPlayerCount() == 1 (bondview2.c, bondinv.c, ...).
- No network code in the repo. Upstream CONTRIBUTING lists co-op and netplay as in development, not public.
- Input record and replay exists (port/src/inputrec.c, GE_INPUTREC), plus a headless build (GE_HEADLESS).

## Plan

1. Second Bond in a solo mission (UDP transport on localhost for tests).
2. Guards pick a target among players.
3. Host-authoritative sync: guards (chrnum, pos, facing, anim, health, target), doors, objects, alarms, objectives, mission end.
4. Steam transport: lobby, invite, ISteamNetworkingMessages, dynamic loading of the flat API.
5. Cutscenes, death and respawn, mission end, saves.

## Unknowns

- Whether float results match between Windows and Linux builds (affects any lockstep idea; state sync chosen to avoid it).
- How level setup scripts reference the player beyond TARGET_BOND.

## Cloud test setup (verified 2026-10-24)

- The repo Dockerfile's wget can't reach GitHub through this sandbox's TLS proxy. Workaround: download the two SDL2 tarballs on the host (sha1 checked, both match the Dockerfile) and COPY them in.
- HOST_UID=0 breaks the Dockerfile's useradd step; build with HOST_UID=1000 and run containers with `-u 0`.
- `GE_HEADLESS=ON` does not link on 1.0.2b (missing port* symbols from video/input). The GE_VIDEO Linux build links and runs.
- `coop/docker/Dockerfile.run` adds Xvfb + 32-bit Mesa on top of the build image. `coop/docker/smoke.sh` boots Dam via GE_STAGE and screenshots the virtual display. Dam reached gameplay at ~58 fps with software GL.
- `GE_SHOT` is listed in the env help but not implemented in this tree; screenshots come from `import -window root`.

## Provenance: GoldenEye 007 Plus

The owner approved (2026-10-24) porting code from GoldenEye 007 Plus
(github.com/Joshua-1248/GoldenEye-007-Plus, an N64 mod of the same decomp with 1-4
player split-screen campaign co-op) where it helps, improving it where possible.
GE+ has no blanket license for its own changes, so this fork stays private.
Every ported piece is listed here with its source file; rewritten pieces say so.

| Piece | Source in GE+ | Ported or rewritten |
| --- | --- | --- |
| Guard target state machine (acquire, commit on stimulus, release after 10 s, passive-only scanning, no ping-pong while fresh) | src/game/chr.c chrCoopGetTargetPlayer / chrCoopSetTargetPlayer | Ported, cut to two targets (local Bond, remote puppet) |
| Stimulus hook points (seen, set target, alerted, shot by player) | src/game/chraction.c setSeenBondTimeToNow, chrlvSetTargetToPlayer, chrlvAlertGuardToPlayerPosition, handles_shot_actors | Same hook points; our code |
| "Swap Bond around the guard's action and fire ticks" | src/game/chr.c chrTick (set_cur_player around chrlvActionTick and chrlvTriggerFireWeapon) | Rewritten: we swap getCurrentPlayerProp() to the puppet instead of switching player structs |

## Milestone 1 results (2026-10-24)

- Two copies connected over UDP on localhost; each spawns the other's puppet Bond.
- Verified in screenshots both ways (host sees the joiner's Bond after it walks ahead; joiner sees the host's).
- Fixed: both Bonds start on one pad and blocked each other; the puppet is non-solid until 90 units apart.
- Open: the first-person gun and HUD were missing in some two-copy runs; under investigation.

## Milestone 2 results (2026-10-24, host side)

- Host guards pick the remote player as "Bond" and the retail AI chases and shoots the puppet.
- Damage a host guard deals to the puppet is sent as a hit packet and applied to the joiner's Bond (log: "guard 6 hit the peer #1" on host, "hit from host guard #1" on joiner).
- Not done yet: the joiner still runs its own guards, so it is attacked by two sets. Milestone 3 turns the joiner's guards into puppets of the host's.
- Hit packets are unreliable UDP for now; lost hits are lost damage. Needs acks before release.

## Milestone 3, guards (2026-10-24)

- Client guards mirror the host: guard 6 tracked within a few units, same animation and frame, across idle, crouch, run and shoot.
- A client shot on a guard goes to the host (GE_COOP_TESTSHOT drives it in the harness), the host applies it as the remote player's hit, the guard dies on the host, the client plays the mirrored death animation, and removes the body when the host frees it.
- Bug found and fixed: the receive buffer was 512 bytes, which truncated 32-guard snapshot packets (1,168 bytes) and made the client delete those guards.
- Still to do: guards the host spawns mid-mission that the client has not (clones, reinforcements); guard gunfire visuals on the client (muzzle flash and tracers come from the fire tick, which the client skips); explosions damaging guards on the client; doors, objects, alarms and objectives; reliable delivery for hits and shots.

## Milestone 3, world sync (2026-10-24)

Each item verified with two copies in Dam using the harness switches listed below.

- Reliable event channel: hits, shots on guards, doors, object damage, pickups and mission end ride seq/ack events that resend every 4 ticks until acknowledged.
- Doors: doorSetOpenState is mirrored both ways, keyed by the door's byte offset in the setup prop defs (GE_COOP_TESTDOOR).
- Mid-mission spawns: unknown chrnums in a snapshot get a local stand-in with the same body, head and gun (GE_COOP_TESTSPAWN). The puppet now uses chrnum 30000 so it can't collide with spawn numbers.
- Guard gunfire on the joiner: muzzle flash and gun sound driven by the host's firecount changes. Tracers are not mirrored yet.
- Objects: the host owns object damage. The joiner forwards its own hits on setup objects; the host applies them and echoes every result, which the joiner replays with the same numbers (GE_COOP_TESTOBJ). The joiner's explosions don't damage objects or chrs (they are replays of the host's), and nobody's explosion hurts the puppet.
- Stage flags and alarm follow the host; room, deposit, photo and key-copy criteria are OR'd between the two games (GE_COOP_TESTWORLD).
- Pickups: the first player to take a setup item removes it from the other world; kept items count for the team's collect objectives (GE_COOP_TESTPICK).
- Mission end: completing the mission (bossReturnTitleStage) ends it for both (GE_COOP_TESTEND). The peer's Bond is removed when it leaves the stage or goes silent for 5 s.
- Death (owner's choice): a dead player respawns beside the partner, with full health and the same inventory, once the partner has been out of combat for 4 s (at least 3 s after the death fade, at most 20 s). If both players are dead, the mission fails as usual (GE_COOP_TESTDIE).

Known gaps: grenades and mines thrown by the joiner don't hurt anything; (cutscenes: see "Synced cutscenes" below); guard tracers; vehicles (Runway plane, tank) run on both sides; and only the host can trigger level exits that check "Bond's" position.

## Milestone 4, Steam (2026-10-24)

- port/src/coop_steam.c: Spacewar (480) over the flat API, loaded at run time; follows the coop-anything Steam playbook (manual dispatch, fixed struct offsets, sender identity at 12 or 16, LobbyCreated handled twice, session accept for lobby members only).
- Host: `-coophost` (GE_COOP=steam). Friends-only lobby tagged `gecoop=1`, rich presence connect string, invite overlay.
- Joiner: `-coopjoin` (GE_COOP=steam:join) finds a friend playing 480 with a lobby; `+connect_lobby <id>` and invites (333/337) also work.
- The joiner follows the host into each mission from the menus (HELLO packet: stage, difficulty, mission serial).
- Tested over the mock (coop/mock/steam_mock.c, coop/docker/two_player_steam.sh): lobby, friend discovery, session accept, follow into Dam, full game sync. steam_api.dll exports checked against the code: all present (Steamworks.NET Standalone 2025.164.1, Windows-x86, SHA-256 fdafb7c6...).
- Not yet tested on real Steam: needs two accounts on two PCs (the owner).
- Package: coop/package (bat launchers, README-COOP.txt) + Release mingw build + steam_api.dll + steam_appid.txt.

## Lobby screen (2026-10-24)

- Mode select (Files menu) gains "3. CO-OP" (cheats move to row 4). It opens
  MENU_COOP_LOBBY (front.c menu1D), drawn on the Options paper with the sheet
  row metrics. Logic lives in coop.c (coopLobby*), Steam calls in
  coop_steam.c.
- Steam now comes up as "steam:idle" when the screen opens (friend list, no
  lobby); Host = "steam:host" (no overlay), Join = first hosting friend or a
  picked friend's lobby. SteamAPI is never shut down mid-run, Leave only
  leaves the lobby. coopNetStart() replaces the env-only init.
- HELLO carries ready + inlobby. Host's Select Mission needs a Ready partner,
  then the normal mission/difficulty screens; the joiner follows as before.
- After a co-op mission, the failed/complete screens go back to the lobby
  (coopLobbyAfterMission). Ready resets when a mission starts.
- Friend names are squashed to ASCII for the menu font.
- Test: coop/docker/two_player_lobby.sh (GE_MENU=lobby, GE_COOP_LOBBY=
  host|invite|join|wait, GE_COOP_LOBBY_DELAY). Verified with the mock: join
  from list, invite + accept, Ready, start Dam, mission end back to the
  lobby, second mission started from the lobby. Real Steam not yet tested.

## Co-op difficulty (2026-10-24)

- Lobby setting COOPSET_DIFFICULTY (host's, copied in HELLO): 0 Normal, 1 Hard,
  2 Brutal. Only with a partner connected; solo and Normal are unchanged.
  Logic in src/game/coop_difficulty.c; coopDifficultyCount/Name for the lobby row.
- Hard: guard health x1.5, damage x1.15, aim x1.15, 1 extra guard per 4 armed
  soldiers (max 6). Brutal: x2, x1.3, x1.3, 1 per 2 (max 12).
- Health goes through get_007_health_mod (chrAllocate and scripted set-health);
  damage and aim scale g_AiDamageModifier/g_AiAccuracyModifier after
  lvlSetMultipliersForDifficulty each tick. Host only (it owns the guards).
- Extra guards: copies of generic soldiers beside their source, 2 s after both
  players are in, standard guard AI, chrnums 20000+. Both games add chr slots and
  anim/model spare slots (the stock 10 anim spares ran out after 9 copies).
- Harness: GE_COOP_DIFFICULTY=<mode> on the host. Verified on Dam with the mock:
  12 (Brutal) / 6 (Hard) extras on the host, all mirrored on the joiner; a PPK
  chest shot from the joiner kills in 1 on Normal, 2 on Brutal; a guard's hit on
  the partner 0.062 on Normal, 0.081 on Brutal (x1.3).

## Mission stats and campaign record (2026-10-24)

- src/game/coop_stats.c. Per player: kills, shots, hits (accuracy), head hits,
  deaths, revives given, respawns. Each game counts its own shots and deaths;
  the host owns kills and hits on guards. The joiner's forwarded shots (and
  blasts set off for it, explosion.c) run between coopStatsBlameBegin/End,
  which moves what they add to the host's counters into the partner's column,
  so the host's own solo numbers stay its own.
- Back in the menus each game sends its final column once (COOPEV_STATS); the
  host's carries the joiner's kills and hits. The statistics page (menu0D,
  shown after a win and after a failure) draws both columns with the players'
  names in place of the solo hit table; a number not in yet shows "-".
- coop_campaign.txt next to the game: one line per partner and mission won
  together, `<steam:id | name:Name> <mission> <agent|secret|00|007> <name>`.
  coopCampaignNext() gives the lobby the mission after the furthest one won
  with the current partner. GE_COOP_TESTWIN=1 counts any end as a win (harness).
- The joiner keeps its own save folder when it follows the host (it used to be
  forced to folder 1), so a co-op win unlocks the next mission in its folder.

## Give to partner (2026-10-24)

- give_partner (default G, unbound on pads; PORTACT_GIVE_PARTNER, appended):
  partner alive, within 250 units across and 200 up or down, and within 60
  degrees of where you look. Sends half the reserve of the right-hand gun's
  ammo type (at least a magazine's worth when you have it; the loaded
  magazine stays). Logic in src/game/coop_give.c.
- COOPEV_GIVE carries gun, ammo type, rounds and stage; the receiver adds the
  gun if new (bondinvAddInvItem), adds what fits under the type's maximum
  (give_cur_player_ammo, as a pickup) and answers with COOPEV_GIVEBACK, which
  refunds what did not fit. A give that arrives in another stage, or to a dead
  partner, is refunded whole.
- The giver keeps the gun: ammo is moved, never made, so a copy is only worth
  the rounds that came with it. Only ammo types 1..AMMO_GGUN and guns from
  the throwing knife to remote mines (no watch laser) are given; mission
  gadgets never are.
- Harness: GE_COOP_TESTGIVE=<s> faces the partner and presses give <s>
  seconds into the mission; GE_COOP_TESTGIVEWEAPON=<item> first draws that
  gun with 100 more rounds. Verified both ways in Dam: host PP7 46 of 93 to
  the joiner (93 -> 139); joiner KF7 35 of 70 plus the gun to the host (no
  KF7 before, 0 -> 35); too far gives "Get closer" and moves nothing.

## When Down: revive / timer (2026-10-24)

- Lobby row "When Down" (host's, copied in HELLO): Revive or Timer (default),
  Revive only, Timer only. Revive: a living partner stands over your death
  spot; the downed screen shows "BEING REVIVED" with a bar, the partner's HUD
  "DOWN - REVIVE THEM". Timer: back beside the calmest living partner after 4
  out-of-combat seconds, 20 s at most. Revive only bleeds out, then OUT until
  the mission ends; everyone down fails it.
- Harness: GE_COOP_DOWNRULE=0|1|2, GE_COOP_TESTDIE=<mission ticks>.

## 3 to 4 players (2026-10-24, COOP_VERSION 4)

- Star: the host is player 0, joiners are 1..3 (host's transport slot + 1).
  Joiners talk only to the host. The host relays each joiner's STATE to the
  others (stamping `player`) and forwards events by CoopEvent.to (a player
  or COOP_ALL), stamping `from`. HIT goes to the victim; SHOTCHR, EXPLODE and
  a joiner's OBJDMG to the host; door, pickup, mission end, AI commands,
  stats and the host's OBJDMG to all; GIVE/GIVEBACK to one player.
- One reliable channel per transport peer (coop.c CoopChan), one remote per
  player (CoopRemote sR[4], puppet chrnum 30000 + player). HELLO from the host
  carries `youare` and a roster of all four slots (id, name, ready, in lobby).
- Guards pick targets over all players in turn; level scripts that check Bond
  loop over living partners (coopAltBondBegin(k) in chrai.c).
- Transport: COOP_NET_MAX_PEERS 3, coopNetSendTo/RecvFrom/PeerConnected per
  peer; the Steam lobby holds 4. Mock: GE_MOCK_FRIEND takes a comma list.
- HUD: per-player colour (yellow, cyan, pink, green) on name tags, radar
  dots and stacked partner status blocks. Stats page draws one column per
  player (names cut to about 5 letters with 4). Campaign record has a line
  per partner; Continue uses the least progress among those present.
- Lobby: player list, invites shown while under 4. Select Mission needs all
  partners Ready.
- Test: coop/docker/party_lobby.sh PLAYERS=2..4 (HOSTENV, P2ENV..P4ENV,
  ALLENV, SHOTS, RUNSECS; GE_COOP_LOBBY_PLAYERS=n makes the host wait for n).
  Verified with the mock, 3 and 4 copies: all follow the host into Dam,
  puppets for every partner, guards hit joiners, give from player 4 to 3
  through the host, revive, same stats page on every game. Real Steam not
  yet tested with more than one account.

## Weapon menu: mission items (2026-10-24)

- Slot 6 in src/game/weapon_menu.c lists the watch inventory's items from
  ITEM_BOMBCASE up (gadgets, keys, documents), read through
  bondinvGetTextbyInvIndex so prop-held items are included, and named with
  the level's own text (bondinvGetNameByIndex: "COVERT MODEM", not "BUG").
  Taking one uses gunRequestHandWeaponChange like a gun, which ends in the same
  currentPlayerUnEquipWeaponWrapper the watch calls.
- Key: weapon_slot_6 (default 6; PORTACT_WEAPON_SLOT6 and pad map +30,
  appended), bind row "Mission Items".
- Harness: GE_WEAPMENU_TEST=items. Verified on Dam 00 Agent (GE_DIFF=2):
  slot 6 shows Covert Modem, fire puts it in hand.

## Friendly fire and the partner-shot crash (2026-10-24)

- Bug (real play test): shooting a partner killed their puppet on the
  shooter's game, and the dead chr's freed model crashed coopPuppetUpdate
  (setsuboffset: no objinst, signal 11). Reproduced with GE_COOP_TESTFF.
- Fix: handles_shot_actors calls coopShotPuppet first; a puppet is never
  damaged locally. Lobby row "Friendly Fire" (COOPSET_FRIENDLYFIRE, was
  SPARE1; default Off). On: the hit goes to that player as COOPEV_HIT,
  0.125 x the gun's destruction amount (a guard's scale), x2 on the head.
- Harness: GE_COOP_TESTFF=<mission ticks> shoots the first partner in the
  chest every 2 s; GE_COOP_FRIENDLYFIRE=0|1. Verified with two copies: Off,
  23 shots each way, no damage, no crash; On, the partner dies, respawns, no
  crash.

## Partners' guns, shots and characters (2026-10-24, COOP_VERSION 5)

- src/game/coop_look.c. CoopStatePkt carries a CoopLook: both hands'
  weapons, the muzzle flash bits (hands[h].field_87D), crouch, speeds, aim
  pitch/yaw (field_2A08/2A0C), the character and a ring of the last 4 shots
  with a sequence number.
- Puppet pose: a port of bondview2.c playerTick's remote MP Bond block
  (firing_animation_groups, chrlvUpdateAimendbackShoulders,
  act_bondmulti.unk2c = firingtable). Guns: chrGiveWeapon with
  getPropForHeldItem, removed with chrSetWeaponFlag4 and given back next tick.
  A dead puppet plays a Bond death anim, unarmed.
- Shots: chraiDefaultWeaponFireHandler calls coopLookShotBegin (miss at 3000
  along the dir), then Bg/Chr/Obj hooks (chrprop.c, chr.c, propobj.c) record
  what it hit; End pushes it. The watching game plays gun sound
  (sub_GAME_7F02BFE4), tracer (CapBeamLengthAndDecideIfRendered from the
  muzzle), and by kind: bullet hole + sound + spark, flesh sound + puff (not
  when the hit is our own Bond), object/glass. Nothing is damaged. The bullet
  loop skips HIT_GUN on a partner's gun (coopPropIsPartner).
- Characters: 0 "Mission Bond", 1+k = mp_chr_setup[k]. Lobby rows under each
  name (CLROW_CHARACTER for ours; A/Z forward, L/R step), "Your Character"
  on the idle screen. Saved in coop_character.txt; hello and roster carry it;
  puppets rebuild when body/head change.
- Name tags (coop_hud.c): 0.55 scale at 3 m to 0.32 at 30 m via the new
  gfx_pc G_NOOP tag 0x5A (scale for the next 0x58 bracket); hidden under
  1.5 m, fade in by 3 m, fade to alpha 0x60 between 40 and 70 m.
- Bug found on the way: fabsf with no prototype under -ffreestanding -w is
  an int call, so each call left a value on the x87 stack; after 8, tanf in
  lvlRender gave NaN, fovy went NaN and the whole view drew in fog colour.
  Any float libm call in game code needs a prototype (the build hides the
  warning; compile with -Wimplicit-function-declaration to check).
- Harness: GE_COOP_CHAR=<mp index> (not saved), GE_COOP_TESTFIRE=<item>:<s>[:<len>]
  (draws and holds fire, pulsed), GE_COOP_TESTLOOK=<s> (step 4 m from the
  partner and face them), party_lobby.sh BURST=<t>:<n>. Mission ticks run
  slower than wall time under Xvfb, so give the shots time. Verified: host
  sees Natalya with PP7 then KF7, muzzle flash and wall sparks from her
  shots; 3 and 4 player lobbies list each character.

## Pings (2026-10-24)

- src/game/coop_ping.c. Key `ping` (default Mouse Middle, unbound on pads;
  PORTACT_PING and pad map +31, appended; bind row "Ping"). The crosshair's
  line (bullet_path_from_screen_center, right hand) is traced like a shot:
  stan tiles for where the floor runs out, then the standing room and every
  drawn room with bgTestBulletHitBackground, nearest wins; then on-screen
  living chrs (not puppets) as 35-wide, 190-tall cylinders from chr->ground,
  if nearer. Nothing is damaged. 20-tick cooldown; 200 m range.
- COOPEV_PING = 16 (12..15 are the host-leaving work's, 20+ the next
  feature batch's, 32+ versus) to everyone: pos, room, chrnum, stage, kind
  (spot / enemy). One ping per player, 10 s, the last 2 s fading. An enemy
  ping follows chrFindByLiteralId(chrnum) (mirrored guards keep chrnums) and
  ends a second after the guard dies. A ping from another stage is dropped.
- HUD (coop_hud.c coopHudPing): diamond in the pinger's colour (red edge for
  an enemy), a closing ring for the first half second, "NAME 12M" or
  "ENEMY 12M" at 0.6 scale; off screen or behind, clamped to the view's
  edge toward it. Radar: hollow square, blinking for the first second.
  Receivers get a BEEP_SFX and "<name> pinged a spot." / "spotted an enemy.".
- Bug found on the way: a channel's numbering only started at 1 after
  coopSessionReset, which GE_COOP / -coophost / -coopjoin (the .bat
  launchers) never called, so the first event on each channel sat at seq 0
  and was never sent (a first door or pickup could be lost). coopNetInitFirst
  now resets before the first coopNetInit.
- Harness: GE_COOP_TESTPING=<s>[:<pitch>] presses ping <s> seconds into the
  mission, looking <pitch> degrees (default -20, negative is down) for the
  second before; GE_COOP_TESTPINGCHR=<chrnum> (-1 the nearest living guard)
  aims at that guard instead, GE_COOP_TESTPINGSTEP=<units> first moves that
  far from it. Verified on Dam with the mock: joiner's floor ping 4 m ahead
  on both games; host's enemy ping on guard 6 (TESTPINGCHR=-1 STEP=500)
  shown as ENEMY on both, the joiner's through the rock; 3 copies, player
  3's ping relayed by the host to 1 and 2, on player 2's screen held to the
  bottom edge (it lay at their feet).

## Host leaving and migration (2026-10-24, COOP_VERSION 6)

- Ids: COOPPKT_AISYNC 8, COOPPKT_BYE 9; COOPEV_PLAYERNOTE 12, COOPEV_ENDALL 13
  (14, 15 kept for this); COOPSET_HOSTLEAVES (was SPARE7). Other threads
  claimed COOPEV 16 (ping), 20..31 and COOPPKT 10..11 (feature batch),
  COOPSET_SPARE2..6, COOPEV 32+ and COOPPKT 16 (versus).
- Detection is wall clock (coopNowMs = osGetTime()/46875), so a paused or
  cutscene-locked game still counts: while g_ClockTimer <= 0, coopKeepAlive
  sends a hello every 500 ms. A joiner that hears nothing from the host for
  3 s shows a banner, waits 12 s more, then migrates. A COOPPKT_BYE (3
  copies, sent from atexit, Leave Lobby and the watch Abort) or Steam
  reporting the owner left the lobby migrates at once.
- Election: lowest player number left in the old host's roster. Everyone
  renumbers in roster order (new host = player 0) with coopRenumber, which
  remaps sR, puppet chrnums (30000+p), roster, channels and coopStatsRemap.
  The new host reserves transport slots from the roster ids (UDP:
  addr<<16|port, Steam: SteamID) via coopNetMigrate. A follower with no
  hello from the new host in 8 s elects again without that player.
- World: the host sends COOPPKT_AISYNC every 60 ticks (ailist id, aioffset,
  action, damage and a few AI fields per chr). coopTakeOverWorld rebuilds AI
  from it on the new host: dead chrs ACT_DEAD, live ones ACT_INIT so their
  scripts restart from the synced offset. Doors, objects and objectives were
  already mirrored on joiners, so they carry over.
- Steam: optional SetLobbyOwner; lobby data "gehost" names the host so a
  rejoiner finds the right player even before ownership moves. The mock
  stores owner and gehost in /tmp/gecoop-mock, and CloseSessionWithUser now
  drops the peer like real Steam (needed for rejoin).
- Joiner drop, on the host: 3 s silence = lost (Bond idle, coopRemoteAliveHere
  false so guards and scripts ignore it), 30 s = slot freed. The host
  broadcasts COOPEV_PLAYERNOTE (LOST/BACK/GONE/LEFT/LEFTMISSION). A joiner
  whose hello is on another stage left the mission.
- Watch Abort as host with partners: first confirm turns the row into
  "HOST: END ALL / HAND OVER". Hand Over sends a fresh AI sync and BYE
  HANDOFF; End All sends reliable COOPEV_ENDALL and everyone goes to the
  lobby (coopEndMissionLocal: mission state 0, title stage, no save reset).
- Harness: coop/docker/host_drop.sh (PLAYERS, KILL, KILLAT, REJOIN, SHOTS,
  RUNSECS, ALLENV/HOSTENV/P<i>ENV); GE_COOP_HOSTLEAVES=0|1,
  GE_COOP_TESTABORT=<s>:handover|endall, GE_COOP_TESTABORTUI=1 (shows the
  host prompt on the watch). Verified with the Steam mock: host killed with
  3 and 2 players (mission goes on, guards keep fighting, dead guards stay
  dead), old host rejoins as a late joiner, joiner killed (lost, then
  removed at 30 s), Hand Over, End All, and Back to Lobby.

## Feature batch: chat, objective credit, team times, cross-save, spectate, public games, lobby controls, photo mode (2026-10-24, COOP_VERSION 7)

- Ids: COOPEV 20..31 are coop_ext.c's (CHAT 20, OBJDONE 22, TEAMTIME 23,
  CAMPAIGN 24; 21 and 25..31 free), COOPPKT 10..11 (TEAMTIME 10). coop.c
  hands those to coopExtApply / coopExtPacket and calls coopExtTick in
  missions (coopTick) and menus (coopFrontTick). No COOPSET. Binds appended
  after ping: chat (+32, T), photo_mode (+33, P).
- Chat (coop_chat.c): the port text input (portTextInputStart); a line of up
  to 63 characters goes as up to 3 events of 24. Feed bottom left, 6 lines,
  10 s, player colours; toasts (coopFeedToast) stack above the chat lines,
  newest lowest, 5 s with a short fade in and out: the lead in the player's
  colour, '|' then the rest in white on the same line (second line when it
  would pass half the screen). Partner news (joined, left, lost, gives,
  objectives) and session news (coopBanner: host left, you are the host;
  red) are toasts; the host-not-answering countdown sits under them while
  it lasts. The game's own mission text and our own action feedback ("No
  spare ammo", "Revived!") stay on the game's message line. Harness:
  GE_COOP_TESTTOASTS=<s> pushes one of each kind every 1.5 s. In the lobby the latest 3 lines and a "Type a Message" row.
  Escape closes the line without opening the watch (coopChatEatEscape).
  Harness: GE_COOP_TESTCHAT=<s>:<text> (mission), GE_COOP_TESTLOBBYCHAT.
- Objective credit (coop_objective.c): the host notes who changed the world
  (coopObjNoteActor from WORLD bits, pickups, shots, damage, explosions; our
  own shots in coop_look.c; chrai.c's "either player" checks through
  coopAltBondCredit). When an objective turns complete on the host the credit
  is whoever acted in the last 3 ticks, else the last damage in 5 s, else the
  host. Our own Bond notes itself (coopObjNoteSelf: camera photo, room
  entered, thrown item deposited, pickup, key copy). A level script's
  "either player" check no longer credits on its own (those run every tick
  while a partner stands in a room, which credited the friend for the host's
  photo and elevator): it records who passed it per script, and only when
  that script then sets a new stage flag does that player get noted; COOPEV_OBJDONE to all, toast unless it is ours. Harness:
  GE_COOP_TESTOBJDONE=<s> (Bunker has room criteria; Dam has none).
- Team time (coop_teamtime.c): the host sends its mission timer each second
  (COOPPKT_TEAMTIME) and the exact time before MISSIONEND (COOPEV_TEAMTIME);
  a joiner runs the last one on with its own clock. end_of_mission_briefing
  and the debrief use coopTeamTimer, so best times and target-time cheats go
  by the team's time. A joiner that ended the mission itself corrects its
  save when the exact time comes (coopTeamTimeCorrect).
- Cross-save (coop_stats.c): the host sends its furthest campaign line once
  per joiner (COOPEV_CAMPAIGN); the joiner writes it under the host's key in
  its coop_campaign.txt and shows "Next: <mission>" in the lobby.
  two_player_split.sh runs the joiner from build_p2 so each has its own files.
- Spectate (coop_spectate.c): once coopDeadTick owns the death (after the
  fade to black), the view fades back in (light red wash) through a standing
  partner's eyes. bondviewRenderDebugBondView takes the camera from
  coopSpectateCamera, and bondviewGetCurrentPlayersRoom / Position /
  Position3 the partner's room and position (portal visibility, fog). The
  watched puppet is hidden (CHRFLAG_HIDDEN, as when both stand on one pad)
  and so is its name tag. Fire (Z) cycles partners. coopSpectateStop on
  respawn, revive and mission failure. Verified with the host 40 m away and
  turned: the joiner's view matched the host's screen.
- Public games and password (coop_browse.c, port/src/coop_steam.c): lobby
  type friends-only or public (SetLobbyType, live), lobby data gever (the
  COOP_VERSION, the list filters on it), host (persona name), pw (0/1).
  RequestLobbyList with string filters gecoop=1 and gever, worldwide, every
  5 s while the lobby screen is idle. The password never enters the game's
  packets: a stranger's game sends GEPW + FNV-1a(password, lobby id) to the
  host on the transport (8-byte message, intercepted in coopSteamRecvFrom);
  the host's onSessionRequest accepts the session but only setPeer()s a
  stranger after a matching GEPW (GEPA back, or GEPR, after which the joiner
  leaves and the lobby screen says "Wrong password."). Steam friends
  (GetFriendRelationship == 3) skip it. After a host migration the new host
  keeps the lobby's type but has no password. The mock stores lobby data in
  <lobby>.data and lists public lobbies. Harness: two_player_public.sh
  (HOSTPW, JOINPW; copies that are not friends; GE_COOP_LOBBY=browse).
- Lobby controls (front.c coopLobbyAddControls): "Your Controls": controller
  layout preset of device 1 (portInputLayoutApply), hfov_wide, mouse,
  mouse_sens_x (and _y with it when they were equal) via portOptStep.
- Photo mode (coop_photo.c): P toggles, solo or co-op, when Bond could move.
  Locks controls (lvlSetControlsLockedFlag) and takes the look delta in
  bondview2.c before the lock drops it; the C buttons / stick fly the camera
  along the view. xz moves only where walkTilesBetweenPoints_NoCallback
  reaches from the camera's tile; y 15..400 over that floor; 800 units from
  Bond's eyes. Camera and room go through the same hooks as spectate;
  maybe_mp_interface draws only coopPhotoRender (a hint, hidden for the 8
  ticks around a shot). The shot is gfx_sdl2.cpp's GE_SHOT writer with
  ge_shot_path set: Photos/GE-<stage>-<date>-<time>-<n>.bmp beside the exe.
  The co-op debrief says how many. Harness: GE_PHOTO_TEST=<s>. A replay was
  ruled out: nothing records the world, and recording every guard, door
  and shot for playback is much heavier than photo mode.
## Online versus (2026-10-24, COOP_VERSION 8)

- The lobby's Rules section starts with Mode: Co-op / Versus (host only;
  COOPSET_VSMODE bit 0). Versus shows Arena, Scenario, Game Length and Weapons
  (COOPSET_VSARENA..VSWEAPONS) instead of When Down, Guards and Friendly Fire,
  and the host's Continue / Select Mission become Start Match. Scenarios that
  don't suit the player count fall back to Normal, as Rare's menu does. A
  Random arena is resolved by the host before the start.
- Start: coopVsHostStart builds the mask of present players (kept in
  VSMODE bits 4-7 so it rides in HELLO with the other settings), calls
  coopVersusRoster(mask, serial) and coopVsStartMatch(stage). Joiners follow
  from coopFrontTick when the host's HELLO shows a versus mission they are
  in; no late joining (over 10 s into the match, or not in the mask).
- coopVsStartMatch (front.c) sets up Rare's own match: GAMEMODE_MULTI,
  selected_num_players, reset_mp_options_for_scenario, the length (through
  portMpLimFromLength; the PC's own Game Limits do not apply), the weapon set
  with portMpWeaponSeed so a Random set rolls the same everywhere, player_char
  from each player's lobby character, handicap normal, and Rare's team sides
  by session number for the team scenarios.
- Each PC is local player 0 (keyboard, mouse, pad 1 stay where they are).
  sVsNet[li] / sVsLocal[p] map local index and session player, in the same
  circle order everywhere. getViewPlayerCount() (player.h) is 1 in versus,
  so viewport, z-buffer, HUD, pillarbox and FOV act as one-player; lv.c's
  render loop skips the others (coopVsRemote).
- Others' bodies: their VIEWER chrs, moved each tick from their STATE
  (coopVsMoveBody). The MP body code (playerTick) puts a remote body back at
  field_488.collision_position / current_tile_ptr each tick, so those are set
  too; without them a body stayed where its player first spawned. A jump of
  over 3 m (a respawn) looks straight down for its tile instead of walking
  the old floor. player->cameramode must stay 0 (normal play) for the body
  to draw. Pose, guns and shots are coop_look.c's, as for co-op puppets.
- coopTick runs with player 0 current in versus (the MP loop leaves the last
  player current), and coopInMission accepts versus (MP never sets
  g_CameraMode).
- Damage, kills, items, blasts and the end: see coop_versus.c's header.
  COOPEV 32..36 (VSHIT, VSKILL, VSEND, VSITEM, VSBOOM) and COOPPKT 16
  (VERSUS: flag hold time / golden gun holder, 5 a second, relayed by the
  host). The first start pad is (serial*5 + p*stride) % count on every game
  (bondviewGetRandomSpawnPadIndex asks coopVersusStartPad first; reset in
  coopVersusRoster because the stage's spawns run before coopStageStart).
- MP pause is off in versus; remote players count as done at the end screen;
  after the match everyone returns to the lobby (menu_init's RUN_STAGE case
  runs once as the stage starts, so it only queues MENU_COOP_LOBBY; the
  lobby's init ends the match).
- The name at the bottom right is the Steam name.
- Shared since COOP_VERSION 14 (see "Versus gameplay" below): the arena's
  weapon, ammo and armour pickups and their respawn timers, guns a victim
  drops, thrown grenades and mines, object damage. Not shared: the per-PC
  Multiplayer Options in midas.ini. A player who drops out leaves a still
  body.
- Harness: GE_COOP_VERSUS=<arena>:<scenario>:<length>:<weapons> (host's
  rows), GE_VS_HUNT=<s> (step 4 m from the next player with a clear walk,
  face them; tries another side every 5 s without a hit; tops up the
  GE_COOP_TESTFIRE gun), GE_VS_GOTO=<s>, GE_VS_GRAB=<s> (walk onto the flag
  or golden gun), GE_VS_RESPAWN=1 (Start to respawn and to leave the end
  screen), GE_VS_KILLS=<n> (kill limit). coop/docker/versus.sh wraps
  party_lobby.sh with these.
- Verified on the mock Steam: 2 players First to 5 on Temple to the end
  screen (5-0, ranks 1st/2nd on both) and back to the lobby; 4 players with
  the same kill table on all four, two matches in a row; 3 players Licence
  to Kill (one hit kills); Golden Gun (pickup leaves the other world, one
  shot kills); Flag Tag (pickup, drop at the holder's body on death); You
  Only Live Twice (two deaths end it).

## Rich presence, invites from anywhere, Discord (2026-10-25)

- What was there: friends-only lobby, in-lobby friend invites and the Steam
  invite window, Join on a hosting friend, `+connect_lobby` as GE_COOP, a
  fixed "Hosting/Playing GoldenEye co-op" status. Invites (333/337) only
  worked with the lobby screen's Steam up and not while hosting.
- Steam now comes up at boot (coopSteamPresenceOpen, once; the lobby screen
  still retries). coopSteamPresenceTick pumps callbacks when no session is
  reading the transport. GE_NO_PRESENCE turns presence and Discord off.
- port/src/coop_presence.c: one waiting join (lobby, password hash, who
  from) fed by 333 / 337, `+connect_lobby` (main.c, no longer GE_COOP) and
  Discord's ACTIVITY_JOIN. src/game/coop_presence.c takes it from the menus
  (after 120 ticks, not on MENU_RUN_STAGE): coopLobbyLeave if in a lobby,
  coopLobbyJoinLobbyId, MENU_COOP_LOBBY. In a stage it waits and toasts
  once. coopFeedClock runs the feed's clock when co-op is off.
- Presence text per frame (coopPresenceGameTick from menu_init and coopTick):
  menus / lobby screen / lobby (co-op or versus, n/4, "Picking a mission"
  off the screen) / co-op, solo, versus mission. Player count changes count
  after 2 s (a joiner's count of the others flickers with the host's hellos).
  Joinable: in a Steam lobby with room, not in a versus match.
- Steam keys: status (our text), steam_display + gamestatus (480: Spacewar's
  #StatusWithoutScore with AtMainMenu / WaitingForMatch, nothing in a
  mission; own AppID: #Status from coop/steam/rich_presence_english.vdf, which
  is "%status%"), steam_player_group(_size) = lobby id / count, connect =
  "+connect_lobby <id>" while joinable (every member, not only the host).
  Values are cached and only changes are sent; coopSteamShutdown clears.
  The AppID is read back from steam_appid.txt (sAppId) for the hosting-friend
  check. Under 480 Steam's Join Game on a friend whose game isn't running
  would start Spacewar, so the README says to start the game first.
- Discord (port/src/coop_discord.c): the RPC socket directly (named pipe
  discord-ipc-N / Unix socket under XDG_RUNTIME_DIR, TMPDIR, TMP, /tmp plus
  the flatpak/snap subfolders), non-blocking, retry every 15 s. App id from
  GE_DISCORD_APPID or discord_appid.txt; none = off. SET_ACTIVITY at most
  every 4 s (Discord allows 5 per 20 s), details/state, start timestamp
  (reset when the first line changes), large_image "logo" (an art asset to
  upload in the Discord app), party ge-<lobby> n/4, secrets.join
  "ge1:<lobby>:<hash hex>" where hash is coop_steam.c pwHash(password, lobby)
  (0 without a password; a joiner in a passworded lobby offers no join).
  The joiner sends that hash as its GEPW (coopSteamSetJoinPasswordHash).
  SUBSCRIBE ACTIVITY_JOIN / ACTIVITY_JOIN_REQUEST; requests show in the
  host's lobby (CLROW_DISCORDYES/NO, up to 2 shown) and answer with
  SEND_ACTIVITY_JOIN_INVITE / CLOSE_ACTIVITY_REQUEST; dropped after 30 s
  or declined at once when there is nothing to join. Windows registers the
  discord-<appid> URL protocol under HKCU so Discord can launch the exe.
- Friends only: Steam keeps a non-friend out of a friends-only lobby (the
  mock now does too: owner's friends and invitees only, response 2). A
  direct join that Steam refuses (sJoinDirect) no longer falls back to
  looking for any hosting friend: the lobby screen says "Couldn't join that
  game. It's full, or friends only." (coopSteamJoinRefused 2).
- Mock Discord: coop/mock/discord_mock.py <dir> (READY, replies, logs every
  frame to <dir>/discord.log; files "join" and "ask" inject ACTIVITY_JOIN /
  ACTIVITY_JOIN_REQUEST). Harness: GE_DISCORD_AUTOACCEPT=<s>.
- Test: coop/docker/presence.sh. Verified with the mock Steam and mock
  Discord, three copies: p3 (Steam friend) at the menus pulled into the lobby
  by the host's invite; p2 (not a Steam friend) at the mission select joins
  through Discord Ask to Join into a public lobby with a password, past it
  with the hash; all three follow into Dam; statuses on every copy
  ("In the co-op lobby (3/4)", "Co-op: Dam, Agent (3/4)"), connect cleared
  in a versus match ("Versus: Temple (2/4)"). FRIENDSONLY=1: p2 kept out
  with the message. P3ENV=GE_STAGE=dam P1DELAY=25: the invite toast in a
  solo mission. two_player_lobby.sh invite/wait and versus.sh still pass.
- Not tested: real Steam's friends list display and real Discord (needs a
  Discord application id and the two accounts).

## Lobby scrolling (2026-10-25)

- The lobby showed 19 rows from y 53, past the bottom of the folder's 4:3
  picture: at real resolutions everything below When Down was cut off, and
  the cursor-edge scroll and the "more" mark sat on rows nobody could see.
  COOP_LOBBY_VISIBLE is now the sheets' 13 (PORT_SHEET_PAGE_ROWS, down to
  Rare's last line, 293); "more" marks sit beside the title and under the
  last row, so no row gives up its slot.
- coopLobbyScrollTick: the wheel scrolls the view a row per notch (hover
  still picks, the watch's rule); the D-pad, a pad's digital Move and the
  arrow keys (watch_up / watch_down) walk the actionable rows and scroll to
  keep the picked one on the paper with a row to spare; past the last
  actionable row they scroll on through text rows (the joiner's rules). C
  buttons are left out: on the keyboard they are WASD, which already moves
  the crosshair. Resting the crosshair above or below the rows still scrolls.
- Scale: the lobby is a folder screen like the rest of the frontend, pinned to
  a centred 4:3 on any window (video.c, non-wide frames). hud_scale and the
  wide HUD options only touch the in-game HUD, so they don't change the
  lobby, which matches the other sheets' size.
- Verified at 1280x720 with the mock Steam (host + joiner): arrow keys from
  the title to Steam Invite Window and back, wheel both ways, joiner's arrow
  keys to its last row. Shots: screens/lobby-scroll/ in the project files.

## Auto-aim ignores partners (2026-10-25)

- Real Steam test: auto-aim locked onto the partner's Bond instead of guards.
  chrpropUpdateAutoaimTarget (chrprop.c) is the one place auto-aim picks a
  target (autoaim_target_x/y, which the gun swivel and aim assist follow);
  it took any armed PROP_TYPE_CHR, and co-op puppets are armed chrs. It now
  skips coopPropIsPartner. Shots still hit partners (Friendly Fire decides
  the damage); versus opponents are VIEWER props, not puppets, so versus
  auto-aim is unchanged.
- Checked, no change needed: the crosshair colour is the fixed midas.ini
  crosshair_color (no target highlight), and the ping's chr pick
  (coop_ping.c) already skips partners, so a partner is never an ENEMY ping.
- Harness: GE_COOP_TESTLOOK now logs "autoaim x <none|partner|chr> ..." each
  second while facing the partner (mouse_auto_aim=1 in midas.ini for Xvfb
  runs, since the mouse blocks auto-aim by default). Verified on Dam with the
  mock Steam: without the fix "partner 30001" every second, with it "none".

## Shared items, thrown objects, knocks, blasts and Use (2026-10-25, COOP_VERSION 9)

From a real Steam test on Dam, Runway and Surface: the joiner never saw the
host's covert modem or gun drops, the shot-off padlock stayed on for the
host, grenade-launcher blasts were invisible to the others, the joiner's
satellite-console work and the Runway plane did nothing on the host.

- **Thrown and placed items** (src/game/coop_items.c). gunInitProjectileFromPlayer
  calls coopItemsThrown for grenades, the three mines, the covert modem
  (ITEM_BUG), the camera, the bomb case, plastique and the GoldenEye key.
  COOPEV_THROW (25) carries item, start pos and velocity; COOPEV_THROWMORE
  (26) the rotation, spin and fuse. Every other game spawns its own copy
  with gunCoopSpawnThrown (gun.c), owned by the thrower's puppet, so it
  flies, sticks and settles the same way. Ids are (player & 3) << 14 | serial.
  A copy that goes off (propExplode, wrapped in coopItemsExplodeBegin/End)
  sends COOPEV_ITEMGONE (27) BOOM and the others set their copy's fuse to 0;
  picking one up sends ITEMGONE PICKED. A partner's remote mines are not
  set off by our detonator (chrobjWeaponTick, coopItemsIsPartners).
- **Guard gun drops**: the host sends COOPEV_DROP (28) when a dying guard's
  weapon prop is detached (chr.c CHRHIDDEN_DROP_HELD_ITEMS); joiners skip
  their own drops for mirrored guards and drop the mirrored chr's matching
  child weapon instead (or spawn one at the given pos). Picked up once, by
  whoever gets there first (ITEMGONE PICKED, then everyone has it).
- **Shared pickups**: a setup weapon, ammo, key or mission item one player
  takes is taken by everyone (COOPEV_PICKUP to all, kept in sTeamPick[96]
  and retried every 15 ticks while a player is dead or not in the mission;
  the host resends the list to a late joiner when its puppet spawns).
  Armour is just removed for the others. Each player gets the item into
  their own inventory, so the joiner has the modem, the key, etc.
- **Knocks**: objHit drops what sits on the hit object and bounces it only
  in the shooter's game. COOPEV_OBJKNOCK (17) sends it in world space
  (objBounce wants view space; the receiver uses its own view rotation
  transposed), so a shot-off padlock falls everywhere.
- **Blasts**: coopExplosionCreated. A joiner's own source-less blast
  (grenade, mine, rocket, GL round) is sent to the host as before; the host
  now also sends every source-less blast (its own, the guards', and the
  relayed ones, minus the originator) to each joiner. A joiner plays it
  under sApplyingHostBoom: the effect plus damage to its own Bond only
  (guard and object damage stays the host's). Object explosions keep
  going through OBJDMG. Blasts hurt every player as in solo, Friendly Fire
  or not (FF only covers bullets).
- **Use** (propobjInteract): a joiner's press on a console, switch, alarm
  or the Runway plane sets RUNTIMEBITFLAG_ACTIVATED only in its own game,
  and the scripts that wait for it (if_object_was_activated) run on the
  host. COOPEV_OBJUSE (18) sends it to the host, which marks it activated,
  flips an alarm switch (the door-switch link is not run again: the joiner's press already ran it and sent its DOOR events).
- **Endings**: the presentation commands the host's script relays to the
  joiners (coopAiCommand) now include AI_MoveObject and Hide/ShowAllChrs.
  The Runway ending moves the plane to its take-off pad from a global list
  that only the host runs; the plane's own list (ai_12) runs everywhere and
  takes off on stage flag 0x00100000, which the WORLD packet brings. Before,
  the joiner's camera cut to an empty runway.
- **Name tags**: coopAsciiName folds UTF-8 to the game font (Latin-1 and
  Latin Extended-A to the base letter, others dropped, "?" if nothing is
  left), used by the name tag, the feed and the lobby list. The partner
  status block moves down a line when the FPS counter is on.
- Harness: GE_COOP_LOBBY_DIFF=<0..3> sets the lobby harness difficulty;
  GE_COOP_TESTAIM=<s>:<yaw>:<pitch> turns the view that many seconds in;
  GE_COOP_TESTOBJ=<model> (2 or more) shoots only that model, at any
  distance; GE_COOP_TESTUSE=<grabtag>:<usetag>:<s> picks up the object
  tagged <grabtag> (-1 none) <s> seconds in and presses Use on <usetag> 5 s
  later.
- Verified with two copies over the mock: the host's covert modem sticks to
  the same rock on both screens; Dam on Secret Agent and 00 Agent gives
  both players the modem; host grenades hurt the joiner and the joiner's
  timed mine hurts the host (each game's own copy); on Runway the joiner
  takes the key and boards the plane, the host's script runs the ending
  and both games finish the mission, with the plane taking off on both
  screens; on Surface the joiner's Use on the dish console runs the host's
  script, and the joiner shooting it reaches the host as OBJDMG.

## Mission card, reconnecting panel, error panel (2026-10-25)

- src/game/coop_screens.c, drawn from lv.c lvlRender (after the views) and
  the lobby's constructor.
- Mission card: a co-op mission holds still on every game until everyone who
  was in the session has loaded in. coop.c coopHoldStart (from
  coopStageStart): the host waits for each present joiner's hello in this
  stage (after the hold began), 30 s at most, dropping anyone who leaves;
  joiners wait for the host's word, 35 s at most, or until the host goes
  quiet. lv.c lvlTick treats coopHoldWorld() as a pause (g_ClockTimer 0), and
  coopTickBody runs coopHoldTick instead of coopKeepAlive: poll, session tick,
  events, a hello every 15 ticks. The host's hello carries it in the old pad
  byte after youare (CoopHelloPkt.hold: bit 0 holding, bits 4-7 who is in),
  so the wire size and COOP_VERSION are unchanged; an older build reads 0 and
  never holds. Late joiners and versus never hold. GE_COOP_NOHOLD turns it off.
- The card: black over the whole frame, CO-OP MISSION, the mission name at
  2x, the difficulty, the objectives for it (one line each), each player with
  their colour as IN or LOADING..., a bar of who is in and the host's
  countdown. It fades over 20 frames when everyone is in.
- Reconnecting: while coopHudSessionBanner has something (host silent, or a
  new host being reached), the view dims under a panel with its two lines, a
  sweep of blocks and what happens if the host stays gone (by the Host Leaves
  rule). It replaces the toast that used to sit under the chat
  (coop_chat.c). Hidden while the watch is up.
- Error panel (coopScreenError(title, text up to three lines)): the lobby
  shows the newest over its paper until A / Start / B / Z dismisses it (the
  rows don't take input meanwhile; not in the first 20 ticks, so a held
  press from the mission doesn't close it). Raised by: the host ended the
  mission for everyone, the host left the mission, the Host Leaves rule
  sent everyone back, a wrong password, Steam keeping us out of a lobby, and
  a peer on another COOP_VERSION (its packets used to be dropped silently,
  which left a joiner on "Connecting to the host..." for good).
  GE_COOP_ERRSHOT=<ticks> dismisses it by itself (harness).
- Verified with the mock Steam: two_player_lobby.sh (host held 3.8 s for the
  joiner, the joiner 0.6 s for the host's go); host_drop.sh PLAYERS=3 KILL=1
  (panel, then migration); PLAYERS=2 with GE_COOP_HOSTLEAVES=1 (panel with
  the countdown, then the lobby with the error panel). Shots in the project
  files under goldeneye-coop/screens/loading/.

## Partners' audio (2026-10-25)

- src/game/coop_audio.c. Original sounds only, at the game's sound and music
  volumes, through the guards' positional path (chrobjSndCreatePostEvent:
  distance volume and the port's stereo pan, re-panned as we turn).
- Already positional before this: a partner's gunfire (coop_look.c plays it
  through the guards' gun sound at the puppet), thrown items
  (gunCoopSpawnThrown), impacts and blasts.
- Gun handling: gunfire.c calls coopAudioCue where our Bond's draw sound
  (SWITCH_RAISE: pickup clack, or the knife's, mine's or laser's) and reload
  sound (RELOAD_SWAP: rifle cock) play. The cue rides every STATE in
  CoopLook.cue (formerly the pad byte: bits 0-4 a counter, 5-6 the kind), so
  the packet keeps its size and an older build sends 0, which plays nothing.
  Puppets play a new cue at their position (coopAudioPuppetCue). A downed
  Bond sends none: its hands cycle through draws while it is down.
- Ping: the BEEP now comes from the ping's direction, full volume to 60 m,
  fading to 300 m but never under a third. Our own ping still beeps centred.
- Stings: coopAudioTick watches CoopHudPartner.dead per partner (a dropped
  connection doesn't count). Down: the game's short death sting
  (M_SHORT_SOLO_DEATH) on the X music track (track 2, what the alarm and exit
  music use), only when that track is free and we are not dead ourselves,
  faded out after 3 s; the level's music on track 1 keeps playing. Back up
  (revived or respawned): OBJ_REGEN_SFX at half volume. coopAudioPartnerDowned
  and coopAudioPartnerRevived are public for the revive work to call
  directly if it ever stops using .dead.
- No footsteps: GoldenEye has no footstep sounds at all (neither Bond nor
  the guards make any), so there was nothing original to play.
- Verified with the mock Steam and SDL's disk audio driver on the host
  (SDL_AUDIODRIVER=disk, music at 0 to hear the effects alone): the
  partner's draw and reload logged and heard; with the host turned 90
  degrees (GE_COOP_TESTAIM=3:90:0) the joiner's ping 18 m away came out
  L 861 / R 223 (RMS), against L 810 / R 820 for a sound at our own spot.
  GE_COOP_TESTDIE with the timer rule: the downed sting on the X track,
  then the revived sting at the respawn; no draw sounds while down.

## Profiles carry your settings; button prompts (2026-10-25)

- **Per-profile settings** (`port/src/port_profiles.c`, profile version 5). The
  profile in player 1's seat now also holds that person's Port Options in a
  `[settings]` section (field of view, the HUD rows, mouse and stick feel,
  rumble, volumes; the list is `sProfSettings`) and their co-op lobby
  character (`coop_character`). Loading it into seat 1 applies them;
  midas.ini is written once after the lot. Display hardware rows, the pad
  count, world rules (Unlock All, the shoot-through fix) and the MP sheet's
  rows stay the machine's. Other seats' profiles are unchanged. Older
  profiles load as before; a v5 file in an older build ignores the new keys.
- **Profile row in the co-op lobby** (Your Controls): steps player 1 through
  the saved profiles and None.
- **Button prompts** (`src/game/menu_glyphs.c`). A bar on the felt above the
  folder paper names Select and Back (and Change, Chat, Continue or OK where
  they apply) with the buttons of whatever was pressed last: keycaps for the
  keyboard and mouse, Xbox's coloured A and B, PlayStation's cross and
  circle, a Switch pad's A and B, or the N64's blue A and green B for a pad in
  N64 mode (`input_pc.c portInputGlyphStyle`). The co-op error panel's OK
  shows the same button. `GE_GLYPHS=0..4` pins a style, `GE_NOHINTS` hides
  the bar (harness).

## Lobby crew strip (2026-10-25)

The co-op lobby opens with a strip under its title (`front.c
coopLobbyStrip`): a polaroid card per player clipped to the folder, with
their colour along the top, their character's photo from the multiplayer
screen (Mission Bond shows Rare's Bond; the generic guards show Rare's own
question-mark photo), the name, and HOST / READY / NOT READY under it. A free
seat is an OPEN card; our own card has a dark edge. On the right, the next
mission's photo with its difficulty (the campaign's Continue / Next), or in
versus the arena's photo with the scenario. Missions without a multiplayer
photo (Surface, Silo, Streets, Train, Jungle, Control) show the briefing
monitors' world map. Before hosting it is just our own card, named after
player 1's profile when there is one.

The player rows the strip replaces are gone; "Your Character" is one row
(L / R or A to change it). The rows below are a little tighter (18 apart, 9
on the paper) and scroll as before.

## Voice chat (2026-10-25, packet 30, COOP_VERSION stays 9)

- Steam Voice: coop_steam.c loads SteamAPI_ISteamUser_StartVoiceRecording /
  StopVoiceRecording / GetAvailableVoice / GetVoice / DecompressVoice as
  optional exports (the shipped steam_api.dll has all of them); without them
  the log says "voice chat is off" and the voice rows stay hidden.
- Transport: COOPPKT_VOICE = 30 (30..31 reserved for voice; the netcode
  thread has 20..23), unreliable, one compressed GetVoice chunk per packet
  (at most 1100 bytes; a bigger read is a stale backlog and is dropped). The
  host stamps the sender's player number and relays a joiner's packets to the
  other joiners. Older builds ignore the type, so no version bump.
- Playback (port/src/voice_pc.c): decoded at 22050 Hz into a 2 s ring per
  player, 100 ms prebuffer, skips ahead to 150 ms when more than 400 ms is
  queued, mixed into audio_pc.c's output before the mute-in-background step.
  Gains glide over 20 ms. Every 10 s of output it logs "player N's stream ran
  dry X times, Y ms skipped to catch up" when either is non-zero.
- Position (coop_voice.c coopVoiceGains): in a mission, the distance from the
  local view to the partner gives the gain through the guards' sound curve
  (sub_GAME_7F0537B8 with 5000/6000, as chrobjSndCreatePostEventDefault;
  0.86 at 4 m), never below 0.3 so a far partner is still heard, and
  portSndPanForPos the pan (solo viewport with Stereo Pan on; centred
  otherwise). Equal-power pan scaled so the centre is full volume. In menus
  and the lobby voice is centred at full volume. Logs "player N at Xm, pan P,
  gain L R" every 2 s per talking player.
- Options: [audio] voice_mode (0 push to talk, 1 open mic, 2 off),
  voice_volume (0..200 %); bind push_to_talk (default V, input_pc pad map
  +34). Mic is never open while the chat line, a password or other text
  input is being typed. Per-player mutes (lobby Voice row, watch Audio page
  "Mute <name>") last for the session, are not saved, and follow host
  migration; a muted player's packets are dropped before decoding.
- HUD (coop_hud.c): speaker icon left of a talking partner's name tag and
  after their name in the partner list (red bar when muted); "TALK" with the
  icon top centre while your mic is open. Lobby: "<name> - Talking".
- Mock Steam: no GE_MOCK_VOICE = no mic (GetVoice reports no data, as Steam
  does without a microphone). GE_MOCK_VOICE=<hz> makes a tone pulsing at 4 Hz
  (8-bit 8 kHz behind an "MV" tag; DecompressVoice resamples to the asked
  rate). Harness: GE_COOP_TESTVOICE=1 talks all the time, GE_COOP_TESTMUTE=<n>
  mutes player n (1-based) at session start. Record a copy's output with
  SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=<file> (s16 stereo 22050).
- Verified with the mock (2026-10-25): lobby, the host's 330 Hz tone reaches
  the joiner centred; Dam, 4 m apart gain 0.86, after the host turned 90°
  "pan 17, gain L 0.86 R 0.25" and the recording's 440 Hz went from L/R 2576
  to L 2577 R 767; mute, the muted player's tone is absent from the
  recording; no mic, nothing sent and nothing breaks; 3 players, each hears
  the other two through the host's relay in the lobby and in the mission.
  Screens in project files goldeneye-coop/screens/voice/.
- Known gaps: not tried on real Steam or a real microphone yet (the sample
  rate and chunk sizes are Steam's own). While the game is paused or in a
  cutscene coopTick does not poll the network, so voice waits until play
  resumes. Split-screen co-op hears voice centred.

## Crash and bug reports (2026-10-25)

- port/src/port_report.c. A crash (SEH filter on Windows, the signal handler
  on Linux) or the stall watchdog's exit writes Data/Crashes/crash-<date>-<time>/:
  info.txt (build string, commit, branch, OS, menu, stage), log.txt (the last
  64 KB of portConLogf lines: port_console.c taps every line into
  portReportTap, since the console ring is only 128 rows), coop.txt
  (coopDebugDump in coop.c: role, player, stage, ticks, host hello, migration
  state, settings, each partner's last STATE, each channel's seq/ack and
  silence), crash.log, midas.ini, and on Windows minidump.dmp
  (MiniDumpWithIndirectlyReferencedMemory | ScanMemory, about 40 KB; .bss is
  23 MB so no data segments) and the last 512 KB of ge_direct.log. Linux adds
  backtrace.txt. The newest 10 folders are kept.
- Windows asks at once (MessageBox, game window minimised first): Yes zips
  the folder to the Desktop (SHGetFolderPath CSIDL_DESKTOPDIRECTORY, so
  OneDrive desktops work), puts the zip on the clipboard as CF_HDROP (Ctrl+V
  in Discord or Explorer attaches the file) and opens Explorer on it.
  Anything unanswered (No, Linux, a stall, GE_NOMSGBOX) leaves
  Data/Crashes/pending.txt, and the next start asks before the window opens
  (Save to Desktop / Don't send / Ask me later).
- The zip is stored, not deflated: the writer allocates nothing and uses
  fixed buffers, since it runs in a dying process.
- F8 (a fixed key in input_pc.c, like ~; also `bugreport` in the console):
  the same files minus the dump, plus screenshot.bmp (gfx_sdl2.cpp's forced
  shot, halved until 1280 wide or less, about 2.7 MB), under
  Data/Reports/bug-<date>-<time>/, zipped to the Desktop and copied
  (Linux: the path as text). The console's flash rows say where.
  portReportTick runs once a frame beside portConRender (boss.c).
- Harness: GE_BUGTEST=<frame>, GE_CRASHTEST=<frame> (writes to address 16),
  GE_REPORT_AUTOSAVE=1 (answer Save without a box), GE_REPORT_DESKTOP=<dir>
  (Linux: where the Desktop is). Verified: F8 in a two-copy Dam run on the
  mock Steam (zip opens, five files, coop.txt lists the host's channel);
  a crash on the host mid-mission (coop.txt has the partner's position and
  channel); the next start's box (screens/reports/crash-next-start.png) and
  its autosave; the Windows build under Wine: the stall path wrote the
  minidump and put the zip on the Wine Desktop.

## Versioned saves (2026-10-25)

- port/include/port_save.h, port/src/port_save.c. A text save in Data/Saves
  starts `#ge-save <kind> <version> <commit>`; portSaveOpenRead returns the
  file past that line with the version (1 for no header: written before
  versioning), so the owner reads old layouts and its next write is current.
  portSaveOpenWrite / portSaveCommit write <name>.tmp and swap it in
  (rename; MoveFileEx REPLACE_EXISTING | WRITE_THROUGH on Windows), so a
  crash mid-write keeps the last good save.
- A legacy path given to portSaveOpenRead is the file's older place: found
  only there, it is moved into Data/Saves (rename; if that fails it is read
  in place and the next write lands in Data/Saves).
- coop_campaign.txt (coop_stats.c) and coop_character.txt (coop_look.c) are
  version 2: same lines as before, now in Data/Saves with the header. v1 was
  beside the exe.
- Backups: Data/Saves/build.txt holds the commit that last ran. A different
  one (or none, with saves present) copies every plain file in Data/Saves plus
  the two legacy co-op files to Data/Saves/Backups/<old commit or
  "older">-<date>-<time>/ before anything reads them (main.c, before the
  crash-report check). Newest 5 kept. eeprom.bin keeps the retail format
  (no header); the backups cover it.
- New save kinds (other threads): pick a name, start at version 1, read with
  portSaveOpenRead, write with portSaveOpenWrite + portSaveCommit.
- Verified: legacy coop_campaign.txt and coop_character.txt beside the
  exe were backed up (3 files with eeprom.bin) and moved in on first read;
  a co-op win on the mock Steam wrote the campaign with the v2 header and
  Continue read it back ("Continue: Facility (Agent)").

## Accessibility (2026-10-25)

- Options > Gameplay > Accessibility (PORT_SHEET_ACCESS, GE_MENU=access):
  Sound Captions, Colour Blind, Text Size, Screen Shake, then the existing
  Stamina Sway (head_roll) and Aim Control, then Crouch Key and Walk Key.
- GoldenEye has no spoken lines (briefings are text and in-game speech is
  grunts), so "subtitles" are sound captions: src/game/coop_access.c.
  coopCaption(kind, pos) from chraction.c (a guard's gun; partner puppets are
  skipped), explosion.c and propobj.c doorSetOpenState (a door starting to
  open, not one within 300 units: your own). Each line says which side
  (ahead/right/behind/left, the radar's convention); the same kind and side
  within 1.5 s merges; lines last 2.5 s, up to 4. "[ ALARM ]" stays while
  alarmIsActive(). Drawn by coop_hud.c above the bottom message line.
- [display] colorblind 0/1/2: coopPlayerColour (coop_access.c) is the one
  palette for name tags, the radar, the feed and WATCHING. Red-green and
  blue-yellow sets are from the Okabe-Ito colours.
- [display] text_size 100..200: coopTextScale; coopTextScaleBegin/End wrap
  the co-op text (partner status, objectives list, feed and chat, captions)
  and the bottom mission message in a 0x5A/0x58 scale bracket; name tag and
  ping labels multiply their own 0x5A scale.
- [display] screen_shake 0: fr.c viShake gets intensity 0.
- [input] crouch_key / walk_key 1 = Toggle: joy_port.c turns the hold key's
  press into a toggle press (crouch) or flips a latch (walk).
- Fix on the way: PORT_BIND_MAX in video.c was 32 with 34 actions, so the
  chat and photo keys could not be bound, and the keyboard poll read past its
  32-byte array: under Xvfb chat read as held and a "SAY: _" line opened by
  itself mid-mission. Now 48.
- Harness: GE_CAPTEST=<s> adds one of each caption <s> s after the first
  mission HUD frame and refreshes them for 10 s (logs "ge: captions: test
  caption").

## "Update needed" on a version mismatch (2026-10-25)

- The host's lobby carries gever (COOP_VERSION, already used to filter
  Public Games) and now gebuild (PORT_BUILD_COMMIT). A joiner arriving by
  invite, friend list or Discord compares gever in onLobbyEntered
  (coop_steam.c): another version leaves the lobby at once (coop.c would drop
  every packet of it anyway) and the lobby screen shows "Update needed to
  join." with "Host <build>, you <build>" under it (coopSteamJoinRefused 3,
  coopBrowseRefusedHint).
- Copy a GetLobbyData answer before the next call: the mock (and possibly
  Steam) reuses one buffer.
- Harness: GE_COOP_FAKEVER=<n> makes this copy claim co-op version <n>
  (two_player_lobby.sh with JOINENV="GE_COOP_FAKEVER=8").

## Drop-in (2026-10-25)

- A joiner that follows the host into a mission over 10 s old (sLateJoin,
  as before) no longer jumps straight onto the host. coopDropInTick (coop.c)
  watches a partner through coop_spectate.c (coopSpectateDropIn: alive, no
  red wash) and comes in by the respawn rules: beside the calmest partner
  once they have been out of a fight for 4 s, at most 20 s, never before 2 s
  of watching, and after coopReplSnapshotDone() (a weak default returns 1;
  the netcode thread's coop_repl.c overrides it with the join snapshot).
  Nobody standing for 60 s: it plays from the level's start.
- Until it arrives it sends no STATE, so the host has no puppet of it at the
  level's start for guards to shoot. The HUD banner says "DROPPING IN BESIDE
  <name>" with a countdown; the feed says "Dropped in beside <name>".
- Public Games: the host sets lobby data gemission=1 while in a mission
  (coopSteamSetInMission from coopBrowseTick) and the row's action reads
  "Drop In" instead of "Join". Invites and Join a Friend's Game drop in the
  same way.
- Harness: coop/docker/drop_in.sh (the joiner is killed at KILLAT and comes
  back), coop/docker/public_drop_in.sh (a third copy, nobody's friend, finds
  the game under Public Games at LATE s; THIRD=browse joins it).
  GE_COOP_TESTDROPIN=<s> holds the watch at least <s> s for screenshots.
  Verified: rejoiner and a fresh third player both dropped in mid-mission
  on the mock Steam.

## Faster host migration (2026-10-25)

- A crashed or stalled host says goodbye from its crash report
  (port_report.c portReportCrash calls coopCrashBye after the text files,
  before the minidump and the question): BYE HANDOFF to the next player, so
  the partners migrate at once. Verified with GE_CRASHTEST on the host in
  host_drop.sh (KILLAT=none): "the host handed over", the new host took
  over 36 chrs.
- A host that just goes silent (killed, cable pulled): COOP_SILENT_MS 3 s ->
  2 s, then COOP_HOST_WAIT_MS 12 s -> 3.5 s once we have played the same
  mission together for 10 s (coopHostWaitMs); early in a mission or in the
  menus the old 12 s stays (COOP_HOST_WAIT_LOAD_MS) so a slower PC's load is
  not taken for a crash. About 5.5 s instead of 15 s (host_drop.sh KILL=1).
- The world the new host takes over is unchanged here (AI sync each second,
  mirrored doors and objects); the netcode thread's coop_repl.c adds its
  snapshot through coopReplMigrated.

## Revive by holding Use, bleed-out (2026-10-25, COOP_VERSION 10)

- Revive is now an action: a living partner holds Use within 1.6 m of where
  you fell for 2.5 s (COOP_REVIVE_HOLD). Their HUD says "HOLD USE TO REVIVE
  NAME", then "REVIVING NAME" with a green bar; your downed screen shows a red
  bar draining over 60 s (COOP_BLEED_OUT) next to "BEING REVIVED". Back with
  half health. The mission fails only when everyone is down (unchanged).
- Wire: the STATE spare byte is now `revive`: standing, whom we hold Use over
  (player + 1); down, our hold count. The downed game decides (coop.c
  coopDeadTick); the reviver gets COOPSTAT_REVIVES when the partner stands.
- Harness: GE_COOP_TESTREVIVE=<s> walks to a downed partner and holds Use.
- Files: coop_revive.c (reviver side), coop_hud.c, joy_port.c
  portJoyPortUseHeldInPlay.

## Synced cutscenes (2026-10-25)

- Before: coopInMission() needs CAMERAMODE_FP, so the whole session (state,
  guards, puppets) paused in intros and cinemas; cinema chrs lived on the host.
- Now (coop_cutscene.c): while a stage intro or scripted cinema (POSEND) runs
  the session keeps going (coop.c coopInStage / coopCinemaTick), partners'
  puppets play what our own Bond plays, guns away, and our HUD is off.
- Intro: everyone's swirl shows every Bond, partners in slots beside ours;
  the host's intro ending (skip or not) ends everyone's (COOPEV_CINEMA
  INTRODONE, bondview2.c coopIntroHostDone).
- Scripted cinemas (endings, Runway/Dam style AI camera scripts): the star is
  the player nearest the cinema's pad; everyone else stands up (if down) and
  is moved to a slot beside them, including their cinema body
  (coopTeleportPlayerTo, as AI_TRYTeleportingChrToPad). The host's AI
  relay still drives the camera commands on every game.
- Player chrs (our Bond as a cinema chr, puppets) never go into guard
  snapshots or AI sync (coopIsPlayerChr); before, the host's cinema Bond was
  applied to the joiner's own Bond and crashed it.
- Harness: GE_COOP_TESTCINEMA=<s> (host runs a 5 s orbit cinema around the
  partner), GE_COOP_CINEMASHOT=<ticks> (cine_p<player>_<n>.bmp).
- Event ids: this feature set owns COOPEV 56..63 (COOPEV_DESIGN_FIRST/LAST)
  and COOPPKT 24..25.

## Results screen (2026-10-25)

- The co-op debrief (menu0D) has two pages. Page 1 "TEAM": one column per
  player with Kills, Accuracy (percent of shots), Head hits, Objectives (the
  ones credited to them, coop_objective.c), Revives (partners they got up),
  Damage (taken, in health bars: health plus armour lost), Downed (times down,
  and how many a partner revived). Page 2 "YOURS": the game's own solo table,
  with a joiner's kill total taken from the host's count.
- Next turns to page 2 before leaving; Previous or B turns back; L/R flip.
- Two new columns in COOPEV_STATS (COOPSTAT_OBJECTIVES, COOPSTAT_DAMAGE): ten
  u16s, 24 bytes of the 28 (checked at compile time). Same COOP_VERSION 10.

## 3-4 player scaling and team unlock rules (2026-10-25)

- coop_difficulty.c: per player past the second, guard health +25% and aim
  +10%, in every mode (Normal included, since Normal is tuned for two). Three
  on Normal: health x1.25, aim x1.1; four on Brutal: health x3, aim x1.56.
  No extra guards from the player count (only the mode adds guards). The count
  (coopLobbyPlayerCount) goes by connection, not a fresh HELLO, so a game still
  loading the stage counts; aim follows it live when a player drops.
  Verified with the mock: Hard soldier health 6.0 with 2 players, 7.5 with 3;
  Normal 4 players logs health x1.5, aim x1.2 on all four games.
- Cheats and unlocks: the time is the team's (coop_teamtime.c, above). New:
  the game marks Bond KIA at any death and fails the debrief on it, so a
  player who went down and came back (or was down when a partner finished)
  got FAILED and no unlocks. coopTeamKiaRule (coopMissionComplete) clears it
  when we stand at the end, a partner stands, or a partner's game ended the
  mission. A wipe still fails. Objectives, difficulty and the team's time
  decide the cheats as in solo.

## Netcode 1: every setup object replicated from the host (2026-10-25)

- Why: the events (doors, OBJDMG, pickups, knocks, Use) only carry the
  changes someone hooked, and only to players already in the mission. Level
  scripts change objects in ways no event covers (locks, flags, AI_DestroyObject,
  AI_MoveObject), and a late joiner never heard what happened before it came.
- src/game/coop_repl.c. Each setup object (the prop def types
  cleanup_objects.c frees; Dam has 241) is a record: present, in the
  sender's inventory, destroyed level, flags and flags2 (minus
  PROPFLAG_INMOTION and PROPFLAG_00000100, which each world keeps itself),
  door lock / open / still / destroyed, and position plus the 3x3 of obj->mtx
  for loose objects at rest (no parent, not moving, not a vehicle or door).
  COOPPKT_REPL (20), 16 records a packet, unreliable.
- Host: when a player's puppet first appears in the mission (and after a
  migration, coopReplMigrated from coopMigrate) the whole list goes to them,
  2 packets a tick, and applies at once (coopReplSnapshotDone() on the joiner
  once it is all in). Otherwise it walks the list a packet every 6 ticks per
  player (about 12 KB/s for Dam; the checksum work replaces most of this).
- Joiner: compares each record with its own capture. Outside a snapshot a
  difference must last 90 ticks before it is corrected (events in flight,
  doors on their own auto-close timers). Corrections only move toward the
  host and never undo: gone there and not in our hands: objFreePermanently;
  in the host's inventory: the shared pickup (coopReplTeamPick); destroyed
  further: AI_DestroyObject's damage until the level matches; flags, lock,
  door open/shut once both doors are still; moved: the host's position and
  rotation (as objDetach places an object). Doors and damage go through
  coopReplDoorSet / coopReplDestroy so they send no events back.
- Found on the way: Dam's first gate keeps PROPFLAG_DOOR_KEEPOPEN on a joiner
  that arrives after the host opened it (doorStartOpen clears it), so its
  door auto-closed differently; the join snapshot now corrects both.
- Not covered here: runtime objects (thrown items, dropped guns:
  coop_items.c's ids), chrs (guard snapshots), stage flags, alarm and
  criteria (the WORLD packet).
- COOP_VERSION stays 9: older builds ignore type 20. Ids for the netcode
  work: COOPPKT 20..23, COOPEV 48..55.
- Harness: GE_COOP_TESTREPL=<s> on the host: <s> seconds in, the nearest
  loose object is moved 5 m ahead on the left and destroyed, the next moved
  ahead on the right, the next one's flags2 changed, the nearest door's
  lock flipped, all without events. Verified with the mock: a joiner already
  in gets all four within a cycle (screens/netcode/repl-before-* and
  repl-after-*, host left, joiner right); a joiner arriving after them gets
  them from the join snapshot ("join snapshot applied, 241 objects, 4
  corrected"); 3 players through the lobby, both joiners corrected.

## Netcode 2: desync checksum with automatic correction (2026-10-25)

- Replaces the periodic walk of netcode 1. Twice a second the host sends each
  joiner COOPPKT_REPLSUM (21): an FNV-1a hash per block of 16 object records
  (what coopReplDiff compares; positions on a 24-unit grid), a hash of the
  stage flags and alarm, and an order-free hash of which chrnums exist
  (puppets left out). About 90 bytes for Dam, instead of ~12 KB/s.
- The joiner hashes its own blocks. A block that differs in 3 checks in a
  row (1 s; events in flight and doors' own timers settle before that) goes
  in a COOPPKT_REPLREQ (22) bitmask, asked again every 4 checks while it
  lasts. The host sends those blocks' records (REPLSNAP_ASKED), which apply
  at once. If they show nothing to correct (rounding at the grid's edges),
  that pair of hashes is agreed until either side's changes.
- Stage flags, alarm and chrs are already sent whole (WORLD, guard
  snapshots); their hashes only report a difference that lasts 10 s. So does
  a block still different after 20 checks: "repl: unresolved desync ...",
  counted by coopReplUnresolved(), which the CI runs fail on.
  coopReplDesyncs() counts the corrected ones.
- Verified with GE_COOP_TESTREPL=22 on Dam: "desync in block 4: 2 objects
  corrected", block 13 (the door lock) 1, then block 4 again for the debris
  that settled after the destroy; nothing asked after that, no unresolved.

## Netcode 3: smooth partners and guards, round trip, network simulator (2026-10-25, COOP_VERSION 10)

- STATE and GUARDS carry the sender's clock (ms, coopNowMs), so COOP_VERSION
  is 10. src/game/coop_interp.c: CoopClock maps a sender's clock onto ours
  (offset from the quickest packet, creeping up slowly if the route gets
  longer; send interval and jitter smoothed); CoopTrack keeps the last 8
  timed samples (late ones go in their place).
- Partners' puppets and a joiner's mirrored guards are drawn at
  now - offset - delay, between the two samples around that moment, delay =
  1.5 send intervals + 2 x jitter, 30..200 ms. Past the newest sample a
  track runs on at its speed for 100 ms, then holds; a step over 3 m (a
  respawn) is not drawn as a slide. Guard snapshots place a guard directly
  only the first time; animation is untouched (it already runs locally and
  resyncs past 6 frames). GE_COOP_NOINTERP=1 draws each packet as it lands.
- Round trip: COOPPKT_TIME (23) pings each transport peer every 500 ms, wall
  clock; coopNetRttMs(player) (a joiner's partner: twice the host's).
  "net: player N round trip .. drawn .. behind .." in the log every 10 s.
- port/src/coop_net.c GE_COOP_NETSIM=<ms>:<jitter>[:<loss %>]: every packet
  arriving is held that long (plus up to the jitter, so out of order too) and
  some dropped. Works on UDP and the Steam mock.
- GE_COOP_MOTIONLOG=1: per puppet, every 2 s of movement, the mean step per
  tick, its coefficient of variation, ticks it froze and ticks it jumped.
- Measured on Dam, host walking (GE_AUTOSTICK), both copies at
  NETSIM=60:40:2: without interpolation cv 0.81..0.87, 25..31 frozen ticks
  and 17..20 jumps per 120; with it cv 0.35, none frozen, 3 jumps (the rest
  is Xvfb's uneven frame time). Guard 6 traced on both: the joiner's follows
  the host's path about 120 ms behind.

## Netcode 4: prediction of our own shots on guards (2026-10-25)

- Our own movement was always local (each game runs its own Bond and sends
  STATE), so prediction here is about the one thing that waited for the
  host: a joiner's shot on a guard reacted a round trip plus the drawing
  delay later (and played no yelp at all on the shooter's game).
- coopClientForwardShot now calls coopPredictShot: the host's damage
  arithmetic (destruction amount x g_AiHealthModifier, head x4, chest x2,
  general halves) against the guard's health from the last AI sync less what
  we have hit it for since (before any sync: our copy's setup health x the
  lobby difficulty's multiplier, coopDifficultyGuardHealthMul). Lethal: a
  death animation from g_HitReactionTable for that part; otherwise a flinch
  animation (with the same left/right hand swap as the host). Both play at
  once with the yelp. Not for fists, gun or hat hits, or a guard the host
  already shows dying (GSF_DYING, new in the snapshot flags).
- The shot event carries predicted (COOPPRED_*) and predidx. On the host,
  triggered_on_shot_hit's two rolls go through coopReactionPick, so when its
  own outcome is the same kind it plays the same animation, and the rare
  variants (stagger against a wall, the butt-hit flinch) are skipped for such
  shots (coopReactionForced).
- While a prediction holds (round trip + drawing delay + 200 ms, 300..1200,
  doubled for deaths), snapshots leave that guard's animation alone. A death
  the host confirms keeps ours to the end if the host's is the same; if the
  host rolled another, it is shown. A death not confirmed in time is dropped
  ("predict: guard N survived"). GE_COOP_NOPREDICT=1 turns it off.
- Verified with GE_COOP_TESTSHOT=6 on the joiner, NETSIM 60:40:2: Normal,
  one PPK chest shot, "dies ... setup health", "death confirmed (same
  animation)"; Brutal (health x2), a flinch, then "dies ... after the AI
  sync", confirmed, same animation.

## Netcode 5: lag compensation for hits (2026-10-25)

- Our shots on guards already favour the shooter: a joiner hits what its own
  screen shows and the host applies the hit to that chrnum wherever the guard
  has got to. Nothing to add there.
- Guards against joiners were the unfair side: the host's guards see and
  shoot at the joiner's puppet, which (with netcode 3) is drawn a trip plus
  the drawing delay in the past.
  - Aim (host): coopRemoteAim keeps, per joiner, where they are about now:
    the newest sample run on at its speed for its age plus half the round
    trip, at most 250 ms, on the floor (coopFindStan). coopChrTickBegin moves
    the puppet there for each guard tick aimed at that joiner (sight, aim,
    hit chance) and coopChrTickEnd puts it back where it is drawn.
  - Cover (joiner): COOPEV_HIT now carries the guard's chrnum. A hit from a
    guard whose line to our Bond, from its newest snapshot position, fails
    chrCanSeeBond's own floor test right now is dropped: we got behind cover
    before it landed ("lagcomp: guard N's hit (...) dropped"). Friendly fire
    and blasts are not guard hits and always count.
- GE_COOP_NOLAGCOMP=1 turns both off. Harness GE_COOP_TESTHIT=<s> (host):
  <s> seconds in, a 0.01 hit on player 2 from the nearest guard that can see
  their puppet and one from the farthest guard. Verified on Dam at NETSIM
  60:20: the farthest (guard 43, 330 m off through the rock) dropped; real
  guard hits during a walk at NETSIM 150:50 all counted (in sight). The host
  logs the aim lead every 5 s ("guards aim at player 2 250 ms ahead ...").


## Netcode 6: two-player runs of every mission in CI (2026-10-25)

- coop/ci/mission_runs.sh <build> <out> [stages]: for each of the 20 story
  missions, a host (GE_STAGE, GE_COOP_TESTEND=900, GE_COOP_TESTWIN=1) and a
  joiner (-coopjoin, GE_COOP_NETSIM=60:20:2) over the mock Steam, PAR pairs
  at once on their own displays, mock folders and ids. A stage fails if the
  joiner never follows the host in, never applies the join snapshot, doesn't
  complete the mission with the host, either copy logs an unresolved desync
  or dies, or it takes over LIMIT (240) s. out/summary.txt has one line per
  stage. TESTEND ends the mission on the host; the objectives are not
  played, so this checks the joiner can join, stay in agreement and finish
  with the host, not each mission's level scripts.
- .github/workflows/coop-missions.yml runs it on every push. The cart image
  comes from the "ci-rom" release as baserom.u.z64.gpg (gpg symmetric,
  passphrase in the CI_ROM_KEY secret), checked against the US sha1 and
  deleted at the end; without the secret the job only leaves a notice.
- First run caught a bug: with 2% loss a lost join snapshot datagram left the
  joiner waiting for it for good, and the checksum (which waits for the
  snapshot) never ran. Now the first checksum a second after the last
  snapshot packet asks for the blocks still missing ("N records of the join
  snapshot lost, asking for M blocks again") and the asked-for records count
  towards the snapshot. All 20 pass locally, PAR=3, in about 6 minutes.
- Drop-in (the live-plumbing thread's #15) holds a joiner arriving in a
  mission under way, sending no STATE, until coopReplSnapshotDone(). The
  host only started the snapshot on the joiner's puppet, which needs STATE,
  so the two waited on each other until drop-in's 20 s cap. Now a joiner in
  the stage with no snapshot sends COOPPKT_REPLREQ with join set every
  second; the host sends the snapshot (and re-asked blocks) to it before its
  Bond appears, holding the checksum until it does. The lost-records re-ask
  moved to the joiner's own tick for the same reason. Tested merged with
  #15: Dam, joiner 30 s late, snapshot in, one lost block re-asked,
  "dropped in beside player 1 after 2.6 s". JOINAFTER=<s> in
  mission_runs.sh starts the joiner that late; CI runs Dam, Facility and
  Frigate that way too.

## Play-test fixes on the combined build (2026-10-25, evening)

- Joiner door switches: the host no longer runs the door-switch link again on
  COOPEV_OBJUSE (it toggled the Dam tunnel gates shut on the joiner).
- Guard drops on the joiner: dropped at the stand-in guard's feet
  (PROPFLAG_ONSCREEN cleared before objDrop), not from its last drawn hand
  matrix, which put guns off the floor (they fell for ever) or near the camera
  with a broken rotation. COOPEV_DROP also carries keys and other held objects
  (item -1, matched by model among the stand-in's children).
- Bodies: a mirrored guard the host says is dying (GSF_DYING), or one our shot
  predicts dead, gets actiontype ACT_DIE on the joiner (coopMirrorDying), so
  chrUpdateCollisionBounds stops blocking Bond; a death that doesn't hold puts
  the old actiontype back.
- Prediction matched handles_shot_actors better: hits on the gun or a hat do
  no damage (the moon hat counts as the head), and a punch never predicts a
  death. Helmet hits used to predict deaths the host never applied.
- Drops lie where the host's do: once the host's copy of a guard's drop has
  landed and stayed still for 30 ticks, COOPEV_DROPREST (21) sends its place
  and orientation (a quaternion in s16s); the joiners' copies stop and are put
  exactly there (coop_items.c coopItemsRestTick / coopItemsApplyRest).

## Character select (2026-10-25)

- "Your Character" opens a select screen over the lobby (front.c
  coopCharsel*, a mode of MENU_COOP_LOBBY so HELLO, the harness and the
  lobby's own ticking carry on underneath): a film strip of all 71 picks,
  the picked one large and turning, arrows, the name on a red band, and a
  file (affiliation, gender, who in the lobby wears it, the team's picks).
  The pick is only taken on Accept; Back leaves it. Nothing in the protocol
  changed: the pick still rides HELLO.
- 3D characters in menus (coop_charsel.c): each pick is loaded once, as the
  solo Bond's body is (private header copies, own buffer and Model), and kept
  for good, because the renderer's texture and part caches key on addresses.
  The port keeps model textures in its own pool, so a figure is the files and
  the Model only: 41 KB at most over all 71, in a 128 KB buffer each.
- A figure is drawn into a rectangle by its projection (the rectangle baked
  into the perspective matrix), not by gSPViewport: the port's viewport path
  is tuned for whole screens and drew large ones zoomed. Framing comes from
  the idle pose's lowest and highest joints.
- The lobby's player cards show each pick as a 3D head-and-shoulders shot
  instead of the multiplayer photo (most of the roster only had the "?").
- The title stage asks for 20 KB of dynamic vertices (-mvtx20, doubled on
  PC). Fifteen characters a frame needs more; dyn.c gives the title stage
  256 KB.
- Input: the arrow keys (read raw, input_pc.c portInputArrowStepLR), the
  wheel, a click on a strip card, L / R, the D-pad (portSheetDpadStep).
  A click on bare paper does nothing (portMouseBindMask tells a click from a
  pad's A or Enter, which accept anywhere). The hint bar has a new
  MENUGLYPH_ARROWS: ARROWS on the keyboard, LB RB / L1 R1 / L R on pads.
- Harness: GE_COOP_CHARSEL=<tick>:<steps>[:<accept tick>] opens the select
  at that lobby tick, steps (negative: left) one every 8 ticks, and takes
  the pick at the accept tick (select ticks). Tested by keyboard and mouse
  with xdotool and by pad with GE_INPUTPLAY on two copies over the mock Steam.


## A player who leaves and comes back (2026-10-26)

- Ghost lobby: a joiner who left (BYE, or its game closed) and joined again
  more than 33 s later sat alone on "Connecting to the host..." for good.
  The host's channel for that slot kept the old player's last-heard time, so
  coopSessionTick freed the slot as silent the moment Steam handed it to the
  newcomer, before its first packet; its next packet asked again, and round
  it went every frame ("freeing slot N (silent)" / "<name> joined"). Each
  channel now remembers who holds its slot (CoopChan.peerId): someone new
  there starts their silence at that moment. Same fix for a drop-in.
- Steam: a lobby created after the player chose to join another (Host, then
  Join before Steam answered) is left instead of taken as ours; an entry into
  a lobby we are no longer joining is left alone; Steam's second report of
  the same entry no longer closes the session the host just opened.
- Test: rejoin3.sh-style run (host and partner in the lobby, the third joins,
  quits, comes back after GAP=40 s): before, 229 frees and the ghost lobby;
  after, "player 3 rejoined (new session)" and all three cards. The same in a
  mission: the third drops in again beside the host.

## Tester fixes after v0.1.0 (2026-10-27)

- Own character: solo_char_load (bondview2.c) built our own Bond (intro
  swirl, cinemas) from the stage's cuff, so each player saw themselves as
  the mission's Bond while the partners saw their pick. In a co-op session
  it now wears coopCharacterBody's pick. Checked with GE_COOP_TESTCINEMA:
  Natalya and the pick-5 character look the same on both screens.
- Partner hits on chrs: coopLookPlayShot drew bullet spark type 2, which
  g_BulletSparkColors makes red. The shooter's game draws chrCreateHitPuffs'
  smoke (g_HitReactionTable's type and size for the hit part). CoopShot.impact
  now carries the hit part for COOPSHOT_CHR (it was -1; an older game's shot
  gets type 1, size 21). No wire size change.
- Music after a revive: death runs set_missionstate(0) (all tracks stop) and
  the death tune; nothing put the level music back until a later music event.
  coopRespawnPlayerAt now restarts it (sub_GAME_7F0C12CC) when the state is
  still 0, then the alarm or countdown state it had at the death.
- Voice chat bar: the talking speaker beside a name tag sat 13 px left of the
  tag, so a tag near the left edge put it at a negative x. gDPFillRectangle
  keeps 10 bits per edge, so the rect wrapped and ran across the screen in
  the partner's colour (the tester's yellow bar over the objective list). The
  speaker skips negative edges and moves right of the tag at the left edge.
- Versus bodies are drawn from coop_interp.c's track (coopRemoteInfo), as
  co-op puppets are, instead of each STATE as it lands. Run with
  GE_COOP_NETSIM=120:40:2 on both: hits, kills and respawns all agree.

## Cutscenes show only your own character (2026-10-27)

Owner's call: in a cutscene each player sees only the character they picked,
never a partner's. Replaces the "Synced cutscenes" line-up beside the star.

- Partners' puppets are hidden whenever the intro (overview, swirl) or a
  scripted cinema runs (coopCutsceneActive, coop.c coopPuppetUpdateGhost).
  The swirl line-up and its camera-near check are gone.
- A scripted cinema someone else triggered moves our Bond onto the star's
  spot rather than beside it, unless our own body already runs the ending's
  list (it walks itself there).
- Endings with Bond in them (Dam bungee jump): the host's SetChrAiList on
  CHR_BOND_CINEMA is relayed again, and the joiner's body runs that list in
  its own character from the host's moment (ported from PR #10's unmerged
  7152670). The host no longer relays presentation commands its own body's
  list runs: the joiner's body runs the same ones. The joiner's copy of the
  level script (chr 254 on Dam) reaches the ending ~4 s late; while our body
  plays the host's ending, its presentation commands are stepped over
  (coopAiSkip, its fade blacked out the jump) and its SetChrAiList to our
  body does not restart the list (coopKeepBodyList).
- That late script also ran HideAllChrs after our body was made, which hid it
  and stopped its ticks mid-jump (the joiner's screen went black). In co-op
  HideAllChrs leaves the local player's body alone (chrai.c).
- Harness: GE_COOP_TESTPAD=<pad>:<s> stands on an unbound pad <s> seconds in
  (330: the Dam bungee room, which starts the ending).
- Checked on Dam with GE_COOP_CHAR=0 (host, Bond) and 14 (joiner, male
  scientist), GE_COOP_CINEMASHOT=40: the bungee ending, the intro swirl and a
  GE_COOP_TESTCINEMA orbit each show only Bond on the host and only the
  scientist on the joiner, same shots in the same order. mission_runs.sh
  dam runway surface: all pass, 0 corrected desyncs.

## Lives, bleed-out and respawn times (2026-10-27, COOP_VERSION 12)

- Testers asked for lives and for setting the bleed-out and respawn times.
  Three host rules under When Down, synced in HELLO like the others
  (COOPSET_LIVES, _BLEEDOUT, _RESPAWN; COOPSET_MAX is now 16, so HELLO grew
  and the version went up). Each is an index into a table in coop.c; 0 is
  the old behaviour.
- Lives: Unlimited, 1, 2, 3, 5, 10, per player per mission (reset in
  coopStageStart). Every down costs one (coopDeadTick, when the fade ends);
  the down that spends the last is for good: sBledOut = COOPOUT_LIVES, the
  same "out until the mission ends" state as bleeding out, spectating a
  partner. Everyone down still fails the mission as before.
- Respawn (Timer and Revive or Timer): 3-20 s (the calm rule) or a fixed
  5..90 s. Bleed Out (Revive only, the row replaces Respawn): 15 s..3 min,
  default 60 s (was COOP_BLEED_OUT); the draining bar scales with it.
- Wire: STATE's new `out` byte (COOPOUT_*): a partner with no lives left
  shows OUT on their tag and is not revivable. Our downed screen adds
  "2 LIVES LEFT" / "LAST LIFE" under the timer.
- Lobby: the host steps the rows with A (forward) or L / R and the D-pad
  (both ways), like the versus rows.
- Harness: GE_COOP_LIVES, GE_COOP_BLEEDOUT, GE_COOP_RESPAWN=<index> (host
  only: the joiner's come from HELLO); GE_COOP_TESTDIEAGAIN=<ticks> dies
  again that long after each GE_COOP_TESTDIE.
- Tested in Dam with two copies over the mock Steam, rules set on the host
  and the joiner dying: Timer, 3 lives, 5 s: back after 5.0 s twice ("LAST
  LIFE" on the second), the third down out ("NO LIVES LEFT", the host's tag
  OUT). Revive only, 2 lives, 15 s: the bar counts down from 15 and bleeds
  out. Revive only, 2 lives, the host reviving: revived (1 left), the second
  down out and the host no longer offered the revive. The joiner's lobby
  shows the host's Lives. Shots: lives/ in the project files.

## The CO-OP screen in the style of Modern Warfare 2 (2026-10-27)

Owner's ask: a lobby laid out like MW2's (IW4x's ui_mp menus were the
reference for the structure only; nothing of theirs is used). No protocol
change: it is all front.c and menu_glyphs.c.

- The folder is gone from MENU_COOP_LOBBY: constructor_menu1D_cooplobby
  draws its own dark screen (coopMwRender): a breadcrumb and the title top
  left, our Steam name top right, the tabs under them, the tab's rows down
  the left, the party (a seat per player: colour, 3D head, name, HOST /
  READY / NOT READY / TALKING; OPEN seats) and the next mission's or the
  arena's photo on the right, the row's description along the foot
  (coopLobbyHelp; Port Options rows use port_option_help.c) and the button
  prompts under it (menuHintBarAt, left-aligned, with each prompt's edges
  so BACK takes a click). The crew strip and its cards are gone.
- Tabs: coopLobbyBuildRows is the old builder; each row now carries the tab
  it was added under (coop_lobby_build_tab) and coopLobbyBuild makes the
  view of the tab on show (coop_lobby_view). PLAY / LOBBY: host, join,
  Ready, Select Mission / Continue / Start Match, Character, voice mutes,
  Discord asks, Leave. GAME SETUP: Mode and the versus rows, When Down,
  Respawn / Bleed Out, Lives, Guards, Friendly Fire, Host Leaves, Lobby type
  and Password (read-only rows for joiners). FIND GAMES (not in a lobby):
  public games and friends. INVITE (host): Steam's window and friends.
  CONTROLS: profile, layout, the five Port Options. CHAT: the last 8 lines
  and Type a Message; T jumps there. A tab with no rows is not shown.
- Input: LB / RB (L_TRIG / R_TRIG) or watch_prev / watch_next (, and .; Q
  and E were tried but are next_inventory and use, and use is Select, so
  the press also acted on the new tab's first row) or a click change tab.
  Up / Down walk the actionable rows as before (coopLobbyScrollTick over
  the view). Left / Right (D-pad, digital Move, the arrow keys) step any
  row with a value (coopLobbySteps / coopLobbyStep: both ways for every
  rule, the character, the options and layout); a click on a value's left
  or right arrow steps it; A elsewhere is the old action (a step on for
  values, the select for Character).
- The highlighted row has MW2's lime bar; the crosshair is hidden while a
  pad or the keys have a row picked (g_portMenuMouseOwns off) and shown for
  the mouse. Opening the screen picks the first row, except under
  GE_SKIPINTRO, whose A taps would press it.
- The character select keeps its layout on the dark screen: dark cards,
  lime band and arrows, light ink; its foot sits lower (0x130).
- Names (friends, public games, voice rows, Discord asks) and info lines
  keep their case; labels and values are in capitals.
- Harness: GE_COOP_LOBBYTAB=<0..4> shows that tab, =c each tab in turn (4 s
  apiece). coop/docker/menu_shots.sh (one copy, not in a lobby) and
  menu_input.sh (two copies, xdotool keys; CLICK=x,y parks the crosshair
  and clicks, since the menu mouse is relative under Xvfb).
- Checked with two copies over the mock Steam, co-op and versus, host and
  joiner: every tab, keys for tabs / rows / values, a click on a tab, the
  character select opened, stepped, taken and left. Shots: mw2-menu/ in the
  project files.

## Mission fixes from the v0.1.0 play-test (2026-10-27, COOP_VERSION 12)

- Bunker 2, Frigate and every exit script: "if Bond is dead" stopped a level
  script for good when the host went down once, so Bunker 2's exit never ran.
  coopTargetIsDead now says dead only when the whole team is down.
- Frigate bombs: a joiner's defuser set the bomb's "used" bit only on its own
  game, and the script's "has the defuser in hand" checked the host's gun.
  USE events carry the gadget (COOPEV_USE.gadget), and the host's script
  takes the joiner's item for that one check (coopGadgetCheck/coopGadgetItem).
- Stage flags: a flag a joiner's own script set was cleared by the next WORLD
  packet before the host had it, so objectives flickered. A joiner keeps its
  own new flags for up to a second until the host's packet has them.
- Surface 2 mine: a partner drawing or throwing the mine counted as the mine
  left in the level (the fail check), and a joiner's mine only stuck to the
  helicopter where the host could see it. The thrower's game now says where
  each throw settled (COOPEV_ITEMSTUCK 29: the object and the matrix, or
  loose), and every game embeds it there (objCoopEmbed, without the onscreen
  check).
- Runway tank (coop_tank.c): the driver sends the tank's position, heading and
  turret in COOPPKT_TANK (11) every other tick; the others move their copy,
  hide the driver's puppet, and can't get in.
- Dam towers: the game stops animating a standing guard (and walks a
  patrolling one in "magic" mode) when it is off the screen, and the host's
  screen is the one that counts, so partners saw tower guards frozen.
  coopChrNearPartner keeps guards within 60 m of a partner animated.
- Harness: GE_COOP_TESTTHROWAT=<tag>:<s>[:<dist>] (stand by an object and look
  at it), GE_COOP_TESTTANK=<s> (get in the nearest tank and turn it),
  GE_COOP_TESTPAD=<pad>:<s> (stand on a pad).
- Tests (two-game runs with the US ROM): Surface 2 Agent difficulty, joiner
  plants the mine (objective B complete, credit player 2) and draws it (no
  fail); Frigate, joiner defuses after a host death (B complete, credit
  player 2); Bunker 2, host dies, respawns and reaches the exit pad (the
  mission ends); Runway, joiner drives the tank and the host's copy follows;
  Dam, idle tower guards within range animate on the host, one out of range
  stays still as before.

## Players far apart: smoothing, ordering and a connection readout (2026-10-27)

Tester report: versus and a campaign mission fell out of step once a player
from another country joined; players in one country were fine. COOP_VERSION
stays 12 (everything below talks to a v0.2.0 game).

- Found with coop/ci/interp_test.c (coop_interp.c against simulated lines,
  no game): at 60 frames a second a partner's STATE comes every 16 ms, so
  the 8 samples a track kept covered 133 ms, while the drawing delay grows
  to 200 ms once the jitter passes ~90 ms. The moment to draw fell before
  the oldest sample kept: 150+100 ms starved 11% of frames, 150+150 ms 49%
  (partners hitched and slid; versus bodies are drawn the same way). The
  two-copy runs never showed it because Xvfb's software GL runs the game at
  ~20 fps, where 8 samples span 400 ms. Tracks keep 32 samples now and the
  delay may grow to 250 ms: 0% starved up to 300+200 ms, 5% loss. CI
  (main.yml) runs the test; the old values fail it.
- Guard snapshots (20 a second) are reordered by jitter, and each entry
  applied its animation, frame, hidden/dying flags and firing at once, so an
  older one could put a guard back a step (an animation restarted, a burst
  of gunfire replayed). An entry older than one applied for that guard only
  adds its spot to the track ("guard snapshot entries came after newer
  ones" in the 10 s net log).
- A joiner's own new stage flags were kept 60 ticks for the host to catch
  up; on a 500 ms line that is not enough for what set them to reach the
  host and its WORLD to come back. Now 1 s plus two round trips.
- Reliable events: more than a packet's worth (32) went out one packet per
  round trip, so a burst on a long line (a late joiner's doors and
  pickups, a firefight's object damage) drained slowly and could fill the
  256 ring. Up to 4 packets go at once now. A bare ack no longer asks for
  an ack back (the two games bounced empty EVENTS packets every tick).
- Lag compensation: guards may aim up to 400 ms ahead of a joiner (was
  250, which a 500 ms round trip reached all the time).
- Host migration: each player's clock (CoopClock) now moves with their new
  player number, and the guard tracks start again, instead of drawing the
  new host's guards by the old host's clock offset.
- Connection quality (coopNetLink): the half-second COOPPKT_TIME pings now
  also give the jitter (smoothed deviation of the round trip) and the loss
  (pings unanswered after 2 s; the one-way share shown is 1 - sqrt(1 - both
  ways)). Good under 120 ms, 2% and 30 ms; poor from 250 ms, 8% or 80 ms.
  The host measures each joiner; a joiner its own line, and the others' from
  the host's hello (linkrtt / linkloss / linkjit appended to CoopHelloPkt;
  a v0.2.0 hello, shorter, is taken with them zero).
  - Lobby party panel: each joiner's seat shows signal bars and the round
    trip beside the name, green / gold / red, and "N% LOSS" on the status
    line once there is some (front.c coopMwLink).
  - HUD: a partner row with a fair or poor line shows its round trip (and
    loss) after the name, gold or red.
  - A line poor for 4 s in a mission toasts "<name> - slow connection (..)",
    once a minute at most: the host about each joiner, a joiner about its
    own line.
- Checked: interp_test before / after (numbers above); the 20-mission CI run
  with GE_COOP_NETSIM=150:120:4 on both copies, old and new builds; the
  lobby and HUD readouts with the joiner at 140:60:6 (shots in the project
  files, far-apart/).
- Found on the way: the game builds with -w, so an undeclared fabsf
  compiled as an int function and the jitter read 12, 31, 51, 71, 91...;
  written out by hand now. Check new calls with -Wimplicit-function-declaration.

## Versus scenarios checked, team games (2026-10-27)

- Every scenario played through over the mock Steam with coop/docker/versus.sh:
  Teams 2 v 2 (4 copies, first to 3: the team's end screen, the same on all
  four), 2 v 1 (3 copies) and 3 v 1 (4 copies) with the host's bot killing a
  team-mate (each kill takes a point off the team, so no limit is reached, on
  every copy), Golden Gun and Flag Tag (GE_VS_GRAB: the item leaves the other
  world), Licence to Kill (3 copies, one-hit kills both ways) and You Only
  Live Twice (two deaths end it). Rare's sides come from session numbers
  (2 v 2: players 1 and 4 against 2 and 3; 3 v 1: player 4 alone; 2 v 1:
  player 3 alone), and portMpTeamsEnsure leaves them as they are.
- Fixed: midas.ini's mp_friendly_fire and mp_ltk_health applied in online
  versus, per PC. A hit is checked on the shooter's PC and again on the
  victim's, so friendly fire only worked when both had it on, and LTK Health
  made one player die in one hit by their own ini. Both are off in versus
  now (video.c), so everyone plays Rare's rules: team-mates hurt, one-hit
  kills only in Licence to Kill. No protocol change (COOP_VERSION stays).
- Harness: versus.sh parks the menu crosshair off the lobby's rows
  (GE_CURSOR=20,225). With three or four copies something presses accept
  in the lobby (cause not found), and the crosshair, centred on Leave Lobby
  since the MW2 lobby, kept dropping the host out, so 3+ copy runs never
  started. Whether real input can do the same is not known.

## In-game updater (2026-10-27)

Owner's ask: the "update needed" line should lead to the update, not to
GitHub. No protocol change (COOP_VERSION stays 12); the lobby gains one key.

- The build knows its release: scripts/port/buildinfo.cmake puts
  `git describe --tags --match "v[0-9]*" --abbrev=0` in PORT_BUILD_RELEASE
  ("" without tags). CI sets GE_RELEASE_TAG on the Windows build, since a
  manual release run tags the commit after building it.
- port/src/port_update.c (worker thread; the menus only read its state):
  - Check: GET api.github.com/repos/reaffirmed/goldeneye-coop/releases/latest
    (skips the coop-latest prerelease) at startup unless [display]
    auto_update=0, when a host refuses us for another co-op version (even
    with auto_update off), and on Check Now. Forced checks at most one in
    10 s (GitHub allows 60 an hour without a token). The startup check fails
    silently (offline is normal); the others show why.
  - Newer = the tag beats PORT_BUILD_RELEASE by version order. Asset for
    this system: name ending -win32.zip (-linux.zip on Linux, none
    published yet: "Not for this PC").
  - Install: download to Data/Update/<name>.part (WinHTTP, the system
    proxy; curl on Linux, forked with our fds closed), size and SHA-256
    against the asset's "digest" (no digest: refused), then every file under
    the zip's top folder (the one with the exe) that portUpdPathAllowed lets
    through is inflated, CRC-checked and written to Data/Update/new/. Never
    written: any cart image extension, *.ini, *.bin, *.sav, anything under
    Data/ but its own .txt files (Saves, Profiles, Logs, Crashes, Reports
    are folders there), coop_* beside the exe, "..", absolute or odd paths.
  - Swap: each target renamed <name>.old (Windows lets a running exe and
    its loaded DLLs be renamed), the new file moved in; any failure moves
    every file back. Data/Update/installed.txt lists them.
  - Restart Now: CreateProcess / fork+exec of the same exe name with
    "-updatewait <pid>", then portRequestQuit (the normal quit, ini saver
    included). The new game waits up to 15 s for the old one, deletes the
    .old files and the zip, and the PLAY tab says "Updated to v...".
- port/src/port_update_util.c: release JSON, version order, the path rule,
  SHA-256, CRC-32, inflate (RFC 1951) and a zip reader, no game or OS calls.
  coop/ci/update_test.c checks them (main.yml), including the v0.2.1
  release zip read back byte for byte.
- CO-OP screen (front.c coopLobbyAddUpdate): at the top of PLAY outside a
  lobby, "Update to v0.2.2 - Install" and "You have v0.2.1"; then
  "Downloading v0.2.2 - 45%", "Installing", "Restart Now"; on a failure a
  red line and Try Again / Check Again, whose foot line says why. CONTROLS:
  "Update Check" (auto_update) and "Check Now" with our version.
- Refused for another version: the lobby now carries gerel (the host's
  release). The hint says "Host has v0.2.2, you v0.2.1", or "Host has v0.2.0
  and needs to update."; the panel says "Update Needed" (it said "Wrong
  Password" for this refusal before).
- Harness: GE_UPDATE_API=<url> (coop/mock/update_server.py plays GitHub,
  --digest good|bad|none), GE_UPDATE_FAKEVER=<tag>, GE_UPDATE_AUTO=install|
  restart. coop/docker/update.sh runs it end to end and checks the result.
- Checked: update.sh on Linux (install and restart; AUTO=none installs
  nothing; bad and missing digests refused, old exe kept; ROM, campaign
  save and midas.ini untouched; the zip's midas.ini, save, cart image and
  ../ entry left alone); the Windows exe under Wine against the mock (WinHTTP
  download, ProjectMidas.exe and SDL2.dll swapped while running, restart,
  .old files deleted by the new copy); two copies over the mock Steam with
  the host on "v0.2.2" / version 13 and the joiner on v0.2.1 with
  auto_update=0 (refusal, forced check, offer). Shots: updater/ in the
  project files.

## Spectator camera, the debrief vote and skip votes (2026-10-27)

Owner's pick from the feature list: spectate between teammates when out, and
votes after a mission and in cutscenes so the host is not the only one in
control. COOP_VERSION stays 12 (the new event is in the design range, which
an older game ignores; see "Mixed versions" below).

- Spectate (coop_spectate.c): Fire next partner (as before), Aim (R) the one
  before, Use (or B) frees the camera at the watched partner's eyes. The free
  camera flies as photo mode's (look from coopSpectateLook, which bondview2.c
  calls beside coopPhotoTick; movement on the stick / C buttons; xz only over
  walkable floor from its tile, 15..400 above it), with no reach limit.
  Fire / Aim / Use go back to a partner's eyes. coopSpectateTarget is the
  partner we look out of (-1 in the free camera, so their puppet and tag
  draw); coopSpectateHud gives the HUD the mode. The HUD says WATCHING <name>
  or FREE CAMERA at the top and the keys along the foot (clear of the partner
  rows). While watching, a partner near our own fallen body is no longer
  hidden (that rule is for our eyes inside their body), coop.c
  coopPuppetUpdateGhost. Harness: GE_COOP_TESTSPECNEXT=<s> (Fire at <s> s
  into watching, Aim 4 s later), GE_COOP_TESTFREECAM=<s> (free the camera,
  back off 1.5 s, turn).
- Debrief vote (coop_vote.c, front.c): menu0D gets a third page, WHAT NEXT
  3/3, while a co-op (not versus) session is up: Replay <mission>, Next
  mission: <next> (only after a win, and not past Cradle, as the solo Next),
  Back to the lobby, each with its voters (colour square and name). Up / Down
  or the cursor pick, Next (A) votes; Previous on page 1 and the report
  page's Previous no longer go straight to the lobby. The host counts
  (COOPEV_VOTE 57: RESULT to the host, TALLY to all, again every second
  while open so a late joiner catches up): decided when everyone present has
  voted or 30 s after the first vote; most votes win, a tie goes the host's
  way if the host is in it, else the lobby. 1.5 s later the host starts the
  mission as the lobby's Continue does (the others follow it in through
  HELLO) or everyone goes to the lobby. Harness: GE_COOP_TESTVOTE=<0..2>[:<s>],
  GE_COOP_DEBRIEFPAGE=<0..2> (the debrief opens on that page, and the report
  page moves on by itself).
- Skip votes: the intro (overview camera and swirl, one vote for both) and
  the ending that waits for a press (AI_TriggerFadeAndExitLevelOnButtonPress,
  now relayed to joiners as a presentation command, so their press counts
  and they leave the ending themselves like the host). A press calls
  coopSkipPress: with nobody else in the session it skips at once, else it
  is a vote to the host (SKIP), which sends SKIPTALLY; it passes at more than
  half (2 of 2, 2 of 3, 3 of 4) and every game skips (coopSkipPassed at the
  three bondview2.c sites). The HUD shows "SKIP n/m - WAITING FOR <names>"
  (or "PRESS TO VOTE") bottom right, in cinemas too (maybe_mp_interface's
  cameramode 1 branch). The host's intro ending still ends everyone's
  (INTRODONE). Harness: GE_COOP_TESTSKIP=<s> (press skip <s> s into each
  cutscene), GE_COOP_TESTCINEMAEND=1 (the test cinema is an ending).
- Found on the way: the joiner's "host's intro is over" was never cleared
  between missions (the menus run no stage ticks), so replaying the same
  mission ended the joiner's intro at once. coopCutsceneStageStart (from
  coopStageStart) clears it.
- Mixed versions: an older host never sends a TALLY, so a newer joiner's
  debrief keeps two pages and the old flow. A newer host with an older
  joiner waits for that joiner's vote until the 30 s clock (or the intro's
  own timer for a skip).
- Tested with two and three copies over the mock Steam (Dam): watching,
  next / back, free camera; intro skip (one vote waits, the second skips on
  both); test ending skipped by both votes, both left the mission; debrief
  Replay (both back in Dam), 1-1 tie Next / Lobby (the host's Next: both in
  Facility), and one vote for Lobby decided by the clock. Shots:
  spectate-vote/ in the project files.

## NPCs stuck in players, slow to react to a joiner (2026-10-28)

Tester report (v0.3.0): an NPC and a player wedged inside each other so
neither could move (Frigate hostages, Boris in Bunker 2), guards and Boris
slow to react, and a Frigate hostage-taker that ignored a player standing in
front of it.

- Stuck: each game moves its own Bond and (host) the guards with collision,
  but puts the partners' bodies and (joiner) the guards where the packets
  say, without one. A host guard or hostage closing on a joiner's delayed
  body lands inside the joiner on the joiner's game, and the stock movement
  test then blocks every step (a move out of a shape crosses its edge). Now
  chrSetMoving and bondviewUpdateGuardTankFlagsRelated note the prop being
  moved (coopCollideMover), and chrUpdateCollisionBounds and
  bondviewGetPropHeightRelatedValues give no shape (coopCollideSkip) to a
  player's body and an NPC that already overlap, while one of them moves,
  until they are apart. A player who is down blocks nobody (a puppet kept
  ACT_BONDMULTI's collision while down, and getPlayerCount() == 1 kept a dead
  host Bond's). Joiner side mostly: the host's own Bond and guards collide
  in one world as in solo.
- Slow or no reaction: a guard checks one player at a time (the target
  machine above), and a guard in a non-passive action (Frigate's
  hostage-takers aim at their hostage, ACT_ATTACK; Boris and others mid-run)
  never rotates, so it kept checking whoever was nearest when the mission
  started. Now its script's "see Bond", "Bond in line of sight", "Bond
  nearly in sight", "Bond aiming at me" and "distance to Bond" checks try
  every player (chrai.c, coopChrAltBegin/End/Keep): one that passes for
  another player makes that player its target for the rest of the script
  (and a sight is a stimulus); "farther than" needs every player that far,
  and the nearer one becomes the target. For a partner, "aiming at me" is
  their facing (STATE yaw) against the chr's body sphere with the stock line
  of sight (coopChrAimedAtByBond).
- Hearing: only the host's gunfire alerted guards (chrCheckGuardsHeardSound
  from the host's own shots). The host now keeps each partner's gun loudness
  as gunTickNoise does, from their flash bits and shot numbers, and runs the
  same check with their body as Bond (coopPuppetNoise).
- Frigate hostage-takers (ai_17) shoot Bond about 6 times in 10 once they
  react and the hostage the rest of the time; that is the retail script.
- Tests (two games over UDP, US ROM, old build = main at 3d9c80f):
  - Bunker 2, joiner put on standing guard 19 (GE_COOP_TESTGOTO=19) and
    holding forward: old, never moves again (282 3412 for 50 s); new, walks
    out.
  - Frigate, joiner put on hostage 44 next to hostage-taker 1, host at the
    start: old, the guard never sees the joiner (50 s); new, it sees them,
    turns and shoots, and the hostage dies.
  - Dam, joiner fires an AK for 4 s at the start: old, guard 6 stays put;
    new, "guards hear player 2's gunfire", guard 6 comes and attacks.

## Player Scaling and Friendly Fire levels (2026-10-28, COOP_VERSION 13)

Owner's pick: co-op scaling with the player count and friendly fire, planned
in the thread first (defaults as proposed). Both existed in a first form
(Guards modes, a fixed +25% health / +10% aim per player past two, Friendly
Fire On/Off for bullets only).

- Player Scaling (COOPSET_SCALING 11, Game Setup under Guards, host only,
  joiners see text): Auto (0, default), Off, 3 Players, 4 Players. On top of
  the Guards mode, per crowd (players past two; coop_difficulty.c sCrowds):
  3 players health x1.25, aim x1.1, damage x1.05, reaction x0.9 (the 007
  slider's g_AiReactionSpeed: a guard on the move stops to fire sooner),
  1 extra guard per 6 armed soldiers up to 4; 4 players x1.5, x1.2, x1.1,
  x0.8, 1 per 4 up to 8. Health and aim at 3 and 4 match v0.3.0's.
  Extra guards add to the mode's; both games allocate chr slots for the most
  the setting could reach (Auto: 4 players), so a drop-in has room.
- Count changes mid-mission: a new count must hold COOPDIFF_SETTLE (300)
  ticks, then the host multiplies every living guard's maxdamage and damage
  by new/old (keeps the share lost; puppets, chrnum 30000+, never). More
  players also raise the extra guards wanted; fewer never removes any. Aim,
  damage and reaction follow the count every tick. Joiners keep the same
  bookkeeping so a migrated host starts from the right multiplier.
- Fixed: a joiner's kill prediction (before the first AI sync) left out the
  3-4 player health bonus; coopDifficultyGuardHealthMul has it now.
- Friendly Fire (COOPSET_FRIENDLYFIRE): 0 Off, 1 Full (v0.3.0's On), 2
  Reduced (half). The lobby steps Off, Reduced, Full. The shooter sends a
  partner's bullet hit unscaled with HIT.friendly set (the old pad byte);
  the victim's game scales it by its copy of the host's setting and drops
  it at Off, so the host decides even across the lobby-to-mission handoff.
- Blasts: every explosion made records its owner (coop.c
  coopExplosionNoteOwner): a thrown or planted item its owner on every game
  (coop_items.c), a joiner's launcher round the sender on the host, and the
  host passes the owner on in EXPLODE.owner (new byte) to the others. When a
  blast reaches our Bond, explosion.c scales a partner's by Friendly Fire
  (coopExplosionDamageScale). Guards', barrels' and unknown blasts hurt in
  full; the host's own non-item blasts (its launcher rounds) are unknown to
  the joiners and still hurt them in full (not plumbed; noted, rare).
- Lives and revive: a friendly kill is a death like any other (costs a life,
  timer or bleed-out). A partner who is down can't be shot (coopShotPuppet
  needs them alive), so friendly fire never finishes anyone off.
- Stats: COOPSTAT_FRIENDLY (appended; the message is 26 of 28 bytes), counted
  by the shooter: each bullet hit sent, and once per own blast reaching a
  living partner. The team page shows a Friendly hits row when the lobby's
  Friendly Fire is not Off (rows a little closer together to fit).
- Protocol: 13 (settings slot 11, HIT.friendly, EXPLODE.owner, the stats
  count); older games get the version panel and the updater.
- Harness: GE_COOP_SCALING=<0..3>, GE_COOP_FRIENDLYFIRE=<0..2>,
  GE_COOP_TESTBLAST=<mission ticks> (once, our own blast at the first
  partner's feet). Note: two_player.sh has no HOSTENV; put host-only
  switches in BOTHENV and override them in JOINENV.
- Tested over the mock Steam and UDP with the US cart: 4 players on Brutal +
  Auto (health x3, 18 extra guards placed before the room ran out); 3
  players on Hard, player 3 killed and back 90 s later (46 guards' health
  x0.80, then x1.25); Friendly Fire Off / Reduced / Full with bullets (0,
  0.062, 0.125 a chest hit) and a blast (x0, x0.5, x1); 1 life with Full:
  8 hits down the host, out and spectating, the 2 shots after go nowhere.
  Shots: scaling-ff/ in the project files.

## Security cameras see every player (2026-10-28)

- A camera (objTick's PROPDEF_CCTV branch) only tested the host's Bond, and
  a joiner's own camera raised an alarm the next WORLD packet turned back off
  ("alarm off (host)"), so a joiner could stand in front of one for good.
- Now the host's camera runs its view test (range, 45 degree cone, line of
  sight: cctvSeesBond, the vanilla math) for its own Bond and then for each
  living partner as Bond (coopAltBondBegin), and the timer counts once a
  tick if any of them is seen. A joiner's cameras don't count
  (coopCctvWatches), so its alarm only follows the host's. Solo and versus
  are as before. The host logs "camera saw player N, alarm on".
- Drone guns (PROPDEF_AUTOGUN) already work for joiners: each game's copy
  aims and fires at its own Bond and the damage is that game's. The host's
  copy doesn't turn towards a joiner, so the two screens can show the gun
  pointing different ways.
- Guards' alarm switches are guard actions on the host, and a joiner's Use on
  an alarm is relayed (COOPEV_USE), so neither needed a change. No packet
  changes; COOP_VERSION stays 12.
- Tests (two games, US ROM, joiner on a camera's look pad with
  GE_COOP_TESTPAD, host at the start): Bunker 1 pad 88 and Bunker 2 pad 78,
  the alarm goes on in both games a few seconds later; on the old build only the
  joiner's flickered on and off. The host on Bunker 1 pad 88 still raises it.
  Bunker 2, joiner beside the drone guns: the nearest one tracks and hits the
  joiner (GE_SNDDBG agstat on the joiner).

## Text cut off on the CO-OP screen (2026-10-28)

Owner's report: chat lines and other text on the MW2-style screen ended in
"..." (a long chat line, a joiner's name beside its ping, the description
along the foot). UI only, no protocol change.

- Chat (coopLobbyBuildRows): each line is word-wrapped to the column
  (coopLobbyWrap; later rows indented COOP_LOBBY_WRAP_INDENT) and the newest
  that fit the 11 rows are shown over Type a Message; the line being typed
  ("Say: ...") wraps the same way. The builder's 48-byte copy cut a line at
  47 characters before it was ever drawn; feed lines are now COOP_FEED_TEXT
  (104) bytes, enough for a 23-character name and a whole message.
  COOP_LOBBY_ROWS went 48 -> 64 for the wrapped rows.
- The foot's description wraps onto a second line (a third is cut with
  "..." on the second): COOP_MW_FOOT_Y 0x11E -> 0x118, COOP_MW_HELP_PITCH
  0xE, the prompts at 0x13B; the next mission's block moved up 2 (0xBE).
- Party seats: only the signal bars sit beside the name now; the round trip
  (or the loss once there is some) moved to the second line's right, in
  place of YOU, so a name has nearly the seat's width.
- Names that still don't fit (seats, the Steam name top right) and the
  label or value of the picked row scroll (coopMwScroll): held at the start
  with "...", slid left a character at a time to the end, held, started
  over. Rows not picked keep "...".
- Rows: label and value are both whole when they fit; otherwise the value
  is kept whole (up to 3/5 of the column) and the label is cut. "Next Player
  Hosts" is now "New Host" and "Controller Layout" is "Button Layout" so
  they fit beside their values.
- Harness: GE_MOCK_NAMESUFFIX on the mock Steam lengthens every name;
  coop/docker/chat_wrap.sh (two copies: long names, long chat lines from
  both, the line being typed, then Game Setup, Invite and Controls). The run
  image has xdotool now (menu_input.sh needed it too).
- Checked host and joiner over the mock Steam, before (v0.3.0) and after,
  in and out of a lobby: every tab. Shots: menu-text/ in the project files.

## Host-only checks audit (2026-10-28)

Owner's question after the Valentin report: what else only works for the
host's Bond? Every place that reads "Bond" (the level-script and chr AI
commands, chraction.c's Bond helpers, propobj.c's objects, doors and
interaction, objectives, pickups, explosions, countdowns, endings) was
classified; the full list is in the thread's report. Most were already
handled (either-player pad, room and height checks; team inventory; the
guard target machine; Use, gadget and door relays; OR'd criteria). The
Valentin thread fixed "Bond holds X", "Bond's health" and the escorts'
screen checks. Fixed here (no packet changes, COOP_VERSION stays 13):

- Near misses: only our own bullets reach chrTestHit, which flags a guard
  they pass as CHRFLAG_NEAR_MISS, and the standard guard list's "Bond missed
  me" then startles it and sends it to attack. A partner's misses did
  nothing on the host, so a joiner could miss a guard with the silenced PP7
  all day. The host now runs the same bounds test along each partner's
  tracer (coopPartnerShotNearMiss, from coopLookPlayShot: their gun to where
  the bullet stopped) and commits the guard to that player (a stimulus).
- Bond's facing: chrGetAngleFromBond and sub_GAME_7F033AAC ("a pad beside or
  behind Bond", Streets' and Jungle's civilians) read our yaw even while
  Bond was a partner's puppet. coopTargetYaw gives the partner's STATE yaw.
- Endings: AI_BondDisableDamageAndPickups (only endings use it) is relayed
  with the other presentation commands, so a joiner can't be shot or pick
  things up during an ending the host's script started.
- Harness: GE_COOP_TESTAIMCHR=<chrnum>:<s>[:<deg>] faces that chr's body
  turned <deg> degrees for 8 s from <s> seconds in.
- Tests (two games over UDP, US ROM, old build = main at 1f3e068): Dam,
  joiner 15 m from guard 6 fires the silenced PP7 4 degrees to its side
  (GE_COOP_TESTAIMCHR=6:20:4 GE_COOP_TESTFIRE=5:22:4), no hit: old, guard 6
  stays where it is (act 1 then 3, same spot); new, "player 2's shot went
  past chr 6 (near miss)" and it runs at the joiner (act 9, 4 m closer).
  Dam bungee started by the host (GE_COOP_TESTPAD=330:10): new, the joiner
  logs "host script command 236" (damage and pickups off); old, it doesn't.

## NPCs a joiner can't talk to (2026-10-28)

Owner's report (v0.4.0): in Statue a joiner can't get the meeting to happen.
Valentin himself already worked for a joiner (his checks are sight and
distance, which ask every player since the NPC-reaction fix); the one that
fails is Janus by Lenin's statue, who only talks once "Bond" has holstered.

- "Bond has item X equipped" read our own player's hands. On the host that
  is the host's gun, whoever the NPC was facing, so a joiner holstering did
  nothing and Janus ended the meeting ("If he won't drop the gun kill him!",
  objective B failed). Now it asks whoever is Bond right now (a guard's
  target, from their STATE look.weapon; coop.c coopBondHasItem), then every
  other player: for a chr's script the one holding it becomes its target, as
  with the sight checks; a level script (Statue's ai_36, which also fails the
  meeting when Bond draws during it) passes when any player holds it. Not
  while damage and pickups are off (cutscenes, endings): there the check
  picks our own Bond's cinema gun. Gadget checks keep coopGadgetItem first.
- "Bond's health less / greater than" read the host's health. Now the
  health of whoever is Bond (coopBondHealth), so the taunts after a sight
  check (Facility, Control, Cradle's Trevelyan) are about the player seen.
- "If I'm on screen" counted only the host's screen; the escorts (Natalya in
  Bunker 2, Control and the Train, Control's technician) teleport up behind
  Bond when nobody sees them, and could pop up in front of a joiner. Now a
  partner sees a chr within about 48 degrees of their facing plus the body,
  120 m, with a clear line (coopChrOnPartnerScreen).
- Audit of every mission's NPC and proximity scripts: the other "Bond" checks
  either ask every player already (sight, line of sight, distance, pad, room,
  height, aimed at, heard, collected, gadget used, object used) or follow
  the NPC's target (angle, run/walk to Bond). Valentin, Natalya (Bunker 2,
  Jungle, Control, Train), Boris, the Silo and Frigate scientists and
  hostages, the Surface safe, Baron Samedi and Trevelyan use only those.
  Surface 1 and 2 reinforcement odds look at Bond's gun (fewer with a weak
  one); they now count any player's. A partner's near miss
  (if_guard_shot_from_bond_missed: Janus and Valentin end the meeting when
  shot at) is left to the host-only audit (PR #44, coopPartnerShotNearMiss).
- Two-player CI runs (mission_runs.sh) pass on Statue, Bunker 2, Control,
  Train, Jungle, Cradle, Facility, Aztec, Silo and Frigate.
- No packet changes; COOP_VERSION stays 13.
- Harness: GE_COOP_TESTGOTO takes an ai list id (1024 and up) for the chr
  running it (Statue's Valentin 1042); GE_COOP_TESTHOLD=<item>:<s> switches
  to that item (1 the fists); TESTPAD and TESTHOLD fire once at or after the
  second, since a slow software-GL run steps past the exact tick.
- Test (two games over UDP, US ROM, old build = main at 1f3e068, host left at
  the start with the PP7): joiner put on Valentin (TESTGOTO=1042), then on
  pad 59, holstered. Old: Janus "If he won't drop the gun kill him!", B
  failed. New: "No gun, 007?", the whole conversation, B complete on both
  games. Shots: npc-talk/ in the project files.

## Tester report after v0.4.0 (2026-10-28)

A tester's report from a stream on the build before v0.4.0. Items already
fixed (cameras, NPCs stuck and slow, desync, objectives) are left out. No
packet changes; COOP_VERSION stays 13.

- Spectating: the level music stopped at death and never came back;
  coopRestoreMusic now runs when spectating starts ("music back after the
  death"). A dead Bond never runs bondviewProcessInput, so the free camera
  never saw the mouse; coopSpectateMouse samples it directly (honouring
  Look Inverted), and smart mouse lock keeps the pointer captured in photo
  mode and the free camera. Strafe was reversed in both (world x runs to the
  left of a camera looking along +z). While spectating, sounds are heard
  from the camera (sub_GAME_7F053894), so a partner's gunshots no longer
  fade out with the distance to our corpse.
- Chat: a line can be 5 parts (111 characters typed, COOP_FEED_TEXT 144);
  a v0.4.0 game drops lines over 3 parts, the wire struct is unchanged.
  In-mission lines and the SAY: line wrap at 3/5 of the screen
  (coopFeedWrap), COOPFEED_ROWS 5. Typing is polled while the stage clock
  is stopped (coopChatTickStill), so the box no longer freezes after a
  cutscene with controls locked.
- Objective credit: a partner's Use (COOPEV_USE, the host's own press, an
  embed) is remembered for 300 ticks (coopObjNoteUse) and the stage script
  that reads AI_IFObjectWasActivated / UsedGadgetOnObject names who pressed
  (coopObjActivatedPass), so Dam's PC, Frigate's tracker and Statue's
  objectives toast the right player. With nobody to credit it falls back to
  the host, or the first standing partner when the host is dead.
  coopScriptSetsFlags falls back to a chr script's target.
- Pickups while the host is down: a partner's pickup used to wait until the
  host stood up again (the black box "needs several walks over"); the host
  now applies it while dead (bonddead cleared around propPickupByPlayer).
- Hostages on high co-op difficulty: guards' shots on chrs no longer take
  coopDifficultyHealthMul, so a hostage dies to the hostage-taker's shot.
- Revive: the hold is counted on the downed game; measured both ways (host
  reviving a joiner and the reverse, GE_COOP_TESTREVIVE / TESTDIE,
  GE_COOP_RESPAWN=7): 2.5 s each. The downed game now logs "revived by
  player N after X s (Y s from the first hold)" so a tester's log shows a
  hold that kept restarting.
- Harness: GE_COOP_TESTFLAG=<hex>:<s> sets the host's stage flags; the
  spectate test has a strafe phase.
- Tests: two games, US ROM. Dam objective C by the joiner (GE_DIFF=2,
  TESTFLAG=100:5, joiner TESTUSE=-1:6:10): "credit player 2". Joiner
  pickup with the host down: "player 2 picked up obj ...: ours too".
  Spectate: music log, mouse pitch, strafe dot product +6 (was -6). Chat
  wrap over the mock Steam (screenshot). Not run: the hostage shot (could
  not trigger on Frigate in the harness), partner sounds, chat after a
  cutscene.

Second pass (the stream's frames, a recount of the report):

- Guards' guns on a joiner: a joiner's chrs don't run their lists, so a gun
  a guard gets from its own list (guard_try_spawning_item: Statue's ai_5
  and ai_6 guards, Trevelyan's ambush) never reached our copy, which fired
  with nothing in its hand. coopApplyGuard now gives our copy the host's
  right-hand gun when it has none ("host chr N drew gun M"). Harness:
  GE_COOP_TESTSPAWN=<chrnum>,late arms the spawned copy 2 s later; Dam,
  the joiner logs "mirrored host chr 5038 (body 37, gun 0)" then "drew gun
  8".
- Free camera dead zone: 10 of the stick's 70 units.
- The stream's 1:02:00 and 1:23:00 "desyncs" are the host's spectator
  camera in its partner-eyes mode (the frames say WATCHING), with the
  partner's own body in view. The camera sat at their last STATE while the
  level was drawn from their drawn body's room, and their body (hidden only
  where the camera is) was drawn elsewhere: at 1:02 Sagar was at 1.7 s and
  88% loss, so the view went into the rock and his body walked ahead of
  it; at 1:23 Crshrry's body stood in a doorway inside a guard. The camera
  now follows the drawn body (coopSpectateFollow), so view, room and the
  hidden body agree. Checked at GE_COOP_NETSIM=200:100:50.
- 1:32:35, Frigate's ending for a player out of lives: spectating carried
  on into the ending with our room swapped for the partner's, so the
  cutscene camera went through the boat's hull into the void. Spectating
  stops for the ending and intro cameras (CAMERAMODE_POSEND, _INTRO).
- Three players, player 3 revives player 2 (tester's 1:31:00):
  "revived by player 3 after 5.1 s (2.5 s from the first hold)".
- Statue's INCOMPLETE ending is the helicopter countdown (ai_37, 180 s):
  it runs out, flag 0x40, ai_44 blows up the heli and Natalya with it.
  Natalya gets up when Bond is within 3 m (ai_21's
  if_guard_distance_to_bond_less_than, already any player's on the host);
  why she waited for a death in the stream needs the joiner's view.

## Overlooked: hellos, votes, a read-only folder (2026-10-28)

A sweep for bugs outside the host-only audit, tested at 3 and 4 players over
the mock Steam with the US cart (copy of host_drop.sh with GE_SKIPINTRO=0:
its A taps in the first 15 s can land on the lobby's Leave row and make the
harness host hand over and re-host).

- Host migration split a 3-4 player group. Forced hellos (coopKeepAlive in
  cutscenes and pauses, the mission card) set sTick to 0 for the call, so the
  roster's "here" test (a hello within 180 ticks) read every joiner as gone.
  Joiners' player counts flapped 2/3 all mission ("scaling Auto as 3
  players" / "as 2 players" every few seconds), and a host lost right after
  one of those hellos left each joiner hosting alone ("player 2 hosts ... 1
  players" on player 2 and "player 3 hosts ... 1 players" on player 3).
  coopSendHelloNow sends off-period without touching sTick. Retested: host
  killed while player 3 was down (Revive, 2 lives): player 2 hosts both,
  player 3 follows, is revived by the new host 19 s later; two scaling lines
  in the whole run.
- Votes (coop_vote.c): the after-mission vote and the skip vote count only
  players still talking (coopLobbyPlayerHere, so a crash stops counting after
  3 s, not after the 30 s the connection is kept); a skip vote recounts each
  tick so a leaver can't hold it; coopVoteRenumber moves votes to their
  players' new numbers in a migration. Tested: 3 players, player 3 votes
  Lobby and is killed, player 2 then the host vote Replay: "decided replay
  (2 of 2 voted)" on the host's vote, not after the 30 s clock.
- Updater: in a folder the player can't write, the check and the install
  said "download failed (curl 23)" (WinHTTP: "can't write <long path>").
  Both try the file first: "Can't write to the game's folder; move it
  somewhere you own". Tested as an unprivileged user on a read-only copy, and
  the same copy writable installs.
- Checked, no change: 4 players with 5% loss on each joiner, player 4 killed
  and rejoining mid-mission (snapshot, drop-in beside the host, 1 object
  corrected, no unresolved desync); versus ignores the co-op Friendly Fire
  and Scaling rows (hidden in versus; blasts check coopVersus, bullets go
  through coopVersusDamage, not puppets).

## Smarter guards (2026-10-28, COOP_VERSION stays 13)

Owner's pick: an optional guard AI, planned in the thread first (two
settings, a Port Option, Classic untouched, Smarter runs count for cheats
and team times). All of it is in src/game/coop_ai.c; Classic runs none of it.

- The setting: Port Option "Guard AI" ([display] guard_ai, Classic 0 by
  default, Smarter 1). In co-op only the host runs guards, so the host's
  option decides; the host's Game Setup row (under Player Scaling) edits the
  same option, and COOPSET_GUARDAI (slot 12) only carries it to the joiners'
  lobby text. Read once a mission (coopAiSmarter latches per stage), so a
  change mid-mission waits for the next. Versus always plays Classic.
- The hook: ai() in chrai.c asks coopAiTick first; nonzero skips the
  script for that tick. A guard is the brain's only while it is a generic
  armed guard (not hidden or invincible, no puppet) on one of the stock
  guard lists (standard, simple, clone, persistently chase) or on Attack /
  Run to / Startle / Idle called from one. Level scripts, scripted
  characters and every other list run as before, and the brain lets go the
  moment a script moves the guard elsewhere.
- Hearing: chrCheckGuardsHeardSound scales the range by coopAiHearScale:
  in the outer half of the range, a door or wall between the guard and the
  shot halves it (one line test, doors only). Partners' shots already wake
  guards in Classic too (coopPuppetNoise, PR #42) - the plan said Smarter
  only, but that change came first as a tester fix and is left as it is.
- Looking (a guard that heard or saw something but not Bond): LOOK (turn
  and look round), INVESTIGATE (walk to the heard spot, run when answering a
  call), SEARCH (two nearby waypoints), RETURN to post. A body seen within
  15 m sends a guard to look into it. Seeing a target from any of these
  starts the fight.
- Fighting: on first sight the guard calls up to 3 calm guards within 20 m
  (they investigate the spot, so they come in looking, not knowing), points,
  and 1 in 4 run for a working alarm within 30 m (one at a time, 60 s apart,
  never when it is already ringing). Then bursts (the script's Attack Bond,
  handed back after each); after 2-3 bursts or a hit, COVER (a pad 1.5-9 m
  away with no line to the target), DUCK (kneel 1-2 s), PEEK (back out to
  fire). At 65% damage it falls back once. Losing sight: hold 1 s, then 1 in
  3 flank to a side pad, else advance to the last seen spot; 30 s with
  nothing ends in a search and the walk home. With two or more players a
  guard every 4 s turns to the player the fewest guards are on, if it can
  see them.
- Netcode: none. Only the host runs guards and its brain state is not sent;
  joiners see the result in the usual guard snapshots, so the protocol
  stays 13 and Classic and Smarter hosts both work with any 13 joiner.
- Cost: sight and cover searches are capped (40 line tests a search, 2
  searches a tick); one small brain per chr slot (256).
- Harness: GE_GUARDAI=0|1 (the option without midas.ini), GE_GUARDAIDBG=1
  (state changes, calls, splits, muffled shots), GE_GUARDAI_TESTNOISE=<s>[,<loudness>]
  (one gunshot from Bond at <s> seconds), GE_GUARDAI_TESTALARM=1 (a first
  sighting always looks for an alarm).
- Tested with the US cart: Dam (solo, a test shot: look, investigate,
  search, return; Classic in the same run walks straight to Bond; a fight
  from 18892,1,17060: cover, duck, peek in cycles), Archives (solo and two
  players: call for help, advance, flank, guards splitting between players),
  six two-player missions over the mock Steam with Smarter on the host
  (dam, facility, archives, frigate, surface, depot: all pass, no desyncs),
  and Classic against the build before it in the same Dam run (differences
  within run-to-run noise). Shots: smart-ai/ in the project files.
- Not seen in a run yet: the alarm run, the fall back, a guard finding a
  body. Each has its own GE_GUARDAIDBG line to look for. (All three seen
  since, see the next section.)

## Smarter guards: code review and the three unseen moves (2026-10-29, COOP_VERSION stays 13)

The module had only been play-tested. Read end to end, then the three moves
never seen were driven in two-player Dam runs (Smarter on the host, Classic
on the joiner's own option):

- Alarm run: the joiner stood by guard 41 (GE_COOP_TESTPAD=179:3,
  GE_COOP_TESTHUNT); 41 ran down the tower to the alarm at pad 10070 and set
  it off, the joiner logged "alarm on (host)" and saw it run past. With
  GE_GUARDAI_TESTALARM=1 (the 1 in 4 roll always passes) and without it in a
  longer run (guard 22, same alarm). Facility and Bunker have no alarm
  objects; Surface's one has no guard of ours within 30 m.
- Fall back: GE_COOP_TESTSHOT=41,201 on the joiner (a quarter-damage shot
  every 2 s); after the third (75%) 41 fell back to pad 190/191/177, ducked,
  fought on, and died to the fourth.
- Body: GE_COOP_TESTSHOT=12,8 on the joiner (one silent head shot from the
  start, out of sight); guards 10, 11 and 45 each found it once, looked,
  walked over, searched two waypoints and went back to post.

Fixed:
- A guard never found a body: the once-a-second check was
  (g_GlobalTimer + slot * 7) % 60 == 0, and g_GlobalTimer steps by the
  frames since the last tick (2 at 30 Hz), so many guards never hit 0.
  Before/after on the same run: 0 finds, 3. Now a per-guard timer.
- Once that works, a guard would have walked back out to the same body every
  time it got back to post. Each guard remembers 8 bodies: one it found, one
  it saw fall, and the ones in view when it gets back to post.
- SEARCH went straight home: in its first 2 ticks the state didn't wait, so
  both waypoints were picked (and dropped) at once. It now waits for the walk
  (coopAiArrived already skips the first 10 ticks).
- A host migration switched the guards to the new host's own Guard AI
  option mid-mission (joiners latch at the mission start, before the first
  snapshot, from their own option). A joiner now latches COOPSET_GUARDAI,
  the host's. Before/after: host (Smarter) leaves at 40 s, the joiner
  (Classic) hosts, a test gunshot at 50 s: no smarter moves, 36.

Checked and fine: the joiner runs no brain while it mirrors (its chr ticks
are skipped), the host's brain uses the guard's own target in co-op
(g_CoopPropOverride), partners' shots are muffled from the partner's spot,
alarms and the guards' moves reach joiners in the usual snapshots. A
three-player Dam run (host Smarter, both joiners fighting): no unresolved
desync; the one "objects corrected" per joiner is a crate shot near the
host's start, and a Classic run shows the same.

Left as design, not bugs: cover is only out of sight of the guard's own
target (another player may see it); the alarm run checks straight-line
distance, not path length; the brain's state isn't sent, so after a
migration guards start over from their chr records.

## Far-away players dropping in mid-mission (2026-10-28)

Tester report: a player from Colombia dropped in to a running co-op mission;
then desync, the joiner couldn't kill guards, and it disconnected. COOP_VERSION
stays 13 (no packet changes).

- Harness: coop/docker/far_drop_in.sh. The host starts Dam with a nearby
  partner (NEARSIM 30:10:0); at LATE s a third copy on FARSIM (300:80:3)
  drops in. GE_COOP_TESTHUNT=<s> on the far one: stands 3 m from the
  nearest living guard and aims at it, GE_COOP_TESTFIRE fires a KF7; it logs
  each guard the host reports down. GE_COOP_NETCUT=<s>:<len> (coop_net.c)
  cuts a copy's line both ways for <len> s. The 10 s net log now has what we
  send each peer ("net: to player N .. KB/s in .. packets/s (% by type)",
  coopNetSentTake).
- At 300 ms every shot the far joiner sent was applied on the host (64 of
  64) and it killed what it could see. Shots carry the chrnum and are not
  checked against positions or clocks, so a stale view doesn't reject them.
  The failure was what follows.
- The host's keep-alive hello (coopKeepAlive, and the mission card's) set
  sTick to 0 around coopSendHello to force a send. coopLobbyPlayerHere then
  aged every joiner's last hello by minus sTick, so each of those hellos told
  the joiners nobody else was here. With 3-4 players, the joiners' player
  count flipped every half second (Player Scaling's guard health for kill
  prediction, skip votes, voice, stats resends), and the host-migration
  roster (sHostHello) was often "nobody". coopSendHelloNow sends out of turn
  without touching sTick.
- The disconnect, reproduced with NETCUT 7 s on the far joiner (old build):
  after 2 + 3.5 s of silence it took the host as gone and migrated to the
  near joiner (still with the host, so it never took over), then to itself:
  "YOU ARE THE HOST NOW", alone in its own world, its shots going nowhere;
  the real host showed "disconnected" 30 s later. A host that really goes
  says so (BYE from the crash report, or Steam sees it leave the lobby), so
  the quiet case is mostly our own line. A joiner whose line was poor when
  the silence began (round trip from 250 ms or jitter from 80 ms), or that
  already had a silence this mission, now waits COOP_HOST_WAIT_FAR_MS (12 s)
  before migrating, inside the host's 30 s before it drops a joiner. Same
  test, new build: "the host is back", nothing lost.
- Reliable events: every unacked event went out again every 4 ticks, with
  every new event and with every ack, so on a 400 ms line each went 5-6
  times, more in a firefight. Now a new event goes once, the unacked ones
  again when the oldest has waited the round trip x 1.25 + 2 x jitter + 50
  ms (80..1200), and the receiver keeps events that arrive ahead of a
  missing one (CoopChan.in / inSeq) instead of dropping them for a resend.
- What the host sends each joiner, Dam, 3 players (far_drop_in.sh): 45
  KB/s and 145 packets/s, 44% STATE and 52% guard snapshots. Now:
  - Guards: a finished body once a second, a guard more than 60 m from every
    player 10 a second, the rest (and anyone firing or dying) 20 a second
    (coopGuardDue). Joiners free a chr missing for 5 s (was 2).
  - STATE (ours and the relayed ones) at 30 a second to each joiner when 3+
    players are in the session or that joiner's line is poor (coopStateThin;
    not versus). Each player added 10 KB/s to every other joiner's line and
    the host's upload, so a third or fourth player raised everyone's ping on
    a slow upload.
  - Result, same run: 25 KB/s and 86 packets/s to each joiner.
- Checked: far_drop_in.sh with HOSTENV GE_COOP_TESTEND=7000 and the far
  copy's line cut for 6 s: dropped in, 11 guards down, 63 of 63 shots
  applied, the host came back, the mission ended together.
  mission_runs.sh: dam facility runway surface bunker1 silo; JOINAFTER=30
  dam facility frigate; the same at NETSIM=300:120:4: 12 of 12 pass.
- Found on the way: coop.c used atan2f and fmodf and coop_give.c fabsf
  without a declaration (the game's include/math.h has none of them, and -w
  hides it): they returned garbage, so "the partner aims at this guard"
  (coopChrAimedAtByBond) and give-to-partner's height check were noise.
  coop.c includes math_atan2f.h now; the other two are written out.

## Mission-by-mission risk review (2026-10-28)

Owner's question: what else could break in co-op, mission by mission? Every
mission's level and NPC scripts were read for checks on "Bond" that the
earlier audits don't cover; the full per-mission table is in the thread's
artifact. Tested on main plus #44 to #50, two games over the mock Steam, US
ROM. Fixed here (no packet changes, COOP_VERSION stays 13):

- Facility, Dr. Doak: the script hands "Bond" the door decoder
  (AI_BondCollectObject). That gives it to the host's Bond, and the stock
  pickup refuses a dead one, so with the host down (bleeding out or out of
  lives) while a partner met Doak the decoder went nowhere: objective B
  completed, nobody could open the bottling room door. The host is stood up
  for that pickup (coopScriptGiveBegin/End, as for a partner's pickup while
  the host is down) and coopPickedUp shares it. Test: GE_DIFF=2, host
  GE_COOP_LIVES=1 GE_COOP_TESTDIE=240, joiner GE_COOP_TESTPAD=11:6
  GE_COOP_TESTGOTO=79. Old: B complete, no pickup on either game. New:
  "a script gives Bond an item while the host is down: taken for the
  team", host "we picked up obj 41432", joiner "player 1 picked up obj
  41432: ours too".
- A down host's body counted as "Bond" for a level script's room, pad
  distance and height checks (coopAltBondBegin adds the living partners,
  but the first check is always our own Bond, dead or not): a host bleeding
  out or spectating inside an objective room, Cradle's fall shaft or by a pad
  Trevelyan runs from kept those checks true (and "every player far" false).
  While a partner stands, the down host's body is nobody
  (coopOwnBondCounts). Checked by reading; the two-player CI runs pass.
- Harness: GE_COOP_TESTGOTO looks for a chr that isn't there yet every
  second until 90 s (Doak is spawned when someone enters his lab).

Found, not fixed here:

- Facility (seen, cosmetic): the joiner gets "objective B failed" for about a
  second when it reaches Doak's lab first; its own copy of the flags has
  0x10000 until the host's WORLD packet clears it.

Second round (same day), also fixed here (this round does change the
WORLD packet: COOP_VERSION 14):

- Cradle fall: ai_39 is the "Bond fell off the platform" check (in pad
  147's room, below y -2550 then -4070: list 0x041c, KIA and the fall
  ending). Any one player dropping into that shaft ended the mission for
  the team (all_objectives_complete=0), host or joiner, and the host's
  partner loop also ran it when a joiner's puppet came up out of the shaft
  on respawn. In co-op those two height checks never pass
  (coopLevelFallCheckOff in AI_IFBondYPosLessThan) and each game kills its
  own Bond at the bottom of the shaft (coopCradleFallTick): an ordinary
  death, so lives, bleed-out and the team-out rule decide. chrai.c sees
  coop.h only further down, so it declares coopLevelFallCheckOff itself:
  undeclared, its f32 went as a double and the check read garbage. Tests
  (GE_COOP_TESTPAD=147:10, GE_AIDBG=1 to watch for list 1052): joiner falls (three runs) ->
  joiner down and spectating, respawned by the host 20 s later where the
  host was calm, no 1052 and no mission end; host falls -> the same the
  other way round; joiner falls on its last life (GE_COOP_LIVES=1) ->
  joiner out, host plays on; both fall -> both down, back to the lobby
  with the mission failed. Before this, three of five joiner falls ended
  the mission through 1052 just after the joiner's respawn.
- Countdowns (Silo, Statue, Bunker 2, Train, and every other
  hud_countdown_* user): each game ran its own copy of the timer from its
  own copy of the script, so a player who dropped in late had a clock that
  started when they arrived, and the blast (AI_TriggerExplosionsAroundBond)
  went off in each game on its own clock. Confirmed on Silo, GE_DIFF=1,
  joiner 30 s late: host 449 s at its start, joiner 444 s at its own start
  some 25 s later, so its blast and fail came 25 s after the host's. Now the
  host owns the clock: a joiner skips its own HudCountdownSet/Start/Stop/
  Show/Hide and the blast (coopAiSkip, coopHostOwnsCmd), the host relays
  the blast like other presentation commands, and the WORLD packet carries
  the clock (value, running, shown; a joiner snaps to it when more than
  half a second off). COOP_VERSION 14: a joiner now leaves its countdown
  to the host, so both sides must be this build. Retest: the late Silo joiner shows "countdown running and shown,
  427.3 s left (host)" on arrival and stays in step; Bunker 2 with both
  joining together, and with the host out of lives (GE_COOP_LIVES=1
  GE_COOP_TESTDIE=600), stops on the host's zero and the blast reaches the
  living joiner. Statue and Bunker 2 with players joining together were
  already within a second before the fix; Train's timer needs chrs 0x46
  and 0x48 gone and wasn't reached in the harness.
- Harness: GE_COOP_TESTFLAG=<hex>:<s> sets stage flags on the host that far
  in (the hex as the game holds it: if_objective_bitfield_is_set_on's
  argument in a setup file is byte-reversed, 0x00800000 there is 8000
  here); GE_COOP_TESTCLOCK=1 logs the countdown every 5 s.

## Versus gameplay: bodies, pickups, drops, throws, objects (2026-10-28, COOP_VERSION 14)

Owner's report: online versus "doesn't play properly": animations are
weird, items aren't synced, players don't look where they're going.

- Root cause of the bodies: lvlViewMoveTick (boss.c) ran MoveBond for every
  player, the other PCs' players included. With no pads for them it zeroed
  their speeds every frame and kept vv_theta from the spawn, and
  playerTick's remote-body block then posed them from that: the idle anim
  (532, rate 0.05, "slow motion") facing the spawn direction while the
  real player ran about (audit before: facing off by up to 179 degrees).
  Found with a hardware watchpoint on the remote player's speedforwards.
  Now boss.c skips coopVsRemote players, and coopVsFeedPlayer
  (coop_versus.c) fills their player struct from STATE each tick: vv_theta
  from the yaw, speedforwards/sideways/theta, aim (field_2A08/2A0C),
  headanim, crouch, the gun flash per hand. Rare's own playerTick then
  picks the run, strafe, back and crouch anims and turns the body, as in
  split screen. coopLookBodyTick (coop_look.c) is the puppet tick without
  the animation part: guns, shots and sounds only.
- Pickups: taking an arena object sends COOPEV_VSPICK (37: setup offset,
  type, item). The other games free it the same way (objFree +
  TICKOP_FREE), or if it is still on its way back reset its timetoregen to
  CHROBJ_REGEN_TICKS, so everyone's respawn timer starts together. The
  scenario item still goes as VSITEM.
- Drops and throws: coop_items is on in versus. drop_inventory sends each
  gun a dying player drops (COOPEV_DROP, chrnum COOP_PUPPET_CHRNUM + net
  index); thrown grenades and mines replay owned by the thrower's body
  (RUNTIMEBITFLAG_OWNER is the local index of that player, so the kill is
  theirs); the thrower's game says where they came to rest. Picking one up
  only removes it elsewhere. Guard-drop code stays co-op only.
- Objects: object damage from a player goes to everyone (OBJDMG, applied as
  that player's local index) and knocks are shared; doors already were.
  coopVersusExplosion skips blasts coop_items is replaying.
- Harness: GE_VS_WANDER=<s> (walk about: run and turn with a B pulse for
  doors, strafe, back, stand turning on the spot, stand, 3 s each, through
  coopVersusTestInput in MoveBond; the turning adds to vv_theta as the
  mouse does, since the stick's x strafes in the PC layouts),
  GE_VS_AUDIT=1 (every 2 s per other player: their yaw, facing error less
  the sidestep lean, anim and rate, speeds, aim sent and drawn, flash;
  every 10 s "vsitems": pickups here and coming back with a hash, dropped
  or thrown items, damaged objects and open doors with hashes),
  GE_VS_PICKUPS=<s> (step onto the nearest pickup every 4 s).
- Checked over the mock Steam with GE_NETSIM 120-150 ms: 2, 3 and 4 players
  on Temple and Facility. Facing within 5 degrees in every sample, a
  player standing still and turning on the spot included (the tester
  stream's opponent who never turned toward the shooter); anims
  follow the movement; the pickup and respawn hashes match on every game
  at quiet moments; death drops, grenades (thrown, landed, went off
  together) and object damage reach the others; 4 players at
  120 ms: the kill table was the same on all four and the end reached
  everyone. Story co-op runs (dam, facility, frigate) still pass.
- Shots: versus-gameplay/ in the project files.

## Joiner review: the mission clock and time after a host change (2026-10-28)

Owner's question: reading everything from the joiner's side, what could a
joiner hit? The full review (per system, graded) and a tester checklist for
the joiner-does-the-objectives pass are in the thread's artifact. Fixed here
(no packet changes, COOP_VERSION stays 14):

- Countdowns after a host change: the WORLD packet carries the clock only
  once the host's own script has used it (sClockSet), and a joiner skips its
  countdown commands, so a joiner that took over as host never sent the
  clock. Anyone who dropped in or rejoined afterwards ran a fresh timer of
  their own, and the others never heard the new host stop or hide it. A
  joiner now notes the host's clock (coopApplyClock), so it keeps sending it
  as the new host. Test: Silo Secret, two players over the mock, host killed
  at 60 s and back at 75 s as a joiner (coop/docker/host_drop.sh, KILL=1
  REJOIN=75, GE_COOP_TESTCLOCK=1). Old: the rejoiner started at 443.6 s
  while the new host was at 372 s. New: "countdown running and shown, 392.5 s
  left (host)" on arrival, then 0.2 s from the new host's.
- The team's time after a host change: the new host sent and debriefed on
  its own mission timer, which started when it got in, so a late joiner
  that took over cut the team's time short (and could beat a target time
  the team didn't). It now goes on from the old host's
  (coop_teamtime.c sAdopt). Test: the same run with GE_COOP_TESTEND on the
  new host: "team time goes on from the old host's", the rejoiner's debrief
  gets 103.86 s against its own 32.6 s.

## Known issues from v0.5.0 and the overlooked lists (2026-10-28, COOP_VERSION stays 14)

New messages are design events (58 COOPEV_LIVES, 59 COOPEV_SEEN), which an
older build drops in coopDesignApply, so no protocol bump.

- Friendly Fire Off and the host's rockets: a launcher or grenade-launcher
  round is a projectile, not a thrown item, so the host's EXPLODE went out
  with owner 0 and a joiner took it at x1.0. coopItemsExplodeBegin now marks
  a blast from our own round (projectile->ownerprop is our Bond) and the
  host tags it as ours. Joiner log, host firing GE_COOP_TESTFIRE=25: old
  "a blast nobody owns reaches us: x1.0", new "player 1's blast reaches
  us: x0.0 (Friendly Fire Off)".
- Lives across a rejoin: the joiner reports its lives each mission (and on
  change) with an arrival id; the host keeps them by Steam ID and answers a
  new arrival with what that player had left ("Back in: N lives left"). A
  player who was out comes back out (spectating), without spending a life.
- Found while testing that: a joiner who rejoined mid-mission kept its old
  channel on the host until the host saw the new session, so the joiner
  took the host's old ack for its own new events and dropped them. Shots,
  pickups and Use from a rejoined player never arrived (GE_COOP_TESTSHOT:
  0 hits on the old build, 1 on the new). coopSessionTick restarts the
  channel when a new peer takes the slot.
- Controller-only chat and lobby password: Steam's floating keyboard when
  Steam offers one (Big Picture, Deck), else the front end's on-screen
  keyboard drawn over the game or the lobby; the player's movement is held
  while it is open. Harness: GE_COOP_TESTGRID=<s>:<presses> (A B S U D L R,
  one every 12 ticks); the mock's floating keyboard answers only with
  GE_MOCK_FLOATKB=1.
- The host's lobby settings (difficulty, friendly fire, host leaves, lives,
  bleed-out, respawn, scaling, the down rule and the versus rows) are kept
  in Data/Saves/coop_lobby.txt. Versus mode itself and the guard AI (a Port
  Option) are not. GE_NOINISAVE runs neither read nor write it.
- False "slow connection" toasts after a host migration: the old host's
  unanswered pings counted as loss on the channel the new host took over.
  3 players, host killed: old, "18% loss" and "30% loss" toasts on both
  survivors; new, 0%. coopLinkStatsReset after coopNetMigrate.
- Drone guns: each game's copy aimed only at its own Bond. Every game now
  aims at the nearest living player in range and arc that it can see
  (coopAutogunTarget), so the gun points the same way everywhere, and only
  the target's own game deals damage. Bunker 2, joiner in front of the gun
  at -3740 3087 (GE_SPAWNXYZ=-3740,600,2750), GE_SNDDBG agstat on the host:
  old, never sees anyone; new, sees=1 los=1 active=1 and fires.
- "If I've not been seen" and the clone-when-unseen spawns read
  CHRFLAG_HAS_BEEN_ON_SCREEN on the host, which only the host's screen set.
  Joiners report the guards they have drawn (COOPEV_SEEN, every 0.5 s);
  the host logs "chr N seen by player 2".
- Checked, no change:
  - Endings a host script starts: "equip unarmed" without "hide weapons"
    only happens behind a fade to black (Cradle ai_31).
  - Frigate hostage-taker 1 (joiner on hostage 44 aiming at him,
    GE_COOP_TESTAIMCHR=1:8): he shoots the joiner; with the host aiming he
    drops his gun and backs off. Not reproduced.
  - Bunker 1 technician (chr 25, list 0x408), led by the joiner and by the
    host with GE_COOP_TESTAIMCHR=25:8:0:95:1 (aim and follow): he takes the
    same route both ways. Not reproduced.
  - Statue's Natalya (list 0x416; the ROM's flags are the setup file's with
    the bytes reversed, so the heli trigger is GE_COOP_TESTFLAG=8000): she
    gets up for the joiner alone and with both players by her, objective D
    completes, and the joiner sees it too. Reaching the helicopter cuts the
    countdown to 15 s; when it runs out the blast kills her if she is still
    beside it and D fails, the same with the host as the one who reached
    her. Her waiting for a death in the stream is not reproduced.
- The host leaving during its ending (Dam bungee, host on GE_COOP_TESTPAD=330
  and killed 4, 10 and 16 s in): a joiner following the host's ending, or one
  that had only seen its "disable damage" step, became host and nobody ended
  the mission (first person with A done, or a black screen). The new host now
  ends it ("the host left during its ending: ending the mission") and
  debriefs. A joiner leaving during the ending was already fine.
- Harness: GE_COOP_TESTAIMCHR=<chr>:<s>[:<deg>[:<len>[:<follow>]]].
- coop/ci/joiner_spots.sh: two-player checks where the joiner acts (bungee,
  drone gun, hostage taker, Natalya).
- Deferred: a per-user data folder for read-only installs (moves where saves
  live; its own change).

## The CO-OP screen: clearer, and a Classic look (2026-10-29)

Owner's report: testers find the MW2-style screen confusing and think it
may be missing options; and a way back to the original menu. UI only, no
protocol change.

- Compared every row against the folder lobby before the MW2 change
  (339c342^) and the mod's options: no co-op row was lost; every rule
  (When Down, Respawn / Bleed Out, Lives, Guards, Player Scaling, Guard AI,
  Friendly Fire, Host Leaves, Lobby, Password, versus rows) is on GAME
  SETUP once hosting. What a player couldn't reach from the screen: the
  rest of the Options folder (video, audio, HUD, accessibility, key binds,
  Port Options).
- CONTROLS is now OPTIONS, and ends with "All Options": the Files menu's
  Options folder, with the lobby running behind it (coopFrontTick); its
  PREVIOUS comes back onto the row (port_options_from_coop,
  coop_lobby_reopen). A mission the host starts meanwhile still takes the
  player in.
- Confusing spots fixed: the tab keycaps were "," and "." in a font that
  draws them as dots, so the bar looked like two blank keys; Tab / Shift+Tab
  now change tab too (portInputTabStep, not a bind) and the keyboard's bar
  shows one TAB key (a pad keeps LB / RB). Before hosting, GAME SETUP only
  had Lobby and Password; it now says the mission rules appear once you
  host. A host alone sees where to invite from, and Select Mission (Start
  Match) says "No Players" rather than "Not Ready" with nobody there.
  "Guards" is "Guard Strength" (it sat beside Guard AI and the mission's
  own difficulty), with a description that says what Hard and Brutal do.
  "Check Now" (which showed the version) is "Game Version".
- Classic: the coop_menu Port Option ([display] coop_menu, "Co-op Menu"
  Modern / Classic, on Port Options and the OPTIONS tab). Classic draws the
  same rows (coopLobbyBuildRows) on the folder's paper as one list, each
  tab under a heading (Hosting / Rules, Options, Chat, Invite a Friend;
  coopLobbyClassicHeadings), with the old crew strip of photo cards and the
  next mission, the PREVIOUS tab, frontHintBar's prompts on the felt, and
  the character select in its paper inks (CS_INK). The layout macros
  follow coop_lobby_classic, so the scrolling, picking and stepping code is
  shared; L / R step values there as they used to. Changing the option on
  either look switches at once with the cursor kept on its row
  (coopLobbyLayoutTick).
- Left alone: the rules still can't be set before hosting (coopSetSetting
  takes only the host's, and a joiner's leftover copy of the last host's
  would show); the rows themselves and their order; the joiner's read-only
  GAME SETUP.
- Checked with two copies over the mock Steam (xdotool keys): every Modern
  tab as host, joiner and not hosting; Tab, Shift+Tab; All Options and back;
  the Co-op Menu row to Classic and back; Classic scrolled top to bottom
  hosting, joined and not hosting; the character select on Classic.
  Shots: menu-classic/ in the project files.
