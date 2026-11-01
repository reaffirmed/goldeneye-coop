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
