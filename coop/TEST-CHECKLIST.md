# Co-op test checklist: the joiner does everything

The mission scripts were written for one Bond. Every bug found in two-player
testing so far comes from a script, effect or check that only looks at one
player. This checklist runs each mission with the **joiner** doing the work
while the host stays back, so any place that still only sees the host shows up.

How to use it: play each row (or run its rig line), then fill in **Result**
with `pass`, `fail` (plus a note) or `untestable` (plus why). "Both screens"
means host and joiner must agree. A row that only shows on the joiner's
screen while the host's script moved on (or the other way round) is a fail.

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
the players with `GE_COOP_STEPS` and the `GE_COOP_TEST*` switches. Results below are
from the bug-fix branch on 2026-10-29 (heads 620f526 to 93ea314), US cart, Linux
build, mock Steam. A blank result was not run.

**Fixed: intermittent desync.** In joiner-heavy runs where a blast kills guards
(Caverns, Jungle, Depot), the joiner sometimes logged "repl: unresolved desync: the
chrs here are not the host's". The bug-fix branch fixed it in 7dfb28e and c4237cd. On
c4237cd, 29 pair runs (the sweep, plus Jungle, Caverns and Depot run 4 times each) had
none.

**Open: Jungle's respawned guards.** Jungle's own respawner (vanilla script) spawns a new
guard when guard 29, 30, 34, 39, 40 or 41 is gone, and gives it the dead guard's number.
The joiner matches guards by number, so it reuses the old, dying copy for the new guard.
c4237cd stands that copy back up. Whether it should be replaced with a fresh one (new
head and gun) is with the bug-fix branch. It is the same in solo: a host on its own
never had a guard leave ACT_DIE (`solo.sh`, `GE_COOP_REVIVE=1`).

---

## 1. Dam

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| D1 | ONEPLAYER, SPLITOBJ | Walks into the bungee room alone, jumps | Bungee objective completes on both; both get the ending | `bungee` || pass (rig, 2 and 3 players; failed on 620f526, fixed in 74d3da9) |
| D2 | ACTION | Shoots out every alarm box (00 Agent) | "Neutralize all alarms" completes on both | || pass (rig: joiner breaks all 4 alarms, 00 Agent) |
| D3 | KEYITEM | Picks up and places the covert modem on the satellite link (SA+) | Objective completes on both; the host's inventory no longer shows a modem to place | | |
| D4 | ACTION | Uses the mainframe for the data backup (00 Agent) | Completes on both | | |
| D5 | LATEJOIN | Joins late after the host raised the alarm | Alarm is sounding on the joiner, guards alerted | `lateAlarm` || pass (rig: drop-in at 35 s sees the alarm on) |
| D6 | CUTJOIN | Joins during the opening swirl | Joiner spawns into play, not a frozen camera | | |
| D7 | SCRIPTDEATH | Dies on the bungee platform while the host is far away | Respawns beside the host; jump still possible | | |

## 2. Facility

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| F1 | SPAWN | Reaches Doak's lab first, looking at his spawn spot | Doak appears on both screens; "Contact 006/Doak" objective not failed | `doak` || pass (rig, 2 and 3 players) |
| F2 | SPLITOBJ | Meets Doak first (the known "objective B failed" flash) | No "failed" message on either screen, even briefly | `doak` || pass (rig: no failed flash) |
| F3 | KEYITEM | Talks to Doak, then opens the door that needs the decoder | Host receives the decoder (script gives it "to the team"); the joiner can still open the door | | |
| F4 | ACTION, FX | Plants charges on the bottling-room tanks | Tanks blow on both; "Destroy all tanks" completes on both | || pass (rig: joiner shoots all 10 tanks, objective on both) |
| F5 | SAMETIME | Both players plant on the same tank in the same second | Counted once; tank blows once | | |
| F6 | ONEPLAYER | Reaches the bottling room first; its alarm/timer fires on the joiner's entry | Same timer on both screens | | |
| F7 | FX | Watches Natalya/the scientist room while the host is elsewhere; guards that hide "when not on screen" vanish | No chr vanishes in front of the joiner | | |
| F8 | ONEPLAYER | Reaches the exit (Ourumov ending) first | Ending starts for both, once | | |

## 3. Runway

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| R1 | KEYITEM | Takes the plane key from the hut | Host's script sees the key collected; plane objective works for whoever boards | `runwayKey` || pass (rig) |
| R2 | ACTION, FX | Blows up the heavy gun emplacements | Destroyed on both, objective on both | || pass (rig) |
| R3 | ONEPLAYER | Gets in the plane first | Ending for both | | |

## 4. Surface 1

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| S1 | ACTION | Shoots the power supply / comms dish | Objective on both | | |
| S2 | KEYITEM | Takes the bunker key from the guard's body | Host can open the bunker door too | || pass (rig: keys collected, objectives on both) |
| S3 | ONEPLAYER | Enters the bunker vent first | Mission ends for both | | |

## 5. Bunker 1

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| B1 | ACTION, KEYITEM | Uses the camera on the GoldenEye / photo of the main room (gadget-on-object checks) | Objective on both | `bunker1Gadget` || pass for the destroy objective (rig); camera photo not run |
| B2 | TARGET | Approaches the technician (the unreproduced tester report) | Technician reacts to the joiner, not to the far host | | |
| B3 | ACTION | Shoots the security cameras | Disabled on both | | |
| B4 | FX | Stands in the six-door gate (the door storm fixed in fixes-1) | Gate opens once; no network flood | | |

## 6. Silo

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| SI1 | ACTION, FX | Plants the charges in the fuel rooms | Charges blow on both | | |
| SI2 | KEYITEM | Picks up the circuit board / satellite plans | Objective on both | | |
| SI3 | SPAWN, TARGET | Reaches Ourumov first, looking at his spawn | Ourumov appears and runs for both; drops his key for both | | |
| SI4 | ONEPLAYER | Starts the escape countdown | Same countdown on both screens | `siloClock` | |
| SI5 | BLEEDOUT | Is down and bleeding out as the countdown ends | Mission fails once, one failure screen on each | | |
| SI6 | FX | Countdown runs out | Explosions around both players, not just the host | | |

## 7. Frigate

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| FR1 | ONEPLAYER | Enters a hostage room first | Hostage-taker reacts to the joiner; hostages live on both | `hostage` || pass (rig `hostage`) |
| FR2 | ACTION | Shoots a hostage-taker | Dead on both; hostage freed on both | | |
| FR3 | FX | A hostage-taker throws a grenade at the joiner | Grenade and blast on both | | |
| FR4 | KEYITEM, ACTION | Plants the tracker bug on the helicopter | Objective on both | `frigateBug` | |
| FR5 | ONEPLAYER | Reaches the boat first | Ending for both (fixed in fixes-1) | `frigateBoat` || pass (fixed in fixes-1) |

## 8. Surface 2

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| SU1 | ACTION | Destroys the communications dish / power | Objective on both | || pass (rig: A and B destroy objectives) |
| SU2 | SPAWN | Watches the reinforcement spawn points | Reinforcements arrive the same on both (Surface 2 spawn fix, #63) | | |
| SU3 | ONEPLAYER | Enters the bunker first | Ends for both | || pass (rig: joiner enters the objective room, C/D on both) |

## 9. Bunker 2

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| BU1 | TARGET | Leads Natalya out of the cell, the host far behind | She follows the joiner, doesn't hesitate between players | `bunker2Nat` || untestable in rig (her follow needs the story up to it); needs a hands-on run |
| BU2 | FX, ONEPLAYER | Looks back at Natalya while she's out of the host's view | She doesn't teleport in front of the joiner | `bunker2Nat` | |
| BU3 | KEYITEM | Picks up the GoldenEye key / Boris's clipboard | Collected check works for the host's script | `bunker2Items` || pass (rig: B and D collect objectives on both) |
| BU4 | ACTION | Stands in front of a drone gun | The host's drone gun fires at the joiner | `dronegun` || pass (rig `dronegun`) |
| BU5 | SCRIPTDEATH | Dies while Natalya follows the joiner | She switches to the host (or waits), doesn't freeze | || untestable in rig (as BU1) |

## 10. Statue

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| ST1 | SPAWN | Host waits at the helicopter when Janus's timer starts | Natalya spawns, objective not failed (fixed in fixes-2) | `natalya` || pass (rig `natalya`, 2 and 4 players) |
| ST2 | TARGET | Talks to Janus with the host nearby | Janus stays facing one player (fixed in fixes-2) | | |
| ST3 | ACTION | Hits Janus | Mission reacts as for the host's hit (fixed in fixes-2) | | |
| ST4 | ONEPLAYER | Walks up to Janus | Guards attack and the "too close" fail happens for the joiner too | | |
| ST5 | KEYITEM | Picks up the flight recorder | Objective on both | || untestable in rig: the recorder sits in the exit room, which ends the mission (as in solo) |
| ST6 | TARGET | Meets Valentin with a gun out | Valentin's "put your gun away" reacts to the joiner | | |

## 11. Archives

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| A1 | CUTJOIN | Joins during the interrogation intro | Joiner starts in play | | |
| A2 | TARGET | Rescues Natalya | She follows the joiner | | |
| A3 | ONEPLAYER | Reaches the exit window first | Ends for both | || pass for the pad 152 room objective (rig) |

## 12. Streets

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| SR1 | ONEPLAYER | Drives the tank (vehicles run on both sides) | "Bond in tank" checks work for the joiner's tank | | |
| SR2 | ACTION | Meets Valentin / reaches the exit | Ends for both | | |
| SR3 | FX | Civilians appear and disappear (Streets reaper fix) | No flicker on the joiner | | |

## 13. Depot

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| DE1 | ACTION, FX | Destroys the helicopters and weapon caches | Destroyed on both | || pass (rig: 17 caches); see the desync note |
| DE2 | SPAWN | Reaches Trevelyan's spawn first, watching it | Trevelyan appears for both | | |
| DE3 | KEYITEM | Picks up the safe key and opens the safe | Works for the joiner, objective on both | || not run |
| DE4 | ONEPLAYER | Boards the train first | Ending for both | | |

## 14. Train

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| T1 | ACTION | Shoots guard 4 (the known "objective B failed" flash) | No failed flash on the joiner | `trainGuard4` || pass (rig): B fails on both, the same as when the host does it |
| T2 | TARGET, FX | Frees Natalya and watches her while the host is out of view | No catch-up teleport in front of the joiner | | |
| T3 | ONEPLAYER | Starts the countdown from the joiner's side (the countdown hand-off) | Same countdown on both, from the host | `trainClock` || pass (rig: countdown values match on both) |
| T4 | FX | Countdown ends / the floor is cut with the watch | Explosions on both (fixed in fixes-2) | | |
| T5 | BLEEDOUT | Is down as the countdown runs out | One failure, one screen | | |
| T6 | KEYITEM | Uses the watch laser on the floor | The cut works for the joiner | | |

## 15. Jungle

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| J1 | TARGET | Walks with Natalya | She follows, doesn't attack the joiner (fixed in fixes-2) | || untestable in rig (as BU1) |
| J2 | ONEPLAYER | Meets Xenia first, the host far away | Xenia's intro (text and music) plays when the joiner sees her, not when the host does | `xenia` || untestable in rig (players can see her spot from the start); needs a hands-on run |
| J3 | ACTION | Kills Xenia | Objective on both | | |
| J4 | ACTION | Destroys the drone guns | Objective on both | || pass (rig: drone guns A and C on both); see the desync note |
| J5 | SCRIPTDEATH | Dies on the bridge while Natalya follows | She keeps going with the host | || untestable in rig (as BU1) |

## 16. Control

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| C1 | TARGET | Escorts Natalya to the console | She stays at the console while the joiner guards her | `controlNat` || untestable in rig (as BU1) |
| C2 | FX, ONEPLAYER | Watches Natalya while the host is out of view | No catch-up teleport in front of the joiner | `controlNat` | |
| C3 | ACTION | Breaks the console Natalya is using | Mission fails on both, as in solo | | |
| C4 | ACTION | Shoots Boris | Same as solo (Boris surrenders or the mission fails) on both | | |
| C5 | ACTION, FX | Destroys the GoldenEye mainframe | Destroyed on both, objective on both | || pass (rig: mainframe objective C on both) |

## 17. Caverns

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| CA1 | ACTION | Destroys the comms equipment / radio | Objective on both | || pass (rig: A, B, C on both); D and E fail on both, the same when the host does it; see the desync note |
| CA2 | TARGET | Meets Trevelyan's taunt spot first | Taunt aimed at the joiner | | |
| CA3 | ONEPLAYER | Reaches the lift first | Ends for both | | |

## 18. Cradle

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| CR1 | HURT | Shoots Trevelyan | Same small twitch as the host sees (fixed in fixes-2) | | |
| CR2 | ACTION, FX | Destroys the control console | Blows on both, objective on both | || pass (rig) |
| CR3 | ONEPLAYER | Corners Trevelyan on the platform first | Ending for both | | |
| CR4 | SCRIPTDEATH | Falls off the cradle | That player's own death, not a mission fail for both | | |

## 19. Aztec

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| AZ1 | SPAWN, TARGET | Reaches Jaws first | Jaws spawns and fights the joiner | | |
| AZ2 | KEYITEM, ACTION | Reprograms the shuttle guidance | Objective on both | | |
| AZ3 | FX | Watches a guard the script removes "when not on screen" | It doesn't vanish in front of the joiner | | |
| AZ4 | ONEPLAYER, FX | Launches the shuttle | Launch on both, ends for both | | |

## 20. Egyptian

| # | Family | Joiner does | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| E1 | KEYITEM | Takes the Golden Gun | Host's script sees it | || pass (rig) |
| E2 | TARGET | Fights Baron Samedi | Samedi appears and moves the same on both | | |
| E3 | ONEPLAYER | Reaches the "Bond must be in room" triggers first | Fire for the joiner | | |

---

## Across every mission

| # | Family | What to do | Both screens should show | Rig check | Result |
| --- | --- | --- | --- | --- | --- |
| X1 | SCRIPTDEATH | Die during a cutscene (fade) | The scene ends with both alive or respawned | | |
| X2 | SAMETIME | Both press the same console / switch on one frame | One activation | | |
| X3 | LATEJOIN | Late join after doors opened and alarm raised | Doors and alarm as the host's | `lateAlarm` || pass for the alarm (rig); doors not checked |
| X4 | LATEJOIN | Host leaves mid-mission (migration) | New host keeps the alarm, doors and objectives | | |
| X5 | CUTJOIN | Drop in during the intro swirl or an end cutscene | Joiner arrives in play or in the same scene | | |
| X6 | SPLITOBJ | End every mission and compare the two results screens | Same objective list and states | every run || pass on 17 missions (rig sweep) |
| X7 | MANY | Repeat the escort rows (BU1, C1, J1) with 3 and 4 players | Followers pick one player and stay on them | | |
| X8 | MANY | Repeat the spawn rows (F1, SI3, ST1) with 3 and 4 players | Spawns still happen | || pass (Facility Doak 3 players, Statue Natalya 4 players) |

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
