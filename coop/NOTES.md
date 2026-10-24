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
