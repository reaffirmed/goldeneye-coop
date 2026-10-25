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

Known gaps: grenades and mines thrown by the joiner don't hurt anything; cutscenes (intro and exit) only play on the host; guard tracers; vehicles (Runway plane, tank) run on both sides; and only the host can trigger level exits that check "Bond's" position.

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
- Not shared: ordinary weapon and armour pickups (each world has its own),
  guns a victim drops (only the scenario item), the per-PC Multiplayer
  Options in midas.ini. A player who drops out leaves a still body.
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
  runs the door-switch link and flips an alarm switch.
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
