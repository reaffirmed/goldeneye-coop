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
