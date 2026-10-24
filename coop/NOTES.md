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
