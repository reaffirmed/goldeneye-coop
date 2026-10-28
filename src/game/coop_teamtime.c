/**
 * Story co-op: the team's mission time.
 *
 * Each game runs its own mission timer from when it got into the mission,
 * so a partner who joined late, or loaded slower, would finish with a
 * different time than the host. In co-op the mission's time is the host's:
 * the debrief shows it, the folder's best time keeps it, and the target time
 * cheats (solo_target_time_array) are judged on it, so a team can still
 * unlock the original cheats together.
 *
 *   - the host sends its timer once a second (COOPPKT_TEAMTIME); a joiner
 *     runs it on with its own clock between packets, which is its estimate
 *   - when the host ends the mission it sends the exact time (COOPEV_TEAMTIME)
 *     before MISSIONEND on the same reliable channel, so a joiner has it
 *     before its own debrief runs
 *   - when a joiner ends it, the joiner debriefs on its estimate at once; the
 *     host's exact time follows, and if it beats a target time the estimate
 *     missed (or improves the best time) the joiner saves that too
 *
 * The 007 difficulty and cheats-on runs keep the game's own rules (no times
 * are saved for those).
 */
#include <ultra64.h>
#include <string.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <boss.h>
#include "front.h"
#include "file.h"
#include "file2.h"
#include "coop.h"

extern void portConLogf(const char *fmt, ...);
extern s32 getMissiontimer(void);
extern bool portFileGetIsCheatUnlockedRaw(save_data *save, s32 cheat);
extern save_data *fileGetSaveForFoldernum(s32 foldernum);

typedef struct
{
    s32 stage;      /* LEVELID */
    s32 timer;      /* the host's mission timer, 60 Hz */
    s32 final;      /* 1: as the mission ended */
} CoopTeamTimeMsg;

typedef struct
{
    u32 magic;
    u16 version;
    u16 type;
    CoopTeamTimeMsg msg;
} CoopTeamTimePkt;

static s32 sHostTimer = -1;     /* joiner: the host's last timer ... */
static s32 sHostStage;
static s32 sMineAt;             /* ... and ours when it came */
static s32 sExact = -1;         /* joiner: the host's final time */
static s32 sExactStage;
static s32 sFrozen = -1;        /* the team's time as the mission ended, for the menus */
static s32 sUsedFolder = -1;    /* the debrief saved with this time (coopTeamTimeUsed) */
static s32 sUsedStage;
static s32 sUsedLevel;
static s32 sUsedDiff;
static s32 sUsedTimer;
static s32 sAdopt;              /* a joiner that took over as host: the old host's time minus ours */
static s32 sFollowed;           /* joiner: we have the host's time for this mission */

/* our own mission timer, on the old host's clock after a migration */
static s32 coopTeamTimeOwn(void)
{
    return getMissiontimer() + sAdopt;
}

static s32 coopTeamTimeLive(void)
{
    const s32 stage = bossGetStageNum();

    if (!coopLobbyHasPartner() || coopExtIsHost())
    {
        return coopTeamTimeOwn();
    }
    if (sExact >= 0 && sExactStage == stage)
    {
        return sExact;
    }
    if (sHostTimer >= 0 && sHostStage == stage)
    {
        return sHostTimer + (getMissiontimer() - sMineAt);
    }
    return getMissiontimer();
}

s32 coopTeamTimer(void)
{
    if (bossGetStageNum() == LEVELID_TITLE && sFrozen >= 0)
    {
        return sFrozen;
    }
    return coopTeamTimeLive();
}

void coopTeamTimeFreeze(void)
{
    sFrozen = coopTeamTimeLive();
}

void coopTeamTimeSend(void)
{
    CoopTeamTimeMsg m;

    if (!coopExtIsHost() || !coopLobbyHasPartner())
    {
        return;
    }
    m.stage = bossGetStageNum();
    m.timer = coopTeamTimeOwn();
    m.final = 1;
    coopExtSend(COOPEV_TEAMTIME, 0xFF /* COOP_ALL */, &m, sizeof(m));
    portConLogf("ge: coop: team time %d.%02d s sent\n", m.timer / 60, (m.timer % 60) * 100 / 60);
}

/* joiner: the host's exact time came after our debrief saved an estimate */
static void coopTeamTimeCorrect(void)
{
    s32 secs = sExact / 60;
    s32 target;
    save_data *save;

    if (sUsedFolder < 0 || sUsedLevel != sExactStage || sUsedTimer == sExact)
    {
        return;
    }
    portConLogf("ge: coop: team time %d s replaces our estimate %d s\n", secs, sUsedTimer / 60);
    fileUnlockStageInFolderAtDifficulty(sUsedFolder, sUsedStage, sUsedDiff, secs);  /* kept only if better */
    target = solo_target_time_array[sUsedStage][sUsedDiff];
    save = fileGetSaveForFoldernum(sUsedFolder);
    if (secs <= target && save != NULL && !portFileGetIsCheatUnlockedRaw(save, sUsedStage))
    {
        fileSaveFolderUnlockCheat(sUsedFolder, sUsedStage);
        g_NewCheatUnlocked = TRUE;
        portConLogf("ge: coop: the team time beat the target: cheat %d unlocked\n", sUsedStage);
    }
    sUsedTimer = sExact;
    if (sFrozen >= 0)
    {
        sFrozen = sExact;
    }
}

void coopTeamTimeApply(const u8 *raw, s32 from)
{
    CoopTeamTimeMsg m;

    memcpy(&m, raw, sizeof(m));
    if (from != 0 || coopExtIsHost() || m.timer < 0)
    {
        return;
    }
    if (m.final)
    {
        sExact = m.timer;
        sExactStage = m.stage;
        portConLogf("ge: coop: team time %d.%02d s from the host (our own clock %d.%02d s)\n", m.timer / 60,
                    (m.timer % 60) * 100 / 60, getMissiontimer() / 60, (getMissiontimer() % 60) * 100 / 60);
        if (bossGetStageNum() == LEVELID_TITLE)
        {
            coopTeamTimeCorrect();
        }
        return;
    }
    if (m.stage == bossGetStageNum())
    {
        sHostTimer = m.timer;
        sHostStage = m.stage;
        sMineAt = getMissiontimer();
    }
}

void coopTeamTimeUsed(s32 folder, s32 stage, s32 difficulty, s32 timer)
{
    if (!coopLobbyHasPartner())
    {
        sUsedFolder = -1;
        return;
    }
    sUsedFolder = folder;
    sUsedStage = stage;
    sUsedLevel = bossGetStageNum();
    sUsedDiff = difficulty;
    sUsedTimer = timer;
}

void coopTeamTimeTick(s32 inmission)
{
    if (!inmission)
    {
        return;
    }
    if (coopExtMissionTicks() <= 1)
    {   /* a new mission */
        sFrozen = -1;
        sExact = -1;
        sHostTimer = -1;
        sUsedFolder = -1;
        sAdopt = 0;
        sFollowed = 0;
    }
    if (!coopExtIsHost())
    {
        sFollowed = sHostTimer >= 0 && sHostStage == bossGetStageNum();
    }
    else if (sFollowed)
    {   /* the host left and we took over: the mission's time goes on from the old host's,
         * not from when we got in (a late joiner's own timer started later) */
        sAdopt = sHostTimer - sMineAt;
        sFollowed = 0;
        portConLogf("ge: coop: team time goes on from the old host's, %d s (ours %d s)\n",
                    coopTeamTimeOwn() / 60, getMissiontimer() / 60);
    }
    if (coopExtIsHost() && coopLobbyHasPartner() && (coopExtMissionTicks() % 60) == 30)
    {
        CoopTeamTimePkt p;

        memset(&p, 0, sizeof(p));
        p.type = COOPPKT_TEAMTIME;
        p.msg.stage = bossGetStageNum();
        p.msg.timer = coopTeamTimeOwn();
        coopExtSendPacket(&p, sizeof(p), 0xFF /* COOP_ALL */);
    }
}
