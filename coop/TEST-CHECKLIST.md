# Co-op test checklist: the joiner does everything

The mission scripts were written for one Bond. Every bug found in two-player
testing so far comes from a script, effect or check that only looks at one
player. This checklist runs each mission with the **joiner** doing the work
while the host stays back, so any place that still only sees the host shows up.

How to use it: play each row (or run its rig line), then set **Result** to one
of four statuses, with the build, the environment and where the evidence is:

- **PASS**: run on that build and every "both screens" item held.
- **FAIL**: run and something didn't; the note says what, and the task that tracks it.
- **BLOCKED**: can't be run in that environment yet; the note says what is missing.
- **NOT RUN**: not run on that build (no rig line yet, or queued); the note says which.

A result from an older build is kept after "Earlier:" and never counts for the
current one. "Both screens" means host and joiner must agree. A row that only
shows on the joiner's screen while the host's script moved on (or the other way
round) is a fail.

## Bug families

| Code | Family | What goes wrong |
| --- | --- | --- |
| SPAWN | One-time spawn blocked by line of sight | A story character spawns once, only where "Bond" can't see. A player watching the spot makes it fail for good. |
| TARGET | "Bond" switches between players | A follower or boss told to face, follow or wait for Bond flips between players, wanders, or turns on the joiner. |
| ACTION | Host doesn't register the joiner's action | The joiner shoots, hits, uses or breaks something; the host's script never hears of it, or counts it without the scripted reaction. |
| FX | Effect only on one side | An explosion, fade, teleport or animation plays on the host only (or on the joiner only). |
| HURT | Joiner-side hit feedback differs | The joiner shows a flinch, blood or death the host didn't. |
| ONEPLAYER | Pass or fail check on one player | "Bond in room", "Bond on screen", "Bond near pad" only looks at the host, so an ending or failure starts for one player. |
| SCRIPTDEATH | Death or respawn during a scripted moment | A player dies mid-cutscene, mid-countdown or while being followed; the script loses its target or ends with one player dead. |
| SAMETIME | Both players trigger it at once | Two charges on one tank, both at a console, both at a switch in one frame: counters double up or skip. |
| KEYITEM | Joiner holds the key item | The script asks "does Bond have X". A joiner-held item doesn't satisfy it, or the joiner can't use the host's. |
| SPLITOBJ | Objective state differs | An objective ticks on one screen and not the other, or the results screens disagree. |
| LATEJOIN | Alarm and door state after drop-in or host migration | A late joiner or new host has alarms off, doors shut, guards calm while the host's are alerted. |
| CUTJOIN | Drop-in during a cutscene | The joiner arrives in a frozen scene or past a one-time spawn. |
| BLEEDOUT | Bleed-out on a timed level | Bleeding out as the countdown ends fails twice or shows the wrong screen. |
| MANY | 3 and 4 players | More "not Bond" bodies in view; everything above can get worse. |

## Setup for every row

- Two copies on one LAN or Steam, host on Agent unless the row says otherwise.
- The host waits at the start (or where the row says) and does **not** look at
  the spot the joiner is working on, unless the row is about that.
- Check both screens and both logs (`ge_direct.log`): the host log should show
  the objective lines (`objective N (X) is now complete/failed`) and the
  joiner log the same lines a moment later.

Rig lines use the test rig in `coop/ci/checklist/` (see its README), which drives
the players with `GE_COOP_STEPS` and the `GE_COOP_TEST*` switches. A name in
backticks in the **Rig check** column is a row in `coop/ci/checklist/rows.py`
(`python3 coop/ci/checklist/rows.py out D2` runs it and judges it from the logs).

**Current results: `main` at 9883333 (after the 1.0 merge batch), 2026-11-02.** Environment
"rig": the Linux i686 build, two to four copies on one machine over the mock Steam
(`coop/mock`), US cart, software GL, both players invulnerable unless a row is about
dying. It is not real Steam and not the internet: a PASS here says the game logic
agrees, not how it plays across real links. Rows marked "campaign" were run by the
campaign workstream on the same build and rig (logs in the project files,
`goldeneye-coop/campaign/evidence-0d88b92/main-9883333-runs.tgz`; the folder name is
older than the build). QA's logs are under `goldeneye-coop/qa/evidence-9883333/`. Five
rows (SI4, DE3, E3, X3b, X4b) need the QA test switches (`GE_COOP_WATCHDOORS`,
`GE_COOP_TESTOPEN`, step `e`, the countdown clock stamp, and the join-snapshot door log);
they say "9883333 with the QA test switches", and those switches change nothing unless set.
Where a row ran on a later commit whose game code is 9883333's plus only the harness
(22b22dc, 1a6bbd6, a16f87c, cba6ffb), the row names the commits.
The campaign rows re-run after the merge batch are in
`goldeneye-coop/campaign/evidence-0d88b92/main-9883333-qa14.tgz`.
Also on 9883333: CI `mission_runs.sh` 20/20 (lossy link), its three late joins 3/3,
`joiner_spots.sh` 4/4, every `rows.py` line, and the objective sweep 18/18.

Tally on 9883333, 101 rows: 70 PASS, 2 FAIL (AZ4, the joiner's launch skips the
countdown, QA-7, fix in PR #123; X9, the split after a 10 s cut, fix in PR #116),
3 BLOCKED (A2, T2, T5), 26 NOT RUN. "Earlier" with no build is the
bug-fix branch on 2026-10-29 and 2026-10-30 (heads 620f526 to 93ea314).

**Fixed: intermittent desync.** In joiner-heavy runs where a blast kills guards
(Caverns, Jungle, Depot), the joiner sometimes logged "repl: unresolved desync: the
chrs here are not the host's". The bug-fix branch fixed it in 7dfb28e and c4237cd. On
c4237cd, 29 pair runs (the sweep, plus Jungle, Caverns and Depot run 4 times each) had
none.

**Fixed: Jungle's respawned guards.** Jungle's own respawner (vanilla script) spawns a new
guard when guard 29, 30, 34, 39, 40 or 41 is gone, and gives it the dead guard's number.
The joiner reused the old, dying copy for it. 118ab73 on the bug-fix branch makes the
joiner spawn a fresh copy instead. In 4 Jungle pairs, the respawned guard's head and spot
matched the host's. It is the same in solo: a host on its own never had a guard leave
ACT_DIE (`solo.sh`, `GE_COOP_REVIVE=1`).

---

## 1. Dam

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| D1 | ONEPLAYER, SPLITOBJ | Walks into the bungee room alone, jumps | Bungee objective completes on both; both get the ending | `D1` | **PASS** (rig, 9883333, `rows.py D1` and CI `joiner_spots.sh bungee`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py D1`); also CI `joiner_spots.sh bungee`: A complete, one mission end and a debrief on each. Earlier: pass (rig, 2 and 3 players; failed on 620f526, fixed in 74d3da9). |
| D2 | ACTION | Shoots out every alarm box (00 Agent) | "Neutralize all alarms" completes on both | `D2` | **PASS** (rig, 9883333, `rows.py D2`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py D2`), 00 Agent: A completes on both, credit player 2; same in `SW-dam`. Earlier: pass (rig: joiner breaks all 4 alarms, 00 Agent). |
| D3 | KEYITEM | Picks up and places the covert modem on the satellite link (SA+) | Objective completes on both; the host's inventory no longer shows a modem to place | `D3` | **NOT RUN** on 9883333: `rows.py D3` throws the modem at the link, but it never attaches and B fails, the host alone the same (`D3-solo`). No rig line attaches it yet. Earlier, 0d88b92: not run on 0d88b92: no rig line places the modem on the link yet. Earlier: fail, fixed (rig, 2026-10-30): a modem the joiner threw short fell through the level on the host, so B failed only on the joiner (now on both, as in solo); a modem on the link was not re-run. |
| D4 | ACTION | Uses the mainframe for the data backup (00 Agent) | Completes on both | `D4` | **NOT RUN** on 9883333: `rows.py D4` and `D4-solo` still fail the same way, the host alone too. Earlier, 0d88b92: not run on 0d88b92: the rig line (`rows.py D4`: modem flag set, the joiner uses both mainframes; the host logs both uses) never starts the backup, and neither does the host alone (`D4-solo`), so the line misses a step of the script. Earlier: not run (rig, 2026-10-30): guard fire in the rig destroyed both mainframes first, with the host doing it too. |
| D5 | LATEJOIN | Joins late after the host raised the alarm | Alarm is sounding on the joiner, guards alerted | `D5` | **PASS** (rig, 9883333, `rows.py D5`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py D5`): host raises the alarm at 15 s, the joiner drops in after it and its first world state has the alarm on and stage flag 0x40000000. Earlier: pass (rig: drop-in at 35 s sees the alarm on). |
| D6 | CUTJOIN | Joins during the opening swirl | Joiner spawns into play, not a frozen camera | `D6` | **PASS** (rig, 9883333, `rows.py D6`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py D6`): host watches its whole intro (no skip), the joiner arrives during it, ends the intro with the host ("the host's intro of stage 33 is over") and plays (its step at 20 s runs). |
| D7 | SCRIPTDEATH | Dies on the bungee platform while the host is far away | Respawns beside the host; jump still possible | | **PASS** (rig, 9883333, campaign, `campaign/evidence-0d88b92/main-9883333-qa14.tgz q_d7b`): passed on the re-run: in q_d7 Dam's body armour absorbed the one death step, so q_d7b uses two. Earlier, 0d88b92: pass (rig, 0d88b92, campaign run, logs `campaign/evidence-0d88b92/row-runs-2.tgz d7`). |

## 2. Facility

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| F1 | SPAWN | Reaches Doak's lab first, looking at his spawn spot | Doak appears on both screens; "Contact 006/Doak" objective not failed | `doak` | **PASS** (rig, 9883333, campaign, `campaign/evidence-0d88b92/main-9883333-runs.tgz m_f1`). Earlier, 0d88b92: pass (rig, 0d88b92, campaign run, logs `campaign/evidence-0d88b92/row-runs.tgz doak`): Doak there for the joiner, B completes on both, credit player 2. Earlier: pass (rig, 2 and 3 players). |
| F2 | SPLITOBJ | Meets Doak first (the known "objective B failed" flash) | No "failed" message on either screen, even briefly | `doak` | **PASS** (rig, 9883333, campaign, `campaign/evidence-0d88b92/main-9883333-qa14.tgz q_f2`). Earlier, 0d88b92: pass (rig, 0d88b92, campaign `doak` logs read by QA): no "failed" line for any objective on either side. Earlier: pass (rig: no failed flash). |
| F3 | KEYITEM | Talks to Doak, then opens the door that needs the decoder | Host receives the decoder (script gives it "to the team"); the joiner can still open the door | | **NOT RUN** on 9883333: the campaign run reached the decoder pickup but not the door (`row-runs.tgz f13`). |
| F4 | ACTION, FX | Plants charges on the bottling-room tanks | Tanks blow on both; "Destroy all tanks" completes on both | | **PASS** (rig, 9883333, objective sweep `rows.py SW-facility`: every joiner step ran, the same objective states on both, no unresolved desync). Earlier, 0d88b92: pass (rig, 0d88b92, objective sweep `rows.py SW-facility`: the joiner breaks all 10 tanks, D completes on both, credit player 2). Earlier: pass (rig: joiner shoots all 10 tanks, objective on both). |
| F5 | SAMETIME | Both players plant on the same tank in the same second | Counted once; tank blows once | `F5` | **PASS** (rig, 9883333, `rows.py F5`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py F5`): both break tanks 1 and 2 in the same second; each destroyed once (level 1) on both. Earlier: pass (rig, 2026-10-30): both blow tanks 1 and 2 in the same second; each destroyed once, the same on both. |
| F6 | ONEPLAYER | Reaches the bottling room first; its alarm/timer fires on the joiner's entry | Same timer on both screens | | **NOT RUN** on 9883333: no rig line yet. |
| F7 | FX | Watches Natalya/the scientist room while the host is elsewhere; guards that hide "when not on screen" vanish | No chr vanishes in front of the joiner | | **NOT RUN** on 9883333: no rig line yet. |
| F8 | ONEPLAYER | Reaches the exit (Ourumov ending) first | Ending starts for both, once | `F8` | **PASS** (rig, 9883333, `rows.py F8`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py F8`): the joiner on pad 309; one mission end on each. Earlier: pass (rig, 2026-10-30): the joiner on the exit pad (309) starts the ending; one mission end on each. |

## 3. Runway

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| R1 | KEYITEM | Takes the plane key from the hut | Host's script sees the key collected; plane objective works for whoever boards | `R1` | **PASS** (rig, 9883333, `rows.py R1`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py R1`): A completes on both; same in `SW-runway`. Earlier: pass (rig). |
| R2 | ACTION, FX | Blows up the heavy gun emplacements | Destroyed on both, objective on both | | **PASS** (rig, 9883333, objective sweep `rows.py SW-runway`: every joiner step ran, the same objective states on both, no unresolved desync). Earlier, 0d88b92: pass (rig, 0d88b92, objective sweep `rows.py SW-runway`: B and C complete on both, credit player 2). Earlier: pass (rig). |
| R3 | ONEPLAYER | Gets in the plane first | Ending for both | `R3` | **PASS** (rig, 9883333, `rows.py R3`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py R3`): the joiner takes the key and uses the plane (tag 3); one mission end on each, all objectives complete. |

## 4. Surface 1

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| S1 | ACTION | Shoots the power supply / comms dish | Objective on both | | **NOT RUN** on 9883333: no destroy target in the setup for it; no rig line yet. |
| S2 | KEYITEM | Takes the bunker key from the guard's body | Host can open the bunker door too | | **PASS** (rig, 9883333, objective sweep `rows.py SW-surface`: every joiner step ran, the same objective states on both, no unresolved desync). Earlier, 0d88b92: pass (rig, 0d88b92, objective sweep `rows.py SW-surface`: B and C collected by the joiner, complete on both). Earlier: pass (rig: keys collected, objectives on both). |
| S3 | ONEPLAYER | Enters the bunker vent first | Mission ends for both | `S3` | **PASS** (rig, 9883333, `rows.py S3`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py S3`): the joiner in the room of pad 46; one mission end on each. |

## 5. Bunker 1

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| B1 | ACTION, KEYITEM | Uses the camera on the GoldenEye / photo of the main room (gadget-on-object checks) | Objective on both | `bunker1Gadget` | **NOT RUN** on 9883333: the camera photo has no rig line; the destroy objective passes (`SW-bunker1`). Earlier: pass for the destroy objective (rig); camera photo not run. |
| B2 | TARGET | Approaches the technician (the unreproduced tester report) | Technician reacts to the joiner, not to the far host | | **NOT RUN** on 9883333: queued with the campaign workstream (CAMP). |
| B3 | ACTION | Shoots the security cameras | Disabled on both | | **PASS** (rig, 9883333, objective sweep `rows.py SW-bunker1`: every joiner step ran, the same objective states on both, no unresolved desync). Earlier, 0d88b92: pass (rig, 0d88b92, objective sweep `rows.py SW-bunker1`: the joiner breaks all four cameras, A completes on both). |
| B4 | FX | Stands in the six-door gate (the door storm fixed in fixes-1) | Gate opens once; no network flood | | **NOT RUN** on 9883333: no rig line yet. |

## 6. Silo

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| SI1 | ACTION, FX | Plants the charges in the fuel rooms | Charges blow on both | `SI1` | **NOT RUN** on 9883333: `rows.py SI1` gets three of the four charges the joiner plants to the host ("peer planted objective item 0, 1, 3"), but the charge for pad 88 never counts for its room in the rig (from pad 89 or looking down, neither), so A never completes, and the blasts were not checked. Earlier, 0d88b92: not run on 0d88b92: not re-run. Earlier: pass (rig, 2026-10-30): each charge the joiner plants reaches the host ("peer planted objective item"); 3 of 4 rooms in one run, the 4th throw rolls out of its room for the host too. |
| SI2 | KEYITEM | Picks up the circuit board / satellite plans | Objective on both | | **PASS** (rig, 9883333, objective sweep `rows.py SW-silo`: every joiner step ran, the same objective states on both, no unresolved desync). Earlier, 0d88b92: pass (rig, 0d88b92, objective sweep `rows.py SW-silo`: C and D complete on both, credit player 2). Earlier: pass (rig, 2026-10-30): C and D complete on both, credit to the joiner. |
| SI3 | SPAWN, TARGET | Reaches Ourumov first, looking at his spawn | Ourumov appears and runs for both; drops his key for both | | **PASS** (rig, 9883333, campaign, `main-9883333-runs.tgz m_si3`). Earlier, 0d88b92: pass (rig, 0d88b92, campaign run, logs `campaign/evidence-0d88b92/row-runs.tgz silo_ourumov, si3host`). |
| SI4 | ONEPLAYER | Starts the escape countdown | Same countdown on both screens | `SI4` | **PASS** (rig, 9883333 with the QA test switches, `rows.py SI4`): the joiner plants a charge, the countdown starts on the host, and the joiner's is within 0.5 s of the host's at the same moment (both logs stamped with the machine clock). Earlier, 0d88b92: not run on 0d88b92: not re-run (the countdown starts when a charge settles; no joiner rig line). Earlier: pass (rig, 2026-10-30): 00 Agent, host 509.6 s and joiner 509.9 s. |
| SI5 | BLEEDOUT | Is down and bleeding out as the countdown ends | Mission fails once, one failure screen on each | | **PASS** (rig, 9883333, campaign, `campaign/evidence-0d88b92/main-9883333-qa14.tgz q_si5`). Earlier, 0d88b92: pass (rig, 0d88b92, campaign run, logs `campaign/evidence-0d88b92/row-runs.tgz si5, si5b`). |
| SI6 | FX | Countdown runs out | Explosions around both players, not just the host | | **NOT RUN** on 9883333: no rig line yet. |

## 7. Frigate

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| FR1 | ONEPLAYER | Enters a hostage room first | Hostage-taker reacts to the joiner; hostages live on both | `hostage` | **PASS** (rig, 9883333, CI `joiner_spots.sh hostage`). Earlier, 0d88b92: pass (rig, 0d88b92, CI `joiner_spots.sh hostage`): guard 1 turns on player 2. Earlier: pass (rig `hostage`). |
| FR2 | ACTION | Shoots a hostage-taker | Dead on both; hostage freed on both | `FR2` | **PASS** (rig, 9883333, `rows.py FR2` and `FR2-solo`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py FR2`) and `FR2-solo`: hostage-taker 1 dies on both; hostage 44 lives, the same as the host alone. |
| FR3 | FX | A hostage-taker throws a grenade at the joiner | Grenade and blast on both | | **NOT RUN** on 9883333: the rig cannot make a hostage-taker throw. |
| FR4 | KEYITEM, ACTION | Plants the tracker bug on the helicopter | Objective on both | `FR4` | **PASS** (rig, 9883333, `rows.py FR4`): the joiner's tracker bug on the helicopter completes the bug objective on both. Earlier, 0d88b92: not run on 0d88b92: no rig line for the tracker throw yet. |
| FR5 | ONEPLAYER | Reaches the boat first | Ending for both (fixed in fixes-1) | `FR5` | **PASS** (rig, 9883333, `rows.py FR5`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py FR5`): both step off the boat, the joiner comes back on pad 153; one mission end on each (`FR5-hoststays`, host never leaving the start, the same). Earlier: pass (fixed in fixes-1). |

## 8. Surface 2

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| SU1 | ACTION | Destroys the communications dish / power | Objective on both | | **PASS** (rig, 9883333, objective sweep `rows.py SW-surface2`: every joiner step ran, the same objective states on both, no unresolved desync). Earlier, 0d88b92: pass (rig, 0d88b92, objective sweep `rows.py SW-surface2`: A and B complete on both, credit player 2). Earlier: pass (rig: A and B destroy objectives). |
| SU2 | SPAWN | Watches the reinforcement spawn points | Reinforcements arrive the same on both (Surface 2 spawn fix, #63) | | **NOT RUN** on 9883333: no rig line yet. |
| SU3 | ONEPLAYER | Enters the bunker first | Ends for both | `SU3` | **PASS** (rig, 9883333, `rows.py SU3`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py SU3`): the joiner on pad 3; the room objective completes on both and one mission end on each. Earlier: pass (rig: joiner enters the objective room, C/D on both). |

## 9. Bunker 2

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| BU1 | TARGET | Leads Natalya out of the cell, the host far behind | She follows the joiner, doesn't hesitate between players | `BU1` | **PASS** (rig, 9883333, `rows.py BU1`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py BU1`): host far off on chr 27, the joiner opens her cell; she goes to player 2 and never to player 1. Earlier: pass (rig: host `c27@8`, joiner `GE_COOP_TESTDOOR=1` `o3@8,c13@18,c12@30`; she goes to player 2 and follows them to rooms 26 and 27). |
| BU2 | FX, ONEPLAYER | Looks back at Natalya while she's out of the host's view | She doesn't teleport in front of the joiner | `bunker2Nat` | **PASS** (rig, 9883333, campaign, `campaign/evidence-0d88b92/main-9883333-qa14.tgz q_bu2`): run on its own with a longer run, since three games at once starve the rig. Earlier, 0d88b92: pass (rig, 0d88b92, campaign run, logs `campaign/evidence-0d88b92/row-runs.tgz bu2`). |
| BU3 | KEYITEM | Picks up the GoldenEye key / Boris's clipboard | Collected check works for the host's script | `bunker2Items` | **PASS** (rig, 9883333, objective sweep `rows.py SW-bunker2`: every joiner step ran, the same objective states on both, no unresolved desync). Earlier, 0d88b92: pass (rig, 0d88b92, objective sweep `rows.py SW-bunker2`: B, C and D complete on both, credit player 2). Earlier: pass (rig: B and D collect objectives on both). |
| BU4 | ACTION | Stands in front of a drone gun | The host's drone gun fires at the joiner | `dronegun` | **PASS** (rig, 9883333, CI `joiner_spots.sh dronegun`). Earlier, 0d88b92: pass (rig, 0d88b92, CI `joiner_spots.sh dronegun`). Earlier: pass (rig `dronegun`). |
| BU5 | SCRIPTDEATH | Dies while Natalya follows the joiner | She switches to the host (or waits), doesn't freeze | `BU5` | **PASS** (rig, 9883333, `rows.py BU5`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py BU5`) (PAR=1 SECSX=4): she goes to player 2, then to player 1 right after "player 2 down". Earlier: pass (rig: as BU1 then `d@40`; "chr 0 goes to player 1" as the joiner goes down; the guards there then killed her). |

## 10. Statue

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| ST1 | SPAWN | Host waits at the helicopter when Janus's timer starts | Natalya spawns, objective not failed (fixed in fixes-2) | `natalya` | **PASS** (rig, 9883333, CI `joiner_spots.sh natalya` and `rows.py X8-nat4`). Earlier, 0d88b92: pass (rig, 0d88b92, CI `joiner_spots.sh natalya` and `rows.py X8-nat4` with 4 players): D completes. Earlier: pass (rig `natalya`, 2 and 4 players). |
| ST2 | TARGET | Talks to Janus with the host nearby | Janus stays facing one player (fixed in fixes-2) | | **PASS** (rig, 9883333, campaign, `main-9883333-runs.tgz m_st2`): Janus faces one player; A and B complete on both. The 3 s line-cut case that failed on 0d88b92 (fixed by PR #114) was not re-run here. Earlier, 0d88b92: fail (rig, 0d88b92, campaign, `campaign/evidence-0d88b92/lost-partner-runs.tgz`): when the joiner's line goes quiet 3 s or more during the talk, Valentin contact fails on the host while the joiner shows it complete (3/3 with a 6 s cut). With PR #114 (CAMP-7, in review): PASS, 5/5 cut runs and `row-runs-2.tgz st2f`. |
| ST3 | ACTION | Hits Janus | Mission reacts as for the host's hit (fixed in fixes-2) | | **PASS** (rig, 9883333, campaign, `campaign/evidence-0d88b92/main-9883333-qa14.tgz q_st3`): run on its own with a longer run. Earlier, 0d88b92: pass (rig, 0d88b92, campaign run, logs `campaign/evidence-0d88b92/row-runs.tgz st3`). |
| ST4 | ONEPLAYER | Walks up to Janus | Guards attack and the "too close" fail happens for the joiner too | | **NOT RUN** on 9883333: the campaign run (`main-9883333-runs.tgz m_st4j` vs `m_st4h`) walked onto Janus early and nothing fired for either player, the host alone the same (only A completes), so the "too close" fail was never staged. Earlier, 0d88b92: fail (rig, 0d88b92, campaign): fails with a 6 s line cut during the meeting, as ST2. With PR #114 (CAMP-7, in review): matches the host alone (`row-runs-2.tgz st4j` vs `st4h`). |
| ST5 | KEYITEM | Picks up the flight recorder | Objective on both | `ST5` | **PASS** (rig, 9883333, `rows.py ST5`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py ST5`): the joiner takes the recorder without walking into the exit room; E completes on both. Earlier: untestable in rig: the recorder sits in the exit room, which ends the mission (as in solo). |
| ST6 | TARGET | Meets Valentin with a gun out | Valentin's "put your gun away" reacts to the joiner | | **PASS** (rig, 9883333, campaign, `main-9883333-runs.tgz m_st6j` vs `m_st6h2`; the first host run, `m_st6h`, timed out). Earlier, 0d88b92: not run on 0d88b92 alone: Valentin's line needs PR #114, a host-only Statue change. With PR #114 on 0d88b92 it passes (rig, campaign, `row-runs-2.tgz st6j2` vs `st6h`). |

## 11. Archives

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| A1 | CUTJOIN | Joins during the interrogation intro | Joiner starts in play | `A1` | **PASS** (rig, 9883333, `rows.py A1`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py A1`): the joiner arrives in the host's intro, ends it with the host and plays; A completes on both. |
| A2 | TARGET | Rescues Natalya | She follows the joiner | | **BLOCKED** on 9883333: as on 0d88b92, the rig cannot stage it. Earlier, 0d88b92: blocked on 0d88b92: the rig cannot stage the rescue (campaign, `row-runs.tgz a2`). |
| A3 | ONEPLAYER | Reaches the exit window first | Ends for both | `A3` | **PASS** (rig, 9883333, `rows.py A3`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py A3`): A completes on both. Earlier: pass for the pad 152 room objective (rig). |

## 12. Streets

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| SR1 | ONEPLAYER | Drives the tank (vehicles run on both sides) | "Bond in tank" checks work for the joiner's tank | | **NOT RUN** on 9883333: no rig line yet. |
| SR2 | ACTION | Meets Valentin / reaches the exit | Ends for both | `SR2` | **PASS** (rig, 9883333, `rows.py SR2`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py SR2`): the joiner on pad 248; one mission end on each. Jungle (`J-exit`, pad 468) and Control (`C-exit`, pad 168) the same. Earlier: pass for the exit (rig, 2026-10-30): the joiner on pad 248; one mission end on each. Jungle (468) and Control (168) exits the same. |
| SR3 | FX | Civilians appear and disappear (Streets reaper fix) | No flicker on the joiner | | **NOT RUN** on 9883333: no rig line yet. |

## 13. Depot

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| DE1 | ACTION, FX | Destroys the helicopters and weapon caches | Destroyed on both | | **PASS** (rig, 9883333, objective sweep `rows.py SW-depot`: every joiner step ran, the same objective states on both, no unresolved desync). Earlier, 0d88b92: pass (rig, 0d88b92, objective sweep `rows.py SW-depot`: all 17 caches; A completes on both; 7 desyncs corrected, none unresolved). Earlier: pass (rig: 17 caches); see the desync note. |
| DE2 | SPAWN | Reaches Trevelyan's spawn first, watching it | Trevelyan appears for both | | **NOT RUN** on 9883333, not applicable as written: Depot has no Trevelyan (campaign). The row needs rewriting to a scripted spawn Depot has. Earlier, 0d88b92: not run , not applicable as written: Depot has no Trevelyan (campaign, 0d88b92 with PR #117). The row needs rewriting to a scripted spawn Depot has. |
| DE3 | KEYITEM | Picks up the safe key and opens the safe | Works for the joiner, objective on both | `DE3` | **PASS** (rig, 9883333 with the QA test switches, `rows.py DE3`, Secret Agent): the joiner takes the safe key (complete on both), its Use opens the safe door on the host, and the blueprints complete on both. Earlier, 0d88b92: not run on 0d88b92: no rig line yet. Earlier: not run. |
| DE4 | ONEPLAYER | Boards the train first | Ending for both | `DE4` | **PASS** (rig, 9883333, `rows.py DE4`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py DE4`): the joiner on pad 271; one mission end on each. Earlier: pass (rig, 2026-10-30): the joiner reaches the train pad; the ending runs for both. |

## 14. Train

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| T1 | ACTION | Shoots guard 4 (the known "objective B failed" flash) | No failed flash on the joiner | `T1` | **PASS** (rig, 9883333, `rows.py T1` and `T1-solo`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py T1`) and `T1-solo`: no failed flash on the joiner; B did not fail at Agent on either side, the same as the host alone. Earlier: pass (rig): B fails on both, the same as when the host does it. |
| T2 | TARGET, FX | Frees Natalya and watches her while the host is out of view | No catch-up teleport in front of the joiner | | **BLOCKED** on 9883333: as on 0d88b92, the rig cannot stage it. Earlier, 0d88b92: blocked on 0d88b92: the rig cannot stage her rescue (campaign, `row-runs.tgz t2`). |
| T3 | ONEPLAYER | Starts the countdown from the joiner's side (the countdown hand-off) | Same countdown on both, from the host | `trainClock` | **NOT RUN** on 9883333: not re-run. Earlier: pass (rig: countdown values match on both). |
| T4 | FX | Countdown ends / the floor is cut with the watch | Explosions on both (fixed in fixes-2) | | **NOT RUN** on 9883333: no rig line yet. |
| T5 | BLEEDOUT | Is down as the countdown runs out | One failure, one screen | | **BLOCKED** on 9883333: as on 0d88b92, the countdown cannot be staged in the rig. Earlier, 0d88b92: blocked on 0d88b92: the countdown cannot be staged in the rig (campaign, `row-runs-2.tgz t5base`); needs a real run to the last car. |
| T6 | KEYITEM | Uses the watch laser on the floor | The cut works for the joiner | | **NOT RUN** on 9883333: no rig line yet. |

## 15. Jungle

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| J1 | TARGET | Walks with Natalya | She follows, doesn't attack the joiner (fixed in fixes-2) | | **PASS** (rig, 9883333, campaign, `campaign/evidence-0d88b92/main-9883333-qa14.tgz q_j1`). Earlier, 0d88b92: pass (rig, 0d88b92, campaign run, logs `campaign/evidence-0d88b92/row-runs.tgz j1, j1b..j1d`). Earlier: untestable in rig (as BU1). |
| J2 | ONEPLAYER | Meets Xenia first, the host far away | Xenia's intro (text and music) plays when the joiner sees her, not when the host does | `xenia` | **NOT RUN** on 9883333: queued with the campaign workstream (CAMP). Earlier: untestable in rig (players can see her spot from the start); needs a hands-on run. |
| J3 | ACTION | Kills Xenia | Objective on both | | **PASS** (rig, 9883333, campaign, `campaign/evidence-0d88b92/main-9883333-qa14.tgz q_j3`): credit goes to player 2 as expected. Earlier, 0d88b92: pass (rig, 0d88b92, campaign, `row-runs-2.tgz j3`): B completes on both. Separate defect: the kill credit goes to the host, because Xenia takes about 7 s to die and the credit window is 5 s (CAMP-8). |
| J4 | ACTION | Destroys the drone guns | Objective on both | | **PASS** (rig, 9883333, objective sweep `rows.py SW-jungle`: every joiner step ran, the same objective states on both, no unresolved desync). Earlier, 0d88b92: pass (rig, 0d88b92, objective sweep `rows.py SW-jungle`: A and C complete on both, credit player 2; 4 desyncs corrected). Earlier: pass (rig: drone guns A and C on both); see the desync note. |
| J5 | SCRIPTDEATH | Dies on the bridge while Natalya follows | She keeps going with the host | | **PASS** (rig, 9883333, campaign, `campaign/evidence-0d88b92/main-9883333-qa14.tgz q_j5b`): passed on the re-run: in q_j5 Natalya died in crossfire from guards 27 and 2 before the death step, the same as solo, not a co-op fault. Earlier, 0d88b92: pass (rig, 0d88b92, campaign run, logs `campaign/evidence-0d88b92/row-runs.tgz j5`). Earlier: untestable in rig (as BU1). |

## 16. Control

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| C1 | TARGET | Escorts Natalya to the console | She stays at the console while the joiner guards her | `controlNat` | **PASS** (rig, 9883333, campaign, `campaign/evidence-0d88b92/main-9883333-qa14.tgz q_c12`). Earlier, 0d88b92: pass (rig, 0d88b92, campaign run, logs `campaign/evidence-0d88b92/row-runs.tgz c12`). Earlier: untestable in rig (as BU1). |
| C2 | FX, ONEPLAYER | Watches Natalya while the host is out of view | No catch-up teleport in front of the joiner | `controlNat` | **PASS** (rig, 9883333, campaign, `campaign/evidence-0d88b92/main-9883333-qa14.tgz q_c12`). Earlier, 0d88b92: pass (rig, 0d88b92, campaign run, logs `campaign/evidence-0d88b92/row-runs.tgz c12`). |
| C3 | ACTION | Breaks the console Natalya is using | Mission fails on both, as in solo | | **PASS** (rig, 9883333, campaign, `main-9883333-runs.tgz m_c3j` vs `m_c3h`): B fails and the mission ends on both, as with the host alone. Earlier, 0d88b92: not run on 0d88b92 alone: the run was on 0d88b92 with PR #117. With #117 it passes (rig, campaign, `row-runs-3.tgz c3j2` vs `c3h2`): when either player breaks console 37 (Natalya's), B fails and the mission ends without completing on both, the same as the host alone; consoles 35 and 36 (`c3j`, `c3h`) fail nothing, also the same on both. |
| C4 | ACTION | Shoots Boris | Same as solo (Boris surrenders or the mission fails) on both | | **PASS** (rig, 9883333, campaign, `campaign/evidence-0d88b92/main-9883333-qa14.tgz q_c4`). Earlier, 0d88b92: pass (rig, 0d88b92, campaign, `row-runs-2.tgz c4j2` vs `c4h2`): the same as the host alone. |
| C5 | ACTION, FX | Destroys the GoldenEye mainframe | Destroyed on both, objective on both | | **PASS** (rig, 9883333, objective sweep `rows.py SW-control`: every joiner step ran, the same objective states on both, no unresolved desync). Earlier, 0d88b92: pass (rig, 0d88b92, objective sweep `rows.py SW-control`: C completes on both, credit player 2). Earlier: pass (rig: mainframe objective C on both). |

## 17. Caverns

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| CA1 | ACTION | Destroys the comms equipment / radio | Objective on both | | **PASS** (rig, 9883333, objective sweep `rows.py SW-caverns`: every joiner step ran, the same objective states on both, no unresolved desync). Earlier, 0d88b92: pass (rig, 0d88b92, objective sweep `rows.py SW-caverns`: A, B, C complete and D, E fail on both, the same when the host does it). Earlier: pass (rig: A, B, C on both); D and E fail on both, the same when the host does it; see the desync note. |
| CA2 | TARGET | Meets Trevelyan's taunt spot first | Taunt aimed at the joiner | | **NOT RUN** on 9883333: the campaign run never reached the taunt spot (`row-runs.tgz caverns_alec`). |
| CA3 | ONEPLAYER | Reaches the lift first | Ends for both | `CA3` | **PASS** (rig, 9883333, `rows.py CA3`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py CA3`): the joiner on pad 369; one mission end on each. Earlier: pass (rig, 2026-10-30): the joiner on the lift pad (369); one ending, one mission end on each. |

## 18. Cradle

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| CR1 | HURT | Shoots Trevelyan | Same small twitch as the host sees (fixed in fixes-2) | | **NOT RUN** on 9883333: the campaign run matches the host alone (`row-runs-2.tgz cr1`), but the twitch is not in the logs: needs someone watching. |
| CR2 | ACTION, FX | Destroys the control console | Blows on both, objective on both | | **PASS** (rig, 9883333, objective sweep `rows.py SW-cradle`: every joiner step ran, the same objective states on both, no unresolved desync). Earlier, 0d88b92: pass (rig, 0d88b92, objective sweep `rows.py SW-cradle`: A and B complete on both). Earlier: pass (rig). |
| CR3 | ONEPLAYER | Corners Trevelyan on the platform first | Ending for both | | **NOT RUN** on 9883333: needs hands-on play; the campaign workstream could not stage it in the rig (`row-runs-3.tgz cr3j`, `cr3h`). |
| CR4 | SCRIPTDEATH | Falls off the cradle | That player's own death, not a mission fail for both | | **NOT RUN** on 9883333: needs hands-on play; the campaign workstream could not stage the fall in the rig. |

## 19. Aztec

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| AZ1 | SPAWN, TARGET | Reaches Jaws first | Jaws spawns and fights the joiner | | **PASS** (rig, 9883333, campaign, `main-9883333-runs.tgz m_az1`). Earlier, 0d88b92: pass (rig, 0d88b92, campaign run, logs `campaign/evidence-0d88b92/row-runs.tgz aztec_jaws`). |
| AZ2 | KEYITEM, ACTION | Reprograms the shuttle guidance | Objective on both | `AZ2` | **PASS** (rig, 9883333, `rows.py AZ2`): the joiner's guidance data on the computer completes A on both, credit player 2. Earlier, 0d88b92: not run on 0d88b92: not re-run. Earlier: pass (rig, 2026-10-30): the joiner uses both guidance items; A completes on both. |
| AZ3 | FX | Watches a guard the script removes "when not on screen" | It doesn't vanish in front of the joiner | | **NOT RUN** on 9883333: no rig line yet. |
| AZ4 | ONEPLAYER, FX | Launches the shuttle | Launch on both, ends for both | `AZ4` | **FAIL** (rig, 9883333's game code, `rows.py AZ4`, task QA-7, fix in PR #123): 7 of 7 runs. They ran on 9883333 with the QA harness: 22b22dc for the first 4, 1a6bbd6 with this check's harness uncommitted for 2, and the #125 head cba6ffb for 1. In all of them the joiner's script cleared "Final checks" and reached the ending before its countdown was under 10 s: in 6 no countdown ran on the joiner before the caption, and in 1 the caption came while it read 21 to 28 s. The host then ended the mission. The first check passed 2 of the first 4 because it only looked for a countdown on the host, even one that started after the ending; the check now places the caption and the ending against each side's countdown. The host alone passes the same check (`AZ4-solo`, 2 of 2). With #123: PASS 4 of 4 on #123's head 1121cd4 plus the #125 harness (uncommitted), and PASS 2 of 2 (AZ4-solo 1 of 1) on main 76ee103 merged with #125 (local merge b91316e). This row moves to PASS when the checklist moves to a build with #123. Earlier, 0d88b92: not run on 0d88b92: not re-run. Earlier: pass (rig, 2026-10-30): the joiner closes the doors and the launch runs; B on both, both debrief complete. |

## 20. Egyptian

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| E1 | KEYITEM | Takes the Golden Gun | Host's script sees it | | **PASS** (rig, 9883333, objective sweep `rows.py SW-egypt`: every joiner step ran, the same objective states on both, no unresolved desync). Earlier, 0d88b92: pass (rig, 0d88b92, objective sweep `rows.py SW-egypt`: A completes on both, credit player 2). Earlier: pass (rig). |
| E2 | TARGET | Fights Baron Samedi | Samedi appears and moves the same on both | | **PASS** (rig, 9883333, campaign, `campaign/evidence-0d88b92/main-9883333-qa14.tgz q_e2`): run on its own with a longer run. Earlier, 0d88b92: pass (rig, 0d88b92, campaign run, logs `campaign/evidence-0d88b92/row-runs.tgz egypt_samedi`). |
| E3 | ONEPLAYER | Reaches the "Bond must be in room" triggers first | Fire for the joiner | `E3` | **PASS** (rig, 9883333 with the QA test switches, `rows.py E3`): the joiner walks into the trap room first; its doors shut and open on the host as the script says. Earlier, 0d88b92: not run on 0d88b92: no rig line yet. |

---

## Across every mission

| # | Family | What to do | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| X1 | SCRIPTDEATH | Die during a cutscene (fade) | The scene ends with both alive or respawned | | **PASS** (rig, 9883333, campaign, `campaign/evidence-0d88b92/main-9883333-qa14.tgz q_x1, q_x1b`): in q_x1b the joiner died just before the scene; the debrief shows 1 death and success. Earlier, 0d88b92: pass (rig, 0d88b92, campaign run, logs `campaign/evidence-0d88b92/row-runs-2.tgz x1, x1b`). |
| X2 | SAMETIME | Both press the same console / switch on one frame | One activation | `X2` | **PASS** (rig, 9883333, `rows.py X2`): both press Aztec's launch console in the same second; the doors swap once (flag 100) and never back (no 800). Earlier, 0d88b92: not run on 0d88b92: not re-run. Earlier: pass (rig, 2026-10-30): both press Aztec's launch console in the same second; the doors and launch run once. |
| X3 | LATEJOIN | Late join after the alarm is raised | Alarm and stage flag as the host's | `X3` | **PASS** (rig, 9883333, `rows.py X3`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py X3`): the alarm and stage flag as the host's. Earlier: pass for the alarm (rig). |
| X3b | LATEJOIN | Late join after doors were opened | Doors as the host's | `X3b` | **PASS** (rig, 9883333's game code with the QA test switches, `rows.py X3b`, 5 of 5): 4 runs on a16f87c with the #125 harness uncommitted and 1 on the #125 head cba6ffb; a16f87c and cba6ffb differ from 9883333's game code only in the harness. Also PASS 1 of 1 on main 76ee103 merged with #125 (local merge b91316e). The host opens door 31512 when the joiner first connects (`GE_COOP_TESTOPEN=join`); it is open and still at the join snapshot the joiner applies after its intro, the snapshot corrects the joiner's 31512, and once the joiner's doors stop moving its open doors are the host's. The FAIL first recorded here (QA-8) was the check's fault, not the game's (`goldeneye-coop/net/qa8/`): it compared the host's doors at the first connect with the joiner's after the snapshot it applied about 14 s later, and by then 31512 had timed out and shut. Two runs with the door opened at 30 s were no test (it opened after the snapshot). Earlier, 0d88b92: not run on 0d88b92: `rows.py X3` does not check doors; no rig line yet. Earlier: not checked. |
| X4 | LATEJOIN | Host leaves mid-mission (migration) | New host keeps the alarm and stage flags | `X4` | **PASS** (rig, 9883333, `rows.py X4`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py X4`): the host hands over 3 s after raising the alarm; the new host keeps the alarm on and stage flag 0x40000000. |
| X4b | LATEJOIN | Host leaves mid-mission (migration) | New host keeps the doors and objectives | `X4b` | **PASS** (rig, 9883333 with the QA test switches, `rows.py X4b`): the old host opens a door and completes D, then hands over; the new host keeps D complete and the door open. Earlier, 0d88b92: not run on 0d88b92: `rows.py X4` does not check doors or objectives; no rig line yet. |
| X5 | CUTJOIN | Drop in during the intro swirl or an end cutscene | Joiner arrives in play or in the same scene | `X5` | **PASS** (rig, 9883333, `rows.py X5`). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py X5`): a joiner dropping into the Dam bungee ending; one mission end on each. Earlier: fail, fixed (rig, 2026-10-30): a joiner dropping into the Dam bungee ending could debrief twice; now once. |
| X6 | SPLITOBJ | End every mission and compare the two results screens | Same objective list and states | every run | **PASS** (rig, 9883333, CI `mission_runs.sh` 20/20 on a lossy link, its late joins 3/3, and the objective sweep 18/18: every mission ends with the same objective states on both). Earlier, 0d88b92: pass (rig, 0d88b92): the objective sweep (16 missions) and CI `mission_runs.sh` (20 missions, lossy link) end with the same objective states on both. Earlier: pass on 17 missions (rig sweep). |
| X7 | MANY | Repeat the escort rows (BU1, C1, J1) with 3 and 4 players | Followers pick one player and stay on them | | **PASS** (rig, 9883333, campaign, `campaign/evidence-0d88b92/main-9883333-qa14.tgz q_x7`). Earlier, 0d88b92: pass (rig, 0d88b92, campaign run, logs `campaign/evidence-0d88b92/row-runs.tgz x7_4`), 4 players. |
| X8 | MANY | Repeat the spawn rows (F1, SI3, ST1) with 3 and 4 players | Spawns still happen | `X8-doak3` | **PASS** (rig, 9883333, `rows.py X8-doak3` and `X8-nat4`, 3 and 4 players). Earlier, 0d88b92: pass (rig, 0d88b92, `rows.py X8-doak3`) (Facility Doak, 3 players) and `X8-nat4` (Statue Natalya, 4 players). Earlier: pass (Facility Doak 3 players, Statue Natalya 4 players). |
| X9 | LATEJOIN | Cut the line between host and joiner for 8 to 10 s mid-mission, then restore it | One game again: the joiner rejoins the host (or a documented tie-break), never two hosts | rig `CUT=1:50:8`, `CUT=2:50:10` (netcode workstream) | **FAIL** (rig, 9883333, `net_scenarios.sh` with `CUT=2:50:10`, run by QA): the joiner takes over ("took over 36 chrs") and hosts alone while the old host keeps hosting; no step-down and no second join snapshot. Fix in PR #116, whose split runs pass on its head 97304bc (`goldeneye-coop/qa/pr116-split/`). Earlier, 0d88b92: fail (rig, 0d88b92, reported by the netcode workstream, task NET-3; not re-run by QA): the joiner takes over and hosts alone, the old host keeps hosting, and they never merge (the new host logs "not in our lobby"). Needs a protocol change. Logs: `goldeneye-coop/net/split-brain/` (`README.txt`; `main-summary.txt` and `logs.tgz` `res-base/joiner-cut10` for 0d88b92, a 10 s joiner cut at 50 s; `pr116-summary.txt` for PR #116). The "result: PASS" in `main-summary.txt` is from the old runner, which had no split check; read the logs, which show two hosts. |

## Code audit notes

How each "Bond" check in the mission scripts is handled on the host
(`src/game/chrai.c`). "Either player" means the host's script also tries each
partner as Bond.

| Script check | Co-op handling | Note |
| --- | --- | --- |
| Bond in room / near pad / below a height | either player | Credit goes to whoever tripped it |
| Bond far from pad | every player must be far | Right for "spawn when away" |
| Chr sees / hears / is aimed at / near Bond | either player, that player becomes its target | |
| Bond has item equipped | either player (guard's target first) | Valentin's "put your gun away" |
| Bond collected object | the host's inventory or a partner holding it | |
| Bond used gadget on object | the joiner's gadget is forwarded | Frigate's bombs, Bunker 1's camera, Facility, Aztec |
| Bond health / Bond dead | the current target's | |
| Chr is on screen (`if_guard_is_on_screen`) | the host's screen or a partner's (`coopChrOnPartnerScreen`, by the partner's position and facing) | Natalya's catch-up teleport (Bunker 2, Control, Train), Aztec's guard removal, Jungle's Xenia intro |
| Chr's room is on screen | **host's screen only** | Not used by the US mission scripts |
| Room with pad on screen | **host's screen only** | Not used by the US mission scripts |
| Bond's total ammo below | **host only** | Not used by the US mission scripts |
| Bond invincible (cutscene check) | the local player | |
