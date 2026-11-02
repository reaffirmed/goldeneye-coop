# Checklist rig

Scripts used to run `coop/TEST-CHECKLIST.md` in two (or more) copies on one
Linux machine over the mock Steam. The game needs the harness in
`src/game/coop_checklist.c` (GE_COOP_STEPS, GE_COOP_WATCH, GE_COOP_WATCHWORLD,
GE_COOP_TESTDUMP; see the top of that file).

Build as in `.github/workflows/coop-missions.yml` (build_pc with
Data/baserom.u.z64 and mock/libsteam_api.so), then from the repo root:

    # one pair: host env, joiner env
    STAGE=facility DIFF=1 HOSTENV="GE_CHEATS=invuln" \
      JOINENV="GE_CHEATS=invuln GE_COOP_STEPS=p11@10,c79@25" coop/ci/checklist/pair.sh out/doak
    # 3 or 4 players
    PLAYERS=3 STAGE=dam P3ENV="GE_COOP_STEPS=p330@10" coop/ci/checklist/party.sh out/dam3
    # a host on its own (is it the same in solo?); GE_COOP_REVIVE=1 logs chr
    # slots freed and filled and any chr leaving ACT_DIE
    STAGE=jungle HOSTENV="GE_COOP_REVIVE=1 GE_COOP_STEPS=o0@12,b0@14" coop/ci/checklist/solo.sh out/jsolo
    # every mission: the joiner breaks or collects each objective target
    python3 coop/ci/checklist/objs.py > objectives.txt
    python3 coop/ci/checklist/sweep.py objectives.txt   # lines for batch.sh
    python3 coop/ci/checklist/cmp.py out               # host vs joiner objective states

A pair stops on its own once the steps are done ("steps: finished").

Rows by name (`rows.py`): each row of TEST-CHECKLIST.md that has a rig line is a
named entry there, with the log lines that decide it. It runs pair.sh, party.sh
or solo.sh for you, PAR pairs at once, and prints PASS or the check that failed:

    python3 coop/ci/checklist/rows.py out/rows D2 F5 X3     # some rows
    python3 coop/ci/checklist/rows.py out/rows sweep        # the objective sweep, one SW-<stage> per mission
    python3 coop/ci/checklist/rows.py --judge out/rows D2   # read old logs again, no run

Keep PAR at most half the cores (default 2). The steps are timed in mission
seconds, and on a loaded machine software GL runs the game well below full
speed: with 3 pairs on 4 cores the sweep's steps never got to run before the
pair was killed. SECSX (default 2.5) is how much wall time a pair gets per
mission second. A FAIL is a lead, not a verdict: read the logs first, since a
step that missed its target also fails a check.

Door and countdown rows use three more harness switches (src/game/coop.c,
src/game/coop_checklist.c; env only, nothing changes without them):

- `GE_COOP_WATCHDOORS=1` logs the open doors once (a joiner waits for its join
  snapshot), then each door that opens or closes. A door's number is its byte
  offset in the setup, the same number `testdoor` and `testopen` print.
- `GE_COOP_TESTOPEN=<s>`: the host opens the closed door nearest it `<s>` mission
  seconds in, partner or not, so a drop-in finds a door already open (X3b, X4b).
- Step `e` presses Use on the door nearest the player, keys and locks as in play
  (DE3's safe door).

`GE_COOP_WATCHWORLD` stamps each countdown line with the mission second and the
machine's monotonic clock, which copies on one machine share. Mission seconds
start when each copy loads the mission, so a joiner that came in later is
seconds behind: compare countdowns by the clock (the `clocksame` check), not by
mission second. The stage-flags line shows the objective flags, the AI script's
bits shifted right by 8 (the script's 0x10000 is 100 there), which is what the
`hflag:`/`jflag:` checks read.

The watch logs a running countdown once a mission second. `cdb:<N>:<regex>` (or
`cdh:`, `cdj:`) needs the first line matching `<regex>` to come while a countdown
runs and once it is under N s: a running countdown before the line, and the first
one after it under N (AZ4: "Final checks cleared" is `cdb:10:`). It fails a script
that reaches the line before its countdown has run that far, even if a countdown
runs afterwards. `seqj:<A>=><B>` needs a line matching `<A>` before the first `<B>`
(AZ4: the countdown shows on the joiner before the launch caption).

`GE_COOP_TESTOPEN=join` opens the door when a partner first connects, so it is open
and still at the join snapshot a joiner applies once its intro is over (X3b).
`GE_COOP_WATCHDOORS` also logs the open and the moving doors when the host starts a
join snapshot and when the joiner has applied it, tagged with the snapshot, and the
joiner's doors again once none moves. `doorsame` compares the host's still doors at
the snapshot the joiner applied with the joiner's settled ones, leaving out doors
the host's watch saw change in between; if the test door itself moved, the run is
no test and has to be run again. The clock stamps that window compares come from each
copy's own monotonic clock, so `doorsame` and `clocksame` hold only when host and
joiner run on one machine, as on the rig, not across two PCs.
