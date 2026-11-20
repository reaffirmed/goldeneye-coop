# Roadmap to 1.0

Status: proposal, written after v0.6.15 (2026-11-01). Nothing here is decided until the owner signs off. Items marked **Decision** need the owner's call.

## Where we are

v0.6.15 is feature complete for what the README promises. Story co-op for 2 to 4 players has host-authoritative sync, drop-in, host migration, revive and lives, own-character cutscenes, votes, spectate, voice, pings and a debrief. Versus has every arena and scenario online, the host's rules, leaving and migration, and end screens that agree on every PC. Around the game there is the updater, Steam and Discord presence, F8 reports and the accessibility options.

What is missing is mostly **proof**, not features. Most testing so far is the rig (two to four copies over the mock Steam on one machine) and the local two-player mission runs (`coop/ci/mission_runs.sh`), which join and finish every mission on Agent but don't play the objectives. The GitHub "Co-op mission runs" workflow skips them without the cart key and still reports success, so a green check there is not mission coverage. No checklist row yet records a pass by human players over real Steam. Half of the co-op test checklist has never been run, and three and four players have had far less play than two.

So 1.0 is mainly a testing and hardening release, plus one protocol change that should land before we promise compatibility.

## What 1.0 means

**1.0 is the build a group of friends can download, play the whole story together and have a versus night on, without meeting a known bug that breaks a mission or a match, and that keeps working with every other 1.x player.**

That splits into four bars. All four must be met.

### 1. Story co-op is done when

1. **Every checklist row has a result.** All 98 rows in [`coop/TEST-CHECKLIST.md`](../coop/TEST-CHECKLIST.md) read `pass`, or `fail` with the fix merged and the row passing again. A row the rig can't reach is played by hand. No row is left blank or `untestable`.
2. **Escorts are proven with real players.** Natalya (Bunker 2, Jungle, Control, Train, Archives) and the other escorts follow a joiner who leads, switch cleanly when their leader goes down or leaves, and do the same with 3 and 4 players (rows BU1, BU2, BU5, J1, J5, C1, C2, T2, A2, X7).
3. **A full campaign is finished by real players over real Steam.** All 20 missions, Dam to Egyptian, by two people on separate PCs, and every mission on 00 Agent in the rig with the joiner doing the objectives.
4. **Three and four players are tested as much as two.** At least one full 4-player session over Steam covering a mission from each act, and the escort and spawn rows (X7, X8) with 3 and 4.
5. **Host leaving and drop-in hold up in every mission.** Row X4 (host leaves mid-mission) and X3 (late join after doors and alarm) run on every mission, not just Dam.
6. **No open known issue breaks a mission or desyncs.** Cosmetic issues may ship if they are listed in the release notes.

### 2. Versus is done when

1. **Every scenario has been played to the end screen online**, including The Living Daylights (Flag Tag), which has never reached its end in a test.
2. **Players can pick their team in the lobby** for 2 v 2, 3 v 1 and 2 v 1. Today sides follow session order.
3. **Custom weapon sets work online**, so versus offers what local multiplayer does.
4. **3 and 4 player matches have been played by people over Steam**, not only in the rig, with someone leaving and the host leaving mid-match.

### 3. Compatibility is frozen when

1. **One last protocol bump.** Everything that needs a change to the host's hello packet lands in one `COOP_VERSION` bump before 1.0: team picks, weapon sets, and the cart fingerprint below. After 1.0, every 1.x build plays with every other 1.x build. A change that would break that waits for 2.0.
2. **Mismatched carts are turned away.** The lobby and the join handshake carry a fingerprint of the cart (and later of each active mod), and a mismatch gets the same "update needed" style refusal versions get today. Today the ROM loader only accepts the stock US cart, so this is safe now, but once Project Midas loads modded carts a mismatch would pass every check and desync. The owner has already said everyone must run the same mods and versions to play together.
3. **The updater gets everyone to 1.0.** Updating to the 1.0 zip is tested from v0.3.0, v0.6.6 (the last build before the v0.6.7 protocol change) and v0.6.15.

### 4. The release itself is ready when

1. **Release candidates soak.** 1.0 is tagged from a 0.9.x release candidate that has gone a week and at least three real play sessions without a new bug that breaks a mission or a match.
2. **The known issues list is honest.** Only cosmetic items remain, and the README, `README-COOP.txt` and the release notes agree.
3. **Licensing and provenance are settled** (see [Blockers today](#blockers-today), item 8).
4. **Housekeeping.** `CONTRIBUTING.md` and `CHANGELOG.md` still read as Project Midas's own files; they become this project's, with the Midas history kept as a link.

## Milestones

Each milestone is one or more releases. Fixes found along the way ship as point releases as they do now.

### 0.7: the protocol freeze

One `COOP_VERSION` bump carrying every hello-packet change, so 1.0 doesn't need another.

| Item | Why | Rough size |
| --- | --- | --- |
| Team picking in the versus lobby | Versus bar 2. Blocked today only because the two free `COOPSET` slots are too few. | 1 to 2 days |
| Custom weapon sets online (eight weapons) | Versus bar 3. Same blocker. | 1 day |
| Cart fingerprint in lobby data, the join handshake and the Public Games filter | Compatibility bar 2. The mod-hash half waits for Midas to publish its mod format. | 1 to 2 days |
| Drop-In row for the host: Open, Friends Only, Off | From the Rare memo; cheap to add while the hello packet is open. | Half a day |
| A mission-run gate that can't pass by skipping: `coop-missions.yml` reports neutral or failed when the cart key is missing on `main` and on tags, and the key is set up (or the runs are done locally with the log attached to the release PR) | So the protocol bump is actually regression-tested. Today the workflow has skipped every step in its last 100 runs and reported success. | Half a day |
| Room in the hello packet for later | A few reserved fields so 1.x can add host rows without a bump. | Small |
| Rig checks for every new row, versus sweep rerun, co-op regression | | 1 day |

**Exit:** v0.7.0 released, the versus sweep (`versus-test-checklist-results.md`) and the two-player mission runs pass (through the fixed gate, or locally with the log attached to the release PR), and older builds get the "update needed" panel.

### 0.8: the story test pass

Run everything the checklist hasn't, and fix what turns up.

| Item | Rough size |
| --- | --- |
| A rig test switch that gets Natalya to follow (the story set up to her escort), then rows BU2, J1, J5, C1, C2, T2, A2 | 1 day, plus hands-on runs for what the rig can't see |
| Joiner reaches story characters first: Janus and Valentin (ST2-ST4, ST6), Ourumov (SI3), Trevelyan (DE2, CA2, CR3), Jaws (AZ1), Baron Samedi (E2), the Bunker 1 technician (B2) | 1 day of rig runs |
| Deaths at bad moments: in a cutscene (X1), bleeding out as a countdown ends (SI5, T5), on the bungee platform (D7), falling off the Cradle (CR4) | Half a day |
| Drop-in during intros (D6, A1) and host migration on every mission (X4) | 1 day |
| The remaining action and effect rows: Frigate (FR2-FR4), Surface 1 (S1, S3), Bunker 1 (B3, B4), Streets (SR1, SR3), Train (T4, T6), Control (C3, C4), Cradle (CR1), Aztec (AZ3), Egyptian (E3), Facility (F3, F6, F7), Silo (SI6), Surface 2 (SU2), Runway (R3), Jungle (J3) | 1 to 2 days |
| Read Archives' level and NPC scripts, the only mission nobody has read through | Half a day |
| 3 and 4 player escort and spawn rows (X7, X8) | Half a day |
| Bunker 2: a joiner's punches missing the cell guard. Reproduce from the next F8 report or close it after the 0.9 sessions | Open-ended |
| Add a short versus run to CI (two copies, one match to the end screen), so versus has a regression check like co-op | Half a day |
| Turn compiler warnings back on for the co-op files (today the global `-w` hides every warning in them) and fix what shows up | Half a day to a day |
| Run the existing `GE_SANITIZE` build (ASan and UBSan) through the mission runs and the versus sweep; it isn't run anywhere today | Half a day, plus fixes |

**Exit:** every row in `coop/TEST-CHECKLIST.md` has a result, every `fail` is fixed and re-run, and the mission risk map has no "untested theory" left.

### 0.9: release candidates

Real people, real Steam, real distances. 0.9.x builds take fixes only.

| Item | Notes |
| --- | --- |
| Play sessions over Steam | Each one records its passes in the checklist as human, real Steam, so the checklist can tell them from rig passes. At least: a full 2-player campaign; a 4-player session across the acts; a session with a joiner on another continent; a versus night with every scenario, Flag Tag to the end, team picks, a 4-player match, someone quitting and the host leaving. |
| Updater path test | From v0.3.0, v0.6.6 and v0.6.15 to the candidate. |
| Read-only installs | **Decision:** move saves and settings to a per-user folder (with a one-time move of existing saves) for 1.0, or keep "unzip to a folder you own" as a documented 1.0 limitation. It is a known issue in every recent release's notes. |
| Linux | **Decision:** ship a Linux release zip alongside the Windows one, or say plainly that Linux players build from source. The README badge says Windows and Linux, but only `-win32.zip` is released. The blank-triangle workaround stays documented either way. |
| Licensing and provenance | See Blockers today, item 8. |
| Docs | README known issues, `README-COOP.txt`, `CONTRIBUTING.md`, `CHANGELOG.md`. |

**Exit:** a 0.9.x candidate goes a week and three sessions with no new mission- or match-breaking bug. That build, renumbered, is 1.0.0.

## Blockers today

What stands between v0.6.15 and the 1.0 bar, with where each is recorded.

1. **47 of 98 co-op checklist rows have never been run**, and a few more are `not run` or `untestable` (D4, B1 camera photo, DE3, ST5, J2). Source: `coop/TEST-CHECKLIST.md`.
2. **Escorts led by a joiner are untested in Jungle and Control**, and partly in Bunker 2 and Train. The rig can't yet get Natalya to follow; BU1 and BU5 now pass in the rig, J1, J5 and C1 are untestable there. Source: the checklist and the story polish thread.
3. **Three and four players are less tested than two.** Listed in the README's known issues.
4. **Flag Tag has never reached its end screen online.** The rig's 150 s runs were too short. Source: `versus-review/versus-test-checklist-results.md`, row 23.
5. **No team picking and no custom weapon sets online.** Both need the protocol bump in 0.7. Source: the versus review, items 11 and 12.
6. **No cart or mod check between players.** Safe today only because the loader accepts the stock US cart alone. Source: the mod support thread.
7. **Bunker 2 punches**, reported once, not reproduced in six rig runs. Source: v0.6.14 and v0.6.15 release notes.
8. **GoldenEye 007 Plus provenance.** The guard targeting state machine is ported from GoldenEye 007 Plus, which has no published license for its own changes, and `coop/NOTES.md` still says the fork was meant to stay private for that reason. Now that releases are public this needs settling before 1.0: written permission from its author, or a rewrite of the ported pieces listed in the NOTES provenance table. This stays an open owner decision. Either way `coop/NOTES.md` still says "this fork stays private", which is no longer true and should be corrected. Separately, no encrypted CI cart is published: there is no `ci-rom` release, draft or published (checked 2026-11-01). If one is added for CI, it must not be a public asset.
9. **Read-only installs and Linux packaging**, both waiting on a decision (see 0.9).
10. **The GitHub mission-run workflow can pass without running anything.** It skips every step without the cart key and reports success. Fix in 0.7.
11. **No real-Steam human passes on record.** Every checklist pass is from the rig or doesn't say. 0.9's sessions fill this in.
12. **Co-op code builds with every warning off, and the sanitizer build is never run.** Both are cheap ways to find bugs before players do. In 0.8.

Not blockers: the Facility "objective B failed" flash (not seen in the 2026-10-31 recheck), the versus death drop logging as "host guard 30000" (log only), and the per-bullet dust puff events (working, could be trimmed).

## After 1.0

Explicitly not part of 1.0. Nothing here should hold the release, and none of it may break 1.x compatibility.

**1.x, small and on systems that exist** (mostly from the "If Rare shipped" memo, in its suggested order):

- Down-rule presets (Partners, Licensed, Classic, Custom) so a first-time host isn't faced with eight rows. Lobby only, no protocol change.
- Versus playlists and saved host rule sets, and an Oddjob Allowed or Banned row.
- Co-op debrief awards in Rare's versus style.
- DualSense: lightbar in your player colour, adaptive triggers, directional haptics, optional gyro aim.
- Quick Play into the best public game.
- Richer presence ("Dam, 00 Agent, 3 of 5 objectives").
- Steam Deck pass: glyphs, menus at 1280x800, larger default text.
- Accessibility, second tier: high contrast, aim assist strength, opt-in objective guide.
- Local medals and a service record (no stat unlocks).
- The cutscene star is whoever did the deed.
- Mod list in the fingerprint, once Project Midas publishes its mod format.
- Trimming the per-bullet dust puff events.

**Later, bigger jobs:**

- **Smarter guards.** Paused by the owner until the core game is done (v0.6.1, `COOP_SMARTER_AI` off; draft PR #64). After 1.0 it comes back as an opt-in with 007 mode; Rare's guards stay the default.
- Simulants (bots) for versus.
- Counter-Operative: a player takes over a guard.
- Co-op objective variants on 00 Agent for a few missions.
- Sending mods to joiners automatically (also a licensing question for ROM-hack mods).
- A 64-bit build, needed only for a Mac or ARM version; 4 to 8 weeks of work and an updater hand-off from the `-win32` zip.

**Not planned:**

- The XBLA remastered textures as a shipped option. That art isn't the player's own cart, so a fan release can't distribute it.
- Steam achievements or our own Steam app. The game runs as Valve's Spacewar test app.
- Regenerating health, loadouts, perks, killstreaks, kill cams, or a permanent team HUD. Versus already lost its hit marker and hit direction arc in v0.6.15 to keep the 1997 HUD.
- Play between players with different mods or versions.

## How this document is kept

Update it when a milestone ships or a decision is made: tick items off by moving them into the release notes, and keep the Blockers list current. The detailed test results stay in `coop/TEST-CHECKLIST.md` and the design history in `coop/NOTES.md`.
