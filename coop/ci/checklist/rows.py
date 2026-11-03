#!/usr/bin/env python3
"""Run rows of coop/TEST-CHECKLIST.md in the rig and judge them from the logs.

    python3 coop/ci/checklist/rows.py <out dir> [ROW ...]      # default: every row below
    python3 coop/ci/checklist/rows.py <out dir> sweep          # the SW-<stage> objective sweep
    python3 coop/ci/checklist/rows.py --judge <out dir> [ROW ...]   # judge earlier logs again, no runs
    PAR=2 SECSX=2.5 python3 coop/ci/checklist/rows.py out/rows D2 F5

PAR pairs run at once (default 2). Seconds below are mission seconds; a pair
is killed after SECSX (default 2.5) times that in wall time, since software
GL runs a loaded machine well under full speed (each pair stops early once
its steps are done). On 4 cores PAR=3 ran the game at about a third of real
time and the steps never got to run: keep PAR at most cores / 2.

Each row is one rig run (pair.sh, or party.sh for 3 and 4 players) plus the
log lines that decide it. A row prints PASS when every check holds, FAIL with
the first check that didn't. A FAIL here is a lead, not a verdict: read the
logs (<out>/<row>/host.log, join.log, or p1.log..p4.log) before calling it a
bug, because a rig step that missed its target (a chr not there yet, an
object out of reach) also fails a check. Rows that need a person at the
controls are not here; TEST-CHECKLIST.md says which.

Checks, each a regex over a log:
    h:<re>   the host's log has it        j:<re>   the joiner's log has it
    b:<re>   both logs have it            !h: !j: !b:  must not have it
    pN:<re>  player N's log (party runs)
    =h:<re>  exactly once in the host's log (=j: =b: likewise)
Every row also checks that the joiner got in (join snapshot applied), that
no log has an unresolved desync and that nothing crashed.
"""
import os
import re
import subprocess
import queue
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
INV = 'GE_CHEATS=invuln'

# row: (stage, diff, host env, joiner env, seconds, join after, checks[, players, extra env per player])
ROWS = {
    # ---- Dam
    'D1': ('dam', 0, f'{INV} GE_COOP_TESTWIN=1', f'{INV} GE_COOP_TESTPAD=330:10', 90, 2,
           ['h:objective 3 \\(A\\) is now complete', 'b:debrief: stage', '=h:mission end', '=j:mission end']),
    'D2': ('dam', 2, INV, f'{INV} GE_COOP_STEPS=o0@12,b0@14,o1@16,b1@18,o2@20,b2@22,o3@24,b3@26', 60, 2,
           ['b:objective 0 \\(A\\) is now complete']),
    # the backup needs the modem in (stage flag 0x10000, set by the host here)
    'D4': ('dam', 2, f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_TESTFLAG=10000:5', f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=o6@8,u6@9,o7@11,u7@12', 70, 2,
           ['b:objective 2 \\(C\\) is now complete', '!b:objective 2 \\(C\\) is now failed']),
    # D3 does not stage: the thrown modem lands but never attaches to the link (B fails),
    # the host alone the same (D3-solo), so the row stays NOT RUN until a line attaches it
    'D3': ('dam', 2, f'{INV} GE_COOP_WATCHWORLD=1', f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=h47@8,v5@10,f1@12', 50, 2,
           ['b:objective 1 \\(B\\) is now complete', '!b:objective 1 \\(B\\) is now failed']),
    'D3-solo': ('dam', 2, f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=h47@8,v5@10,f1@12', '', 50, 2,
                ['h:objective 1 \\(B\\) is now complete'], 1),
    'D4-solo': ('dam', 2, f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_TESTFLAG=10000:5 GE_COOP_STEPS=o6@8,u6@9,o7@11,u7@12', '', 70, 2,
                ['h:objective 2 \\(C\\) is now complete'], 1),
    'D5': ('dam', 0, f'{INV} GE_COOP_TESTWORLD=1 GE_COOP_WATCHWORLD=1', f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=p1@50', 75, 18,
           ['j:watch: alarm on', 'j:repl: join snapshot applied']),
    # the host watches its whole intro (no A taps), the joiner starts 8 s after it: a drop-in during the swirl
    'D6': ('dam', 0, f'{INV} GE_SKIPINTRO=0', f'{INV} GE_COOP_STEPS=p278@20', 60, 8,
           ['h:intro: overview camera ended .* by its timer', 'j:cutscene: the host.s intro of stage \\d+ is over', 'j:repl: join snapshot applied', 'j:step p278 at \\d+ s done']),
    # ---- Facility
    'F5': ('facility', 0, f'{INV} GE_COOP_TESTDUMP=30 GE_COOP_STEPS=o1@10,b1@14,o2@16,b2@20', f'{INV} GE_COOP_TESTDUMP=30 GE_COOP_STEPS=o1@10,b1@14,o2@16,b2@20', 50, 2,
           ['b:dump: tag 1 .* destroyed [1-9]', 'b:dump: tag 2 .* destroyed [1-9]', '!b:objective 3 \\(D\\) is now failed']),
    'F8': ('facility', 0, INV, f'{INV} GE_COOP_STEPS=p309@10', 60, 2,
           ['=h:mission end', '=j:mission end']),
    # ---- Runway
    'R1': ('runway', 0, INV, f'{INV} GE_COOP_STEPS=o2@10,g2@12', 40, 2,
           ['b:objective 0 \\(A\\) is now complete']),
    'R3': ('runway', 0, INV, f'{INV} GE_COOP_STEPS=o2@10,g2@12,o3@14,u3@16', 80, 2,
           ['=h:mission end', '=j:mission end']),
    # ---- Surface 1
    'S3': ('surface', 0, INV, f'{INV} GE_COOP_STEPS=p46@10', 80, 2,
           ['=h:mission end', '=j:mission end']),
    # ---- Frigate
    # the joiner shoots hostage-taker 1 from 3 to 10 m; FR2-solo is the host doing the same alone
    'FR2': ('frigate', 0, f'{INV} GE_COOP_WATCH=1,44', f'{INV} GE_COOP_WATCH=1,44 GE_COOP_STEPS=n1@10,k1@11', 45, 2,
            ['b:watch: chr 1 gone']),
    'FR2-solo': ('frigate', 0, f'{INV} GE_COOP_WATCH=1,44 GE_COOP_STEPS=n1@10,k1@11', '', 45, 2,
                 ['h:watch: chr 1 gone'], 1),
    # the boat's exit arms once "Bond" has left the boat's room, and in co-op that is every player: both step off first
    'FR5': ('frigate', 0, f'{INV} GE_COOP_STEPS=p60@8', f'{INV} GE_COOP_STEPS=p60@8 GE_COOP_TESTPAD=153:20', 80, 2,
            ['=h:mission end', '=j:mission end']),
    'FR5-hoststays': ('frigate', 0, INV, f'{INV} GE_COOP_STEPS=p60@8 GE_COOP_TESTPAD=153:20', 80, 2,
                      ['=h:mission end', '=j:mission end']),
    # ---- Surface 2
    'SU3': ('surface2', 0, INV, f'{INV} GE_COOP_STEPS=p3@10', 50, 2,
            ['b:objective 3 \\(\\w\\) is now complete', '=h:mission end', '=j:mission end']),
    # ---- Bunker 2 (Natalya is chr 0; the joiner opens her cell, tag 3, with GE_COOP_TESTDOOR)
    'BU1': ('bunker2', 1, f'{INV} GE_COOP_TESTDOOR=1 GE_COOP_STEPS=c27@8', f'{INV} GE_COOP_TESTDOOR=1 GE_COOP_STEPS=o3@8,c13@18,c12@30', 70, 2,
            ['h:chr 0 goes to player 2', '!h:chr 0 goes to player 1']),
    # pair.sh stops both once either side's steps are done: the host's last step waits for the joiner's death
    'BU5': ('bunker2', 1, f'{INV} GE_COOP_TESTDOOR=1 GE_COOP_STEPS=c27@8,c27@60', 'GE_COOP_TESTDOOR=1 GE_COOP_STEPS=o3@8,c13@18,c12@30,d@40', 80, 2,
            ['h:chr 0 goes to player 2', 'h:chr 0 goes to player 1']),
    # the ending (Natalya and Bond run out to the helipad); GE_COOP_TESTALLOBJ=1 counts every objective
    # complete, pad 51 is in the exit room. BU6: the host gets there, the joiner is still at the start: the
    # joiner's own Bond is stood on the ending's pad, not left running where it stood (off the shot)
    'BU6': ('bunker2', 0, f'{INV} GE_COOP_TESTALLOBJ=1 GE_COOP_STEPS=p51@12', INV, 70, 2,
            ["j:our Bond goes to the ending's pad 119", "j:our Bond runs the ending's list 0417",
             '!j:mission complete, telling the peer', '=h:mission end', '=j:mission end']),
    # BU7: the joiner gets there and its own objectives read incomplete (the switch is on the host only):
    # its copy of the exit script waits at the host's ending instead of taking the failure branch, which
    # ended the joiner's mission a second into the host's cutscene
    'BU7': ('bunker2', 0, f'{INV} GE_COOP_TESTALLOBJ=1', f'{INV} GE_COOP_STEPS=p51@12', 70, 2,
            ["j:our copy of that script waits", "j:our Bond runs the ending's list 0417",
             '!j:mission complete, telling the peer', '=h:mission end', '=j:mission end']),
    # ---- Statue
    'ST5': ('statue', 0, INV, f'{INV} GE_COOP_STEPS=g1@12', 60, 2,
            ['b:objective 4 \\(E\\) is now complete']),
    # ---- Archives
    'A1': ('archives', 0, f'{INV} GE_SKIPINTRO=0', f'{INV} GE_COOP_STEPS=p152@25', 60, 8,
           ['h:intro: overview camera ended .* by its timer', 'j:cutscene: the host.s intro of stage \\d+ is over', 'j:step p152 at \\d+ s done', 'b:objective 0 \\(A\\) is now complete']),
    'A3': ('archives', 0, INV, f'{INV} GE_COOP_STEPS=p152@10', 40, 2,
           ['b:objective 0 \\(A\\) is now complete']),
    # ---- Streets
    'SR2': ('streets', 0, INV, f'{INV} GE_COOP_STEPS=p248@10', 70, 2,
            ['=h:mission end', '=j:mission end']),
    # ---- Depot
    'DE4': ('depot', 0, INV, f'{INV} GE_COOP_STEPS=p271@10', 80, 2,
            ['=h:mission end', '=j:mission end']),
    # ---- Train
    'T1': ('train', 0, f'{INV} GE_COOP_WATCH=4', f'{INV} GE_COOP_WATCH=4 GE_COOP_STEPS=c4@10,k4@12', 40, 2,
           # B never fails at Agent, not even solo: the row is that the
           # joiner sees no failed flash either
           ['!h:objective 1 \\(B\\) is now failed', '!j:objective 1 \\(B\\) is now failed', 'j:step k4 at \\d+ s done']),
    'T1-solo': ('train', 0, f'{INV} GE_COOP_WATCH=4 GE_COOP_STEPS=c4@10,k4@12', '', 40, 2,
                ['!h:objective 1 \\(B\\) is now failed', 'h:step k4 at \\d+ s done'], 1),
    # ---- Caverns
    'CA3': ('caverns', 0, INV, f'{INV} GE_COOP_STEPS=p369@10', 80, 2,
            ['=h:mission end', '=j:mission end']),
    # ---- Jungle / Control exits (SR2's note)
    'J-exit': ('jungle', 0, INV, f'{INV} GE_COOP_STEPS=p468@10', 80, 2,
               ['=h:mission end', '=j:mission end']),
    'C-exit': ('control', 0, INV, f'{INV} GE_COOP_STEPS=p168@10', 80, 2,
               ['=h:mission end', '=j:mission end']),
    'X4': ('dam', 0, f'{INV} GE_COOP_TESTWORLD=1 GE_COOP_TESTABORT=18:handover', f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=p1@60', 90, 2,
           ['h:testabort \\(hand over\\)', 'j:host migration: player 2 hosts', 'j:watch: alarm on']),
    # ---- Silo: the joiner plants all four charges (00 Agent); the countdown starts once one settles.
    # On 9883333 three of the four reach the host; the charge thrown at pad 88 never counts
    # for its room (from pad 89, or looking down, neither), so SI1 is a lead, not a verdict
    'SI1': ('silo', 2, f'{INV} GE_COOP_WATCHWORLD=1', f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=h34@8,p4@10,f1@12,p12@16,f1@18,p88@22,f1@24,p95@28,f1@30', 70, 2,
            ['h:peer planted objective item 0 ', 'h:peer planted objective item 1 ', 'h:peer planted objective item 2 ',
             'h:peer planted objective item 3 ', 'b:objective 0 \\(A\\) is now complete']),
    'SI4': ('silo', 2, f'{INV} GE_COOP_WATCHWORLD=1', f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=h34@8,p4@10,f1@12,p1@40', 60, 2,
            ['b:watch: countdown running', 'clocksame']),
    # ---- Frigate: the joiner throws the tracker bug onto the helicopter (tag 0); D on both
    'FR4': ('frigate', 0, f'{INV} GE_COOP_WATCHWORLD=1', f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=h47@8,v0@10,f1@12', 50, 2,
            ['b:objective 3 \\(\\w\\) is now complete']),
    # ---- Aztec: guidance data on the computer (A), then the launch: console 11 swaps the doors,
    # the joiner's gadget on 15 starts the 30 s countdown, and the shuttle goes (B)
    'AZ2': ('aztec', 0, f'{INV} GE_COOP_WATCHWORLD=1', f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=h50@8,v19@10,x19@12', 45, 2,
            ['b:objective 0 \\(A\\) is now complete']),
    'AZ4': ('aztec', 0, f'{INV} GE_COOP_WATCHWORLD=1',
            f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=h50@8,v19@10,x19@12,o11@14,u11@16,h73@20,v15@22,x15@24,p1@75', 100, 2,
            ['b:objective 0 \\(A\\) is now complete', 'hflag:100',
             # the countdown shows on the joiner, and "Final checks cleared" (under 10 s) and the
             # ending (under 3 s, as solo) come only once each side's countdown has run that far
             'seqj:countdown running and shown=>Final checks cleared',
             'cdb:10:Final checks cleared', 'cdb:3:objective 1 \\(B\\) is now complete']),
    'AZ4-solo': ('aztec', 0, f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=h50@8,v19@10,x19@12,o11@14,u11@16,h73@20,v15@22,x15@24,p1@75', '', 100, 2,
                 ['cdh:10:Final checks cleared', 'cdh:3:objective 1 \\(B\\) is now complete'], 1),
    # ---- Depot: the joiner takes the safe key, opens the safe door and takes the blueprints (Secret Agent)
    'DE3': ('depot', 1, f'{INV} GE_COOP_WATCHDOORS=1', f'{INV} GE_COOP_WATCHDOORS=1 GE_COOP_STEPS=g0@10,p50@14,e0@16,g5@22', 50, 2,
            ['b:objective 2 \\(\\w\\) is now complete', 'j:step e: door \\d+ at \\d+, closed before', 'h:watch: door \\d+ open',
             'b:objective 3 \\(\\w\\) is now complete']),
    # ---- Across every mission
    'X3': ('dam', 0, f'{INV} GE_COOP_TESTWORLD=1 GE_COOP_WATCHWORLD=1', f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=p1@50', 75, 18,
           ['j:watch: alarm on', 'j:watch: stage flags 4']),
    # ---- Egypt: the joiner walks into the trap room (pad 151): doors 1 and 2 shut and 0 opens; the room of pad 69 resets them
    'E3': ('egypt', 0, f'{INV} GE_COOP_WATCHDOORS=1', f'{INV} GE_COOP_WATCHDOORS=1 GE_COOP_STEPS=p151@10,p69@20', 45, 2,
           ['j:step p69 at \\d+ s done', 'h:watch: door \\d+ closed', 'h:watch: door \\d+ open']),
    'X2': ('aztec', 0, f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=o11@14,u11@16', f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=o11@14,u11@16', 50, 2,
           ['h:step u11 at 16 s done', 'j:step u11 at 16 s done', 'hflag:100', '!hflag:800', '!jflag:800']),
    'X3b': ('dam', 0, f'{INV} GE_COOP_TESTOPEN=join GE_COOP_WATCHDOORS=1', f'{INV} GE_COOP_WATCHDOORS=1 GE_COOP_STEPS=p1@50', 75, 40,
            ['h:testopen: door', 'doorsame']),
    'X4b': ('dam', 0, f'{INV} GE_COOP_TESTOPEN=8 GE_COOP_TESTFLAG=1000:10 GE_COOP_WATCHDOORS=1 GE_COOP_TESTABORT=18:handover',
            f'{INV} GE_COOP_WATCHDOORS=1 GE_COOP_STEPS=p1@60', 90, 2,
            ['h:testabort \\(hand over\\)', 'j:host migration: player 2 hosts', 'j:objective 3 \\(\\w\\) is now complete',
             '!j:objective 3 \\(\\w\\) is now incomplete', 'doorkept']),
    # ---- Open rows, first rig lines (e1e93f5)
    # F7: Facility's only "on screen" script check (list 0x40d, chrs 51-55 per a solo run with the
    # check logged) holds a guard until he is on screen; none removes one. The joiner stands
    # 3-10 m from each in turn, facing him, with the host at the start: none may vanish in view
    'F7': ('facility', 0, f'{INV} GE_COOP_WATCH=51,52,53,54,55',
           f'{INV} GE_COOP_WATCH=51,52,53,54,55 GE_COOP_STEPS=n51@10,n52@25,n53@40,n54@55,n55@70', 90, 2,
           ['!j:watch: chr 5[1-5] gone \\(was on our screen\\)', 'h:chr 5[1-5] seen by player 2', 'j:step n55 at \\d+ s done']),
    # SR1: the joiner gets in the tank, the host stands chr 2 in front of it; no stage script
    # uses if_bond_in_tank, so "in tank" is the engine's (running over, the tank's hits)
    'SR1': ('streets', 0, f'{INV} GE_COOP_TESTTANKCHR=2', f'{INV} GE_COOP_TESTTANK=10', 50, 2,
            ['h:player 2 is driving tank', 'j:we are driving tank', 'h:player 2.s tank runs over chr 2', 'j:host chr 2 fades away']),
    # SR3: Streets' script shows civilians 39 and 40 for about half a second every few seconds;
    # before the reaper fix the joiner kept each for 5 s, so its showings ran together. Each showing
    # lasts at most 4 s in the once-a-second watch (two showings 2 s apart can read as one)
    'SR3': ('streets', 0, f'{INV} GE_COOP_WATCH=39,40', f'{INV} GE_COOP_WATCH=39,40 GE_COOP_STEPS=l0@5 GE_COOP_STEPSLINGER=60', 80, 2,
            ['spanh:39:4', 'spanh:40:4', 'spanj:39:4', 'spanj:40:4']),
    # SU2: once the alarm is on, the script (list 0x1003) spawns guards at pad 214 in the places of
    # chrs 15-17, and list 0x1004 spawns a new chr 9 once 9 is gone. The guards run ai_34, which
    # removes a guard on its first tick while nobody is near pad 214 (alone too: SU2-solo), so a
    # player must stand there for one to stay. The host raises the alarm (testworld) and goes to
    # pad 214, the joiner kills chr 9: the host's guard reaches the joiner under the host's number,
    # and the chr sets must stay in step (no unresolved desync; QA-9: they didn't on e1e93f5)
    'SU2': ('surface2', 0, f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_TESTWORLD=1 GE_COOP_WATCH=9,15,16,17 GE_COOP_STEPS=p214@18 GE_COOP_STEPSLINGER=70',
            f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_WATCH=9,15,16,17 GE_COOP_STEPS=k9@20 GE_COOP_STEPSLINGER=70', 100, 2,
            ['b:watch: alarm on', 'h:step p214 at \\d+ s done', 'j:mirrored host chr 1[567] ']),
    # SU2 split in two: the alarm alone with nobody near pad 214 (each guard removes itself at
    # once, and since #129 the host never sends it), and chr 9 shot with no alarm
    'SU2-alarm': ('surface2', 0, f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_TESTWORLD=1 GE_COOP_WATCH=9,15,16,17',
                  f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_WATCH=9,15,16,17 GE_COOP_STEPS=l0@20 GE_COOP_STEPSLINGER=70', 100, 2,
                  ['b:watch: alarm on', 'j:step l0 at \\d+ s done']),
    'SU2-kill9': ('surface2', 0, f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_WATCH=9,15,16,17',
                  f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_WATCH=9,15,16,17 GE_COOP_STEPS=k9@20 GE_COOP_STEPSLINGER=70', 100, 2,
                  ['j:step k9 at \\d+ s done']),
    # SU2-solo: the host alone, alarm at 15 s and chr 9 shot at 20 s: how the spawned chrs come and go without a partner
    'SU2-solo': ('surface2', 0, f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_TESTWORLD=1 GE_COOP_WATCH=9,15,16,17 GE_COOP_STEPS=k9@20 GE_COOP_STEPSLINGER=70', '', 100, 2,
                 ['h:watch: alarm on', 'h:watch: chr 1[567] appears'], 1),
    # SI6: Agent, the joiner plants the charge: the 360 s countdown runs out and the host's blast
    # (AI_TriggerExplosionsAroundBond, 246) reaches the joiner, whose own copy then sets off more
    # than six explosions at once around its player (the Unlimited Explosions line). Both are
    # invulnerable, so the hurt itself is not measured; B fails on both
    'SI6': ('silo', 0, f'{INV} GE_COOP_WATCHWORLD=1',
            f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=h34@8,p4@10,f1@12 GE_COOP_STEPSLINGER=400', 420, 2,
            ['b:watch: countdown running', 'clocksame', 'j:host script command 246',
             'seqj:host script command 246=>more than the N64.s 6 at once', 'b:objective 4 \\(B\\) is now failed']),
    # T3: the joiner shoots Ourumov (chr 71, list 0x414) in the last car: stage flag 800, and 20 s
    # later the host's 60 s countdown, the same on both
    'T3': ('train', 0, f'{INV} GE_COOP_WATCHWORLD=1',
           f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=c71@10,s71@12 GE_COOP_STEPSLINGER=40', 60, 2,
           ['hflag:800', 'b:watch: countdown running', 'clocksame']),
    # T4: the same run to the end of the countdown: the end flag (setup 0x00000800, 80000 in the
    # stage-flags line), the host's blasts reach the joiner, and the car parts they break (tags 41
    # and 42 on the host's object watch) are broken on both at 80 s
    'T4': ('train', 0, f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_WATCHOBJS=1 GE_COOP_TESTDUMP=80',
           f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_WATCHOBJS=1 GE_COOP_TESTDUMP=80 GE_COOP_STEPS=c71@10,s71@12 GE_COOP_STEPSLINGER=100', 120, 2,
           ['hflag:80000', 'jflag:80000', 'j:host explosion \\(type 13\\)',
            'b:dump: tag 41 .* destroyed [1-9]', 'b:dump: tag 42 .* destroyed [1-9]']),
    'X5': ('dam', 0, f'{INV} GE_COOP_TESTPAD=330:10 GE_COOP_TESTWIN=1', INV, 90, 24,
           ['=j:mission end', '=h:mission end']),
    'X8-doak3': ('facility', 1, INV, INV, 90, 2,
                 ['p3:step c79 at', '!p1:objective 1 \\(B\\) is now failed'], 3, {3: f'{INV} GE_COOP_STEPS=p11@10,c79@25'}),
    'X8-nat4': ('statue', 0, f'{INV} GE_COOP_TESTFLAG=8000:5', INV, 100, 2,
                ['p1:objective 3 \\(D\\) is now complete'], 4, {4: f'{INV} GE_COOP_TESTGOTO=0'}),
}


def add_sweep():
    """SW-<stage>: the joiner breaks or collects every destroy/collect target (objs.py, sweep.py), 00 Agent."""
    objs = subprocess.run([sys.executable, os.path.join(HERE, 'objs.py')], capture_output=True, text=True).stdout
    tmp = os.path.join(os.environ.get('TMPDIR', '/tmp'), 'rows-objectives.txt')
    open(tmp, 'w').write(objs)
    lines = subprocess.run([sys.executable, os.path.join(HERE, 'sweep.py'), tmp], capture_output=True, text=True).stdout
    for line in lines.split('\n'):
        if line.count('|') == 2:
            st, steps, secs = line.split('|')
            ROWS['SW-' + st] = (st, 2, INV, f'{INV} GE_COOP_STEPS={steps}', int(secs), 2, ['objsame'])


def objstates(text):
    final, seq = {}, []
    for m in re.finditer(r'objective (\d+) \((\w)\) is now (\w+)', text):
        final[m.group(2)] = m.group(3)
        seq.append((m.group(2), m.group(3)))
    return final, seq


def doorsopen(text):
    """The open doors the first watch line lists (GE_COOP_WATCHDOORS)."""
    m = re.search(r'watch: \d+ doors, open: (\S+)', text or '')
    return set() if m is None or m.group(1) == 'none' else set(m.group(1).split(','))


def special(c, L):
    h, j = L.get('h') or '', L.get('j') or ''
    op = re.search(r'testopen: door (\d+) -> open', h)
    if c == 'doorsame':
        # the host's doors when it started the join snapshot the joiner applied, against the
        # joiner's once that snapshot is in and no door of its moves (GE_COOP_WATCHDOORS).
        # Only doors standing still count: a door moving on the host at the snapshot, or one
        # the host's watch saw change between the snapshot and the joiner's list, is left out
        if op is None:
            return ['the host opened no door (testopen)']
        d = op.group(1)
        ap = re.search(r'watch: snapshot (\d+) applied', j)
        if ap is None:
            return ['the joiner logged no applied snapshot']
        sid = ap.group(1)
        hs = re.search(rf'watch: snapshot {sid} sent: open (\S+), moving (\S+), clock ([\d.]+)', h)
        js = re.search(rf'watch: snapshot {sid} (settled|never settled): open (\S+), moving (\S+), clock ([\d.]+)', j)
        if hs is None:
            return [f'the host logged no snapshot {sid}']
        if js is None:
            return [f'the joiner logged no door list after snapshot {sid}']
        ids = lambda v: set() if v == 'none' else set(v.split(','))
        hopen, hmov, jopen = ids(hs.group(1)), ids(hs.group(2)), ids(js.group(2))
        t0, t1 = float(hs.group(3)), float(js.group(4))
        changed = {x for x, w in re.findall(r'watch: door (\d+) (?:open|closed), clock ([\d.]+)', h) if t0 - 1.5 <= float(w) <= t1 + 1.5}
        skip = hmov | changed
        why = []
        if js.group(1) != 'settled':
            why.append(f'doors still moving on the joiner 15 s after snapshot {sid}: {js.group(3)}')
        if d not in hopen or d in skip:
            why.append(f'door {d} was not standing open on the host from snapshot {sid} to the joiner\'s list (no test)')
        if (hopen ^ jopen) - skip:
            why.append(f'snapshot {sid}: host open {sorted(hopen)}, joiner open {sorted(jopen)}'
                       f' (left out: {sorted(skip) or "none"})')
        return why
    if c == 'doorkept':
        # the door the old host opened stays open on the new host after the hand-over
        if op is None:
            return ['the host opened no door (testopen)']
        d = op.group(1)
        mig = j.find('host migration: player 2 hosts')
        if mig < 0:
            return ['no migration on the joiner']
        before, after = j[:mig], j[mig:]
        seen = re.findall(rf'watch: door {d} (open|closed)', before)
        if (seen[-1:] or [None])[0] != 'open' and d not in doorsopen(before):
            return [f'door {d} was not open on the joiner before the hand-over']
        if re.search(rf'watch: door {d} closed', after):
            return [f'door {d} closed on the new host after the hand-over']
        return []
    if c == 'clocksame':
        # the joiner's countdown at a moment against the host's at that moment (linear between
        # the host's samples): within 0.5 s. GE_COOP_WATCHWORLD stamps each line with the
        # machine's monotonic clock, shared by copies on one machine
        rx = r'watch: countdown running ([\d.]+) s at mission \d+ s, clock ([\d.]+)'
        hv = sorted((float(w), float(v)) for v, w in re.findall(rx, h))
        jv = [(float(w), float(v)) for v, w in re.findall(rx, j)]
        pairs = []
        for w, v in jv:
            for (w0, v0), (w1, v1) in zip(hv, hv[1:]):
                if w0 <= w <= w1 and w1 - w0 < 15:
                    pairs.append((round(w - hv[0][0], 1), v, v0 + (v1 - v0) * (w - w0) / (w1 - w0)))
                    break
        if not pairs:
            return ['no joiner countdown sample between two host samples']
        off = [(t, jvv, round(hvv, 1)) for t, jvv, hvv in pairs if abs(jvv - hvv) > 0.5]
        return [f'countdown apart (s after the host started, joiner, host): {off[:3]}'] if off else []
    m = re.match(r'seq([hjb]):(.+?)=>(.+)$', c)
    if m:
        # seqj:<A>=><B>: on that side a line matching <A> comes before the first line matching <B>
        why = []
        for w in (('h', 'j') if m.group(1) == 'b' else (m.group(1),)):
            t = L.get(w) or ''
            b = re.search(m.group(3), t)
            if b is None:
                why.append(f'{w} log lacks "{m.group(3)}"')
            elif re.search(m.group(2), t[:b.start()]) is None:
                why.append(f'{w}: no "{m.group(2)}" before "{m.group(3)}"')
        return why
    m = re.match(r'cd([hjb]):([\d.]+):(.+)$', c)
    if m:
        # cdb:<N>:<regex>: on that side the first line matching <regex> comes while a countdown
        # runs and once it is under N s: a running countdown is logged before the line, and
        # the first one logged after it reads under N (the countdown only goes down, so the
        # line came at a value between the two). A script that checks "countdown under N s"
        # before its countdown has run that far fails it, even if a countdown runs later
        # (AZ4: the launch caption and the ending came first). With no countdown logged after
        # the line (the mission ended), the last one before it must be under N + 3
        why = []
        rx = r'watch: countdown running ([\d.]+) s'
        lim = float(m.group(2))
        for w in (('h', 'j') if m.group(1) == 'b' else (m.group(1),)):
            t = L.get(w) or ''
            at = re.search(m.group(3), t)
            if at is None:
                why.append(f'{w} log lacks "{m.group(3)}"')
                continue
            before = [float(v) for v in re.findall(rx, t[:at.start()])]
            after = re.search(rx, t[at.end():])
            if not before:
                why.append(f'{w}: "{m.group(3)}" with no countdown running before it')
            elif after is not None and float(after.group(1)) >= lim:
                why.append(f'{w}: "{m.group(3)}" before the countdown was under {m.group(2)} s'
                           f' (between {before[-1]} and {after.group(1)} s)')
            elif after is None and before[-1] >= lim + 3:
                why.append(f'{w}: "{m.group(3)}" before the countdown was under {m.group(2)} s (last {before[-1]} s)')
        return why
    m = re.match(r'span([hj]):(\d+):([\d.]+)$', c)
    if m:
        # spanj:<chr>:<s>: that chr shows at least once on that side (GE_COOP_WATCH), and each
        # showing, appears to gone, lasts at most <s> s (sampled once a second)
        t = L.get(m.group(1)) or ''
        ev = re.findall(rf'watch: chr {m.group(2)} (appears|gone)[^\n]*, at (\d+) s', t)
        spans, start = [], None
        for what, at in ev:
            if what == 'appears':
                start = int(at)
            elif start is not None:
                spans.append(int(at) - start)
                start = None
        if not spans:
            return [f'{m.group(1)}: chr {m.group(2)} never showed and went']
        long_ = [x for x in spans if x > float(m.group(3))]
        return [f'{m.group(1)}: chr {m.group(2)} showings of {long_} s (of {len(spans)})'] if long_ else []
    m = re.match(r'(!?)([hj])flag:([0-9a-f]+)$', c)
    if m:
        # hflag:<hex>: some stage-flags line (GE_COOP_WATCHWORLD) on the host has those bits; !hflag: none has.
        # The line shows the objective flags: a setup file's argument with its four bytes reversed
        # (its 0x00010000 is 100 here, 0x00080000 is 800, 0x00000800 is 80000)
        bits = int(m.group(3), 16)
        seen = any(int(x, 16) & bits == bits for x in re.findall(r'watch: stage flags ([0-9a-f]+)', L.get(m.group(2)) or ''))
        if m.group(1) and seen:
            return [f'{m.group(2)} stage flags had {m.group(3)}']
        if not m.group(1) and not seen:
            return [f'{m.group(2)} stage flags never had {m.group(3)}']
        return []
    return [f'unknown check {c}']


def logs_of(d, players):
    if players == 1:
        return {'h': os.path.join(d, 'host.log')}
    if players > 2:
        return {f'p{i}': os.path.join(d, f'p{i}.log') for i in range(1, players + 1)}
    return {'h': os.path.join(d, 'host.log'), 'j': os.path.join(d, 'join.log')}


def read(p):
    try:
        return open(p, errors='replace').read()
    except OSError:
        return ''


def judge(name, d, checks, players, je=''):
    L = {k: read(v) for k, v in logs_of(d, players).items()}
    if players == 2:
        L['b'] = None
    host = L.get('h', L.get('p1', ''))
    joiners = [v for k, v in L.items() if k not in ('h', 'p1', 'b')]
    why = []
    if not all('join snapshot applied' in j or 'following the host into stage' in j for j in joiners):
        why.append('a joiner never got in')
    alltext = ''.join(v for v in L.values() if v)
    if 'unresolved desync' in alltext:
        why.append('unresolved desync')
    if re.search(r'segmentation|signal 11|assertion', alltext, re.I):
        why.append('crash in a log')
    for c in checks:
        if c == 'objsame':
            (hf, hs), (jf, js) = objstates(L['h']), objstates(L['j'])
            for k in sorted(set(hf) | set(jf)):
                if hf.get(k) != jf.get(k):
                    why.append(f'objective {k}: host {hf.get(k)}, joiner {jf.get(k)}')
            flash = sorted({k for k, v in js if v == 'failed' and ('failed' not in [w for kk, w in hs if kk == k])})
            if flash:
                why.append('joiner-only failed flash: ' + ','.join(flash))
            if not hf:
                why.append('no objective changed')
            want = set(re.findall(r'(?:^|,)(\w+)@', (re.search(r'GE_COOP_STEPS=(\S+)', je or '') or [None, ''])[1]))
            done = set(re.findall(r'step (\w+) at \d+ s done', L['j']))
            if want - done:
                why.append(f"joiner's steps not all done (missing {','.join(sorted(want - done))})")
            continue
        if c in ('doorsame', 'doorkept', 'clocksame') or re.match(r'!?[hj]flag:|cd[hjb]:|seq[hjb]:|span[hj]:', c):
            why += special(c, L)
            continue
        neg = c.startswith('!')
        exact = c.startswith('=')
        cc = c[1:] if (neg or exact) else c
        who, rx = cc.split(':', 1)
        if who == 'b':
            texts = [('h', L['h']), ('j', L['j'])]
        else:
            texts = [(who, L.get(who, ''))]
        for w, t in texts:
            n = len(re.findall(rx, t or ''))
            if neg and n:
                why.append(f'{w} log has "{rx}"')
            elif exact and n != 1:
                why.append(f'{w} log has "{rx}" {n} times, not once')
            elif not neg and not exact and n == 0:
                why.append(f'{w} log lacks "{rx}"')
    return why


def run(name, out, slot):
    spec = ROWS[name]
    st, diff, he, je, secs, ja, checks = spec[:7]
    players = spec[7] if len(spec) > 7 else 2
    extra = spec[8] if len(spec) > 8 else {}
    d = os.path.join(out, name)
    os.makedirs(d, exist_ok=True)
    env = dict(os.environ, STAGE=st, DIFF=str(diff), SECS=str(int(secs * float(os.environ.get('SECSX', '2.5')))), SLOT=str(slot), JOINAFTER=str(ja))
    if players == 1:
        env.update(HOSTENV=he)
        script = os.path.join(HERE, 'solo.sh')
    elif players > 2:
        env.update(PLAYERS=str(players), P1ENV=he)
        for i in range(2, players + 1):
            env[f'P{i}ENV'] = extra.get(i, je)
        script = os.path.join(HERE, 'party.sh')
    else:
        env.update(HOSTENV=he, JOINENV=je)
        script = os.path.join(HERE, 'pair.sh')
    subprocess.run([script, d], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    why = judge(name, d, checks, players, je)
    return f"{name:9s} {'PASS' if not why else 'FAIL: ' + '; '.join(why)}"


def main():
    rejudge = sys.argv[1] == '--judge'
    if rejudge:
        del sys.argv[1]
    out = sys.argv[1]
    add_sweep()
    names = sys.argv[2:] or list(ROWS)
    if names == ['sweep']:
        names = [n for n in ROWS if n.startswith('SW-')]
    bad = [n for n in names if n not in ROWS]
    if bad:
        sys.exit('no such row: ' + ' '.join(bad))
    par = int(os.environ.get('PAR', '2'))
    os.makedirs(out, exist_ok=True)
    if rejudge:
        for n in names:
            spec = ROWS[n]
            why = judge(n, os.path.join(out, n), spec[6], spec[7] if len(spec) > 7 else 2, spec[3])
            print(f"{n:9s} {'PASS' if not why else 'FAIL: ' + '; '.join(why)}")
        return
    slots = queue.Queue()
    for i in range(par):
        slots.put(i)

    def one(name):
        slot = slots.get()
        try:
            return run(name, out, slot)
        finally:
            slots.put(slot)

    with ThreadPoolExecutor(par) as ex:
        res = list(ex.map(one, names))
    with open(os.path.join(out, 'summary.txt'), 'a') as f:
        for r in res:
            print(r)
            f.write(r + '\n')


if __name__ == '__main__':
    main()
