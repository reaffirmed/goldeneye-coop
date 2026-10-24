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
