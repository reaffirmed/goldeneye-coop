/**
 * Smarter guards (port only): an optional guard AI, picked by the Guard AI
 * row (COOPSET_GUARDAI, the host's, in co-op) or the guard_ai Port Option
 * (solo). Classic, the default, never reaches any of this.
 *
 * Only the generic guards change: a chr whose script is one of the shared
 * global lists (Standard Guard, Simple Guard, the clones' and the chase
 * lists, or Attack / Run To Bond called from one of those) and that holds a
 * gun. Level scripts, named characters, civilians and unarmed chrs keep
 * Classic, and no AI command means anything new. A level script that gives
 * one of ours another list takes it back at once.
 *
 * While nothing has happened the guard runs its Classic list (idle
 * animations, patrols, the Classic sight and hearing checks). The moment
 * that list reacts (it jumps to Attack, Run To Bond or the clone list), the
 * brain here takes over:
 *
 *   investigate  heard a shot, saw a guard hit or killed, found a body, or
 *                answering a call: look round, walk (run, if called) to the
 *                place, check the waypoints around it, walk back to its post
 *   combat       first sight calls up to COOPAI_CALL_MAX calm guards nearby
 *                and maybe sends one to the nearest working alarm; then
 *                bursts of Classic Attack Bond from where it stands, a move
 *                to a spot the target can't see after a few bursts or a
 *                hit, a crouch there, and a step back out to fire again;
 *                losing sight sends it to the last place it saw the target,
 *                or round the side (a flank) when a squadmate still sees
 *                them; badly hurt, it falls back to cover once
 *   several Bonds  a guard on a player that half the fight is already on
 *                turns to another player it can see; one that sees a player
 *                reviving a downed partner turns to them
 *   suppression    a guard being shot at keeps its head down in cover and
 *                leaves the open for cover sooner; one whose target reloads
 *                pushes out of cover or round the corner at once
 *   grenades       a target that has held the same spot a while, that the
 *                guard knows of, gets a grenade (a point first, so it is fair)
 *   clues        a calm guard goes to look at a door a player left open,
 *                something smashed, or running footsteps close by
 *   roles        riflemen, rushers (close in, flank) and anchors (hold, cover)
 *   wounds       badly hurt it walks; worse, alone and cornered, it may
 *                surrender (hands up, gun dropped)
 *   regrouping   with two of its own just killed near it, back to where the
 *                other guards are, to make a stand there
 *   cut-offs     a player who has strayed from their partners: a guard gets
 *                between them
 *
 * Alert levels, one for the mission (calm, suspicious, alerted, lockdown):
 * a heard shot makes it suspicious, a player seen or a body found alerted,
 * the alarm or a second body lockdown. Suspicious calms down after a while;
 * alerted only falls back to suspicious, and lockdown stays. The higher it
 * is, the longer guards search, the sooner they run, and from alerted on
 * some hold where their search ended instead of walking back to post; in
 * lockdown calm guards set out on sweeps and hear a little further.
 *
 * Bursts are the Classic Attack Bond list itself (fire, kneel, roll, side
 * step, grenade), so the fighting looks like GoldenEye; the brain only
 * decides where to stand. Hearing is muffled by walls (coopAiHearScale).
 *
 * The host runs it (joiners only mirror guards), so every new move reaches
 * the other screens through the guard snapshots, and doors and alarms the
 * guards use go through the usual events. The brain's own state is not sent:
 * after a host migration each guard starts over from what its chr record
 * remembers (last seen, last heard), which is enough to pick the fight up.
 */
#include <ultra64.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <bondaicommands.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <random.h>
#include <boss.h>
#include "math_atan2f.h"
#include "chr.h"
#include "chrai.h"
#include "chraction.h"
#include "lv.h"
#include "model.h"
#include "propobj.h"
#include "stan.h"
#include "coop.h"

extern void portConLogf(const char *fmt, ...);
extern s32 portGuardAiSmarter(void);    /* video.c: midas.ini guard_ai */
extern s32 portGuardBarks(void);        /* video.c: midas.ini guard_barks */
extern s32 portGuardAlertness(void);    /* video.c: midas.ini guard_alertness, percent */
extern s32 portGuardBudget(void);       /* video.c: midas.ini guard_budget, 101 no limit */
extern int ge_shot_force;               /* gfx_sdl2.cpp: a screenshot this frame, to ge_shot_path */
extern char ge_shot_path[512];
extern s32 portOptFind(const char *key);
extern s32 portOptSetValue(s32 i, s32 v);
extern int coopNetIsHost(void);

extern s32 plot_course_for_actor(ChrRecord *self, coord3d *pos, StandTile *stan, SPEED speed);
extern s32 chrIsNotDeadOrShot(ChrRecord *chr);
extern s32 chrlvFindPathNeighborRelated(coord3d *bondpos, StandTile *stan, f32 rot, u8 quadrant);
extern waypoint *chrlvStanPathRelated(coord3d *pos, StandTile *stan);
extern bool doorIsClosed(DoorRecord *door);

#define COOPAI_MAX_CHRS     256
#define COOPAI_SEC          60          /* g_GlobalTimer ticks */
#define COOPAI_CALL_RANGE   2000.0f     /* 20 m: who hears a guard call for help */
#define COOPAI_CALL_NEAR    800.0f      /* 8 m: close enough to hear it round a corner */
#define COOPAI_CALL_MAX     3           /* guards that come running per call */
#define COOPAI_BODY_RANGE   1500.0f     /* 15 m: a body a guard on post notices */
#define COOPAI_ALARM_RANGE  3000.0f     /* 30 m: an alarm worth running to */
#define COOPAI_COVER_NEAR   150.0f      /* cover spots from 1.5 m ... */
#define COOPAI_COVER_FAR    900.0f      /* ... to 9 m away */
#define COOPAI_COVER_TESTS  40          /* sight tests per cover search */
#define COOPAI_SEARCHES     2           /* cover searches per tick, every guard together */
#define COOPAI_MUFFLED      0.5f        /* hearing through walls */
#define COOPAI_BODIES       8           /* bodies a guard remembers having seen */
#define COOPAI_CAMP_DIST    250.0f      /* 2.5 m: a player who stays this close to one spot is holding it */
#define COOPAI_CAMP_SECS    6           /* ... for this long: a camper, worth a grenade */
#define COOPAI_NADE_NEAR    400.0f      /* grenades from 4 m ... */
#define COOPAI_NADE_FAR     2000.0f     /* ... to 20 m */
#define COOPAI_NADE_GAP     8           /* seconds between any two guards' grenades */
#define COOPAI_NADE_AGAIN   15          /* ... and between two at the same player */
#define COOPAI_NADE_CLEAR   800.0f      /* no grenade with anyone not ours this close to the target */
#define COOPAI_NADE_FIGHT   4           /* a guard fights a camper this long before it throws */
#define COOPAI_SUPPRESS     20          /* ticks after a shot from its target that a guard counts as shot at */
#define COOPAI_SUPPRESS_RANGE 2000.0f   /* ... from within 20 m */
#define COOPAI_SUPPRESS_ARC 0.26f       /* ... facing within 15 degrees of it */
#define COOPAI_RELOAD_PUSH  90          /* ticks after its target starts a reload that a guard pushes */
#define COOPAI_SWEEP_GAP    20          /* lockdown: seconds between sweeps */
#define COOPAI_SWEEPERS     2           /* ... and at most this many out at once */
#define COOPAI_CLUES        16          /* doors players opened and things smashed, for guards to notice */
#define COOPAI_CLUE_SECS    60          /* ... for this long after */
#define COOPAI_CLUE_RANGE   1500.0f     /* ... from 15 m, in sight */
#define COOPAI_WRECKS       96          /* smashed objects already known of */
#define COOPAI_STEP_RANGE   500.0f      /* running footsteps heard from 5 m (half that through a wall) */
#define COOPAI_STEP_SPEED   4.0f        /* units a tick: faster than this is running */
#define COOPAI_STEP_TICKS   15          /* the speed is measured over this many ticks */
#define COOPAI_LIMP         0.5f        /* this much of its health gone: it walks, it can't run */
#define COOPAI_GIVEUP       0.75f       /* ... this much, alone, a player close: it may surrender */
#define COOPAI_GIVEUP_NEAR  600.0f      /* ... within 6 m */
#define COOPAI_ALONE        1500.0f     /* no other guard of ours fighting within 15 m */
#define COOPAI_FALLEN       8           /* our guards' deaths remembered */
#define COOPAI_LOSING_RANGE 2000.0f     /* two of them dying within 20 m ... */
#define COOPAI_LOSING_SECS  20          /* ... in 20 s: the fight is going badly */
#define COOPAI_REGROUP_FAR  4000.0f     /* a regroup spot within 40 m */
#define COOPAI_REGROUP_NEAR 1000.0f     /* guards within 10 m of each other are one group */
#define COOPAI_SPLIT_DIST   1500.0f     /* a player 15 m from every partner is on their own */

enum { AIA_CALM = 0, AIA_SUSPICIOUS, AIA_ALERTED, AIA_LOCKDOWN, AIA_COUNT };

/* how a guard fights: most are riflemen; a rusher closes in and flanks, an
 * anchor holds its ground and covers (picked from the chrnum, so the same
 * guard fights the same way every time) */
enum { AIR_RIFLE = 0, AIR_RUSHER, AIR_ANCHOR, AIR_COUNT };
static const char *const sRoleNames[AIR_COUNT] = { "rifleman", "rusher", "anchor" };

enum { AIC_NONE = 0, AIC_DOOR, AIC_WRECK };

/* barks (COOPBARK_*): what a guard calls out, and the game's own sound that goes with it */
static const char *const sBarkText[COOPBARK_COUNT] = {
    "OVER THERE!\n", "WHERE DID THEY GO?\n", "NEED BACKUP!\n", "COVER ME!\n", "GRENADE!\n",
    "MAN DOWN!\n", "SOUND THE ALARM!\n", "FALL BACK!\n", "WHO'S THERE?\n", "DON'T SHOOT!\n",
};

static const s16 sBarkSting[COOPBARK_COUNT] = {
    CAMERA_BEEP1_SFX, BEEP_QUIET_SFX, RADIO_SFX, GUN_RIFLECOCK_SFX, BING_SFX,
    RADIO_SFX, ALARM_SWITCH_SFX, BEEP_QUIET_SFX, BEEP_QUIET_SFX, BEEP_QUIET_SFX,
};

#define COOPAI_BARKS        6           /* barks on show at once */
#define COOPAI_BARK_TICKS   120         /* each shows 2 s */
#define COOPAI_BARK_FADE    30          /* the last half second fading */
#define COOPAI_BARK_GAP     3           /* seconds between one guard's barks */
#define COOPAI_BARK_RANGE   4000.0f     /* shown out to 40 m */

static const char *const sAlertNames[AIA_COUNT] = { "calm", "suspicious", "alerted", "lockdown" };

enum
{
    AIS_POST = 0,       /* Classic list runs */
    AIS_LOOK,           /* look round before going to see */
    AIS_INVESTIGATE,    /* on the way to the place */
    AIS_SEARCH,         /* checking the waypoints around it */
    AIS_RETURN,         /* back to its post */
    AIS_COMBAT,         /* deciding, or a Classic burst under way */
    AIS_ADVANCE,        /* to where the target was last seen */
    AIS_COVER,          /* to a spot the target can't see */
    AIS_DUCK,           /* in cover, crouched */
    AIS_PEEK,           /* back out to where it fired from */
    AIS_FLANK,          /* round the target's side */
    AIS_ALARM,          /* to an alarm */
    AIS_FALLBACK,       /* hurt: to cover, away from the target */
    AIS_GRENADE,        /* pointing at a camper, then the throw */
    AIS_REGROUP,        /* the fight going badly: back to where the others are */
    AIS_CUTOFF,         /* between a player on their own and their partners */
    AIS_COUNT
};

static const char *const sStateNames[AIS_COUNT] = {
    "post", "look", "investigate", "search", "return", "combat", "advance",
    "cover", "duck", "peek", "flank", "alarm", "fall back", "grenade", "regroup", "cut off",
};

typedef struct
{
    ChrRecord *chr;         /* the chr this is about (slots are reused) */
    s16 chrnum;
    u8 state;
    u8 origin;              /* the Classic list it goes back to: GAILIST_STANDARD_GUARD or _SIMPLE_GUARD */
    u8 delegated;           /* a Classic Attack Bond burst is running */
    u8 called;              /* it has called for help this fight */
    u8 fellback;            /* it has fallen back this fight */
    u8 answering;           /* investigating because someone called */
    u8 bursts;              /* bursts since it last moved */
    u8 searchleft;          /* waypoints still to check */
    s8 target;              /* the player it fights (co-op), 0 our own Bond */
    s32 since60;            /* when the state began */
    s32 until60;            /* when it gives the state up */
    s32 lastsight60;        /* last tick it saw its target */
    s32 nextsplit60;        /* next look at the head count */
    s32 coverpad;           /* the pad it is going to or crouched at, -1 none */
    f32 lastdamage;         /* chr->damage at its last decision */
    s32 hit60;              /* last time that went up */
    s32 heard60;            /* the hearing it is investigating */
    s32 seen60;             /* chr->lastseetarget60 when it last calmed down */
    s8 arghs;               /* chr->numarghs then */
    coord3d home;
    StandTile *homestan;
    coord3d goal;
    StandTile *goalstan;
    coord3d lastpos;        /* where it last saw the target */
    StandTile *laststan;
    coord3d firepos;        /* where it fired from before taking cover */
    StandTile *firestan;
    s32 nextbody60;         /* next look round for bodies */
    s16 bodies[COOPAI_BODIES];  /* chrnum + 1 of the bodies it has seen, 0 none */
    u8 nextbodyslot;
    u8 threw;               /* it has thrown a grenade this fight */
    u8 sweeping;            /* lockdown: out on a sweep from its post */
    u8 heldown;             /* it has kept its head down in this duck (for the log) */
    s32 supp60;             /* last time its target shot at it */
    s32 pushed60;           /* the target's reload it last pushed on */
    s32 nextrevive60;       /* next look for a player reviving a partner */
    s32 fight60;            /* when this fight began */
    s32 bark60;             /* when it may bark again */
    u8 role;                /* AIR_* */
    u8 regrouped;           /* it has fallen back to the others this fight, and holds there */
    u8 surrendered;         /* hands up: the brain holds it there */
    u8 dead;                /* its death is counted */
    u8 limping;             /* (for the log) */
    u32 clueseen;           /* sClues slots it has seen, or doesn't care about */
} CoopAiBrain;

/* the players, as the guards keep track of them (indexes as coopChrCurrentTarget) */
typedef struct
{
    u8 valid;
    u8 shotseq;
    u8 cue;
    u8 reviving;
    s32 fired60;            /* the last tick they fired */
    s32 reload60;           /* the last tick they began a reload */
    s32 camp60;             /* since when they have stood about camppos */
    s32 nade60;             /* the last grenade thrown at them */
    f32 yaw;                /* where they face */
    coord3d camppos;
    coord3d steppos;        /* where they were COOPAI_STEP_TICKS ago */
    s32 step60;
    f32 speed;              /* units a tick over the last COOPAI_STEP_TICKS */
    StandTile *stan;
    coord3d pos;
} CoopAiPlayer;

static CoopAiBrain sBrain[COOPAI_MAX_CHRS];
static s32 sLatchStage = -1;    /* the stage, and the timer, the setting was read at */
static s32 sLatchTimer;
static s32 sLatchOn;
static s32 sSearchTick;         /* cover searches this tick */
static s32 sSearches;
static s32 sAlarmRunUntil;      /* one alarm runner at a time */
static CoopAiPlayer sPlayers[COOP_MAX_PLAYERS];
static s32 sPlayersTick = -1;
static s32 sAlert;              /* AIA_*: the mission's alert level */
static s32 sAlertFloor;         /* it never calms down below this again */
static s32 sAlertRaised60;      /* when it was last raised, or raised again */
static s32 sLastFight60;        /* the last tick a guard of ours was fighting */
static s32 sBodiesFound;
static s32 sNextSweep60;
static s32 sNextNade60;         /* no guard throws before this */
static struct
{
    s16 chrnum;
    u8 kind;
    s32 until;      /* g_GlobalTimer */
} sBarks[COOPAI_BARKS];         /* the callouts on show (every game) */
static s32 sBarkShotAt;         /* GE_GUARDAI_BARKSHOT: a screenshot then */
static s32 sBarkShots;
static struct
{
    u8 kind;        /* AIC_* */
    s32 at60;
    coord3d pos;    /* where to go and look */
    StandTile *stan;
    void *what;     /* the DoorRecord (a door shut again is no clue) */
} sClues[COOPAI_CLUES];
static s32 sNextClue;
static PropRecord *sWrecks[COOPAI_WRECKS];
static s32 sNumWrecks;
static s32 sNextWreckScan60;
static struct
{
    coord3d pos;
    s32 at60;
} sFallen[COOPAI_FALLEN];       /* where our guards died, and when */
static s32 sNextFallen;
static s32 sBudget;             /* escalations left this mission, -1 no limit */
static s32 sBudgetFull;

static void coopAiWreckScan(void);
static s32 coopAiAlertnessPct(void);

/* ---- the setting -------------------------------------------------------- */

static s32 coopAiDecideOn(void)
{
    if (!COOP_SMARTER_AI)
    {   /* paused: Classic even for a v0.6.0 host's Smarter (COOPSET_GUARDAI) */
        return 0;
    }
    if (coopVersus())
    {
        return 0;
    }
    if (coopSessionActive() && !coopNetIsHost())
    {   /* a joiner goes by the host's choice, so a migration mid-mission
         * carries on with the guards the mission started with */
        return coopSetting(COOPSET_GUARDAI) == COOPAI_SMARTER;
    }
    /* only the host (or a solo game) runs guards, so its own option decides */
    return coopAiSmarterOption() == COOPAI_SMARTER;
}

/* the Guard AI Port Option; GE_GUARDAI=0|1 stands in for midas.ini in the harness */
s32 coopAiSmarterOption(void)
{
    const char *e = getenv("GE_GUARDAI");

    if (!COOP_SMARTER_AI)
    {   /* paused: a saved guard_ai=1 is kept in midas.ini but not used */
        return COOPAI_CLASSIC;
    }
    if (e != NULL)
    {
        return atoi(e) == COOPAI_SMARTER ? COOPAI_SMARTER : COOPAI_CLASSIC;
    }
    return portGuardAiSmarter() ? COOPAI_SMARTER : COOPAI_CLASSIC;
}

/* the host's lobby row edits the same Port Option (saved to midas.ini) */
void coopAiSetSmarterOption(s32 mode)
{
    if (!COOP_SMARTER_AI)
    {
        mode = COOPAI_CLASSIC;
    }
    portOptSetValue(portOptFind("guard_ai"), mode == COOPAI_SMARTER);
    coopSetSetting(COOPSET_GUARDAI, mode);
}

/* read once a mission, so a partner leaving can't switch it half way */
s32 coopAiSmarter(void)
{
    s32 stage = bossGetStageNum();

    if (stage != sLatchStage || g_GlobalTimer < sLatchTimer)
    {
        sLatchStage = stage;
        sLatchOn = coopAiDecideOn();
        memset(sBrain, 0, sizeof(sBrain));
        memset(sPlayers, 0, sizeof(sPlayers));
        memset(sBarks, 0, sizeof(sBarks));
        sPlayersTick = -1;
        sAlarmRunUntil = 0;
        sAlert = sAlertFloor = AIA_CALM;
        sAlertRaised60 = sLastFight60 = sBodiesFound = 0;
        sNextSweep60 = sNextNade60 = 0;
        memset(sClues, 0, sizeof(sClues));
        memset(sFallen, 0, sizeof(sFallen));
        sNextClue = sNextFallen = 0;
        sNumWrecks = -1;    /* the first scan only notes what was smashed from the start */
        sNextWreckScan60 = 0;
        sBudgetFull = (getenv("GE_GUARDAI_BUDGET") != NULL) ? atoi(getenv("GE_GUARDAI_BUDGET")) : portGuardBudget();
        sBudget = (sBudgetFull > 100) ? -1 : sBudgetFull;
        if (sLatchOn)
        {
            portConLogf("ge: ai: smarter guards on (stage %d, alertness %d%%, alert budget %s%d)\n", stage,
                        coopAiAlertnessPct(), sBudget < 0 ? "no limit " : "", sBudget < 0 ? 0 : sBudget);
        }
    }
    sLatchTimer = g_GlobalTimer;
    return sLatchOn;
}

s32 coopGuardAiCount(void)
{
    return 2;
}

const char *coopGuardAiName(s32 mode)
{
    return (mode == COOPAI_SMARTER) ? "Smarter" : "Classic";
}

/* ---- small helpers ------------------------------------------------------- */

static f32 coopAiDist2(coord3d *a, coord3d *b)
{
    f32 dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;

    return dx * dx + dy * dy + dz * dz;
}

static s32 coopAiRandom(s32 n)
{
    return (s32) ((u32) randomGetNext() % (u32) n);
}

/* the 007 screen's Guard Alertness (the guard_alertness Port Option, the
 * host's): ranges for hearing and noticing, and how long they stay on edge */
static s32 coopAiAlertnessPct(void)
{
    const char *e = getenv("GE_GUARDAI_ALERTNESS");    /* the harness's stand-in for midas.ini */

    return (e != NULL) ? atoi(e) : portGuardAlertness();
}

static f32 coopAiAlertness(void)
{
    return (f32) coopAiAlertnessPct() / 100.0f;
}

/* ---- the alert budget ------------------------------------------------------
 * The 007 screen's Alert Budget (the guard_budget Port Option, the host's):
 * how many times a mission the guards may escalate. A guard answering a call
 * for help, an alarm run, a grenade, a lockdown sweep, a cut-off and a regroup
 * each spend one; with none left the guards still fight, search and take
 * cover, but bring no one else in and try nothing new. */

/* how many of n it may spend now */
static s32 coopAiAfford(s32 n)
{
    return (sBudget < 0 || n <= sBudget) ? n : sBudget;
}

static void coopAiSpend(s32 n, const char *what)
{
    if (sBudget < 0 || n <= 0)
    {
        return;
    }
    sBudget -= (n < sBudget) ? n : sBudget;
    if (sBudget == 0)
    {
        portConLogf("ge: ai: alert budget spent (%d, the last on %s)\n", sBudgetFull, what);
    }
}

static s32 coopAiArmed(ChrRecord *chr)
{
    return chrGetEquippedWeaponPropWithCheck(chr, GUNRIGHT) != NULL
        || chrGetEquippedWeaponPropWithCheck(chr, GUNLEFT) != NULL;
}

static s32 coopAiTerminal(s32 id)
{
    switch (id)
    {
        case GAILIST_STANDARD_GUARD:
        case GAILIST_SIMPLE_GUARD:
        case GAILIST_TRY_CLONE_SEND_OR_RUN_TO_BOND:
        case GAILIST_STANDARD_CLONE:
        case GAILIST_PERSISTENTLY_CHASE_AND_ATTACK_BOND:
            return 1;
        default:
            return 0;
    }
}

/* Classic's reactions: the lists a calm guard jumps to when something happens */
static s32 coopAiReaction(s32 id)
{
    switch (id)
    {
        case GAILIST_ATTACK_BOND:
        case GAILIST_RUN_TO_BOND:
        case GAILIST_STARTLE_AND_RUN_TO_BOND:
        case GAILIST_TRY_CLONE_SEND_OR_RUN_TO_BOND:
        case GAILIST_STANDARD_CLONE:
        case GAILIST_PERSISTENTLY_CHASE_AND_ATTACK_BOND:
            return 1;
        default:
            return 0;
    }
}

static s32 coopAiListId(ChrRecord *chr)
{
    bool global = FALSE;
    s32 id;

    if (chr->ailist == NULL)
    {
        return -1;
    }
    id = chraiGetAIListID(chr->ailist, &global);
    return global ? id : -1;
}

/* one of ours: a generic armed guard on a shared list */
static s32 coopAiOwns(ChrRecord *chr, s32 id)
{
    if (chr->prop == NULL || chr->model == NULL || chr->chrnum >= 30000)
    {
        return 0;   /* free, or a partner's puppet */
    }
    if (chr->chrflags & (CHRFLAG_HIDDEN | CHRFLAG_INVINCIBLE))
    {
        return 0;
    }
    if (coopAiTerminal(id))
    {
        return coopAiArmed(chr);
    }
    if (id == GAILIST_ATTACK_BOND || id == GAILIST_RUN_TO_BOND || id == GAILIST_STARTLE_AND_RUN_TO_BOND
        || id == GAILIST_PLAY_IDLE_ANIMATION)
    {   /* called as a subroutine: ours only when it returns to one of ours */
        return coopAiTerminal(chr->aireturnlist) && coopAiArmed(chr);
    }
    return 0;
}

static CoopAiBrain *coopAiBrainOf(ChrRecord *chr)
{
    s32 slot;
    CoopAiBrain *b;

    if (g_ChrSlots == NULL)
    {
        return NULL;
    }
    slot = coopChrSlot(chr);
    if (slot < 0 || slot >= COOPAI_MAX_CHRS)
    {
        return NULL;
    }
    b = &sBrain[slot];
    if (b->chr != chr || b->chrnum != chr->chrnum)
    {
        memset(b, 0, sizeof(*b));
        b->chr = chr;
        b->chrnum = chr->chrnum;
        b->coverpad = -1;
        b->origin = GAILIST_STANDARD_GUARD;
        b->seen60 = chr->lastseetarget60;
        b->arghs = chr->numarghs;
        b->nextbody60 = g_GlobalTimer + (slot * 7) % COOPAI_SEC;   /* spread the guards' looks over a second */
        {   /* 1 in 4 a rusher, 1 in 4 an anchor, the rest riflemen */
            u32 h = ((u32) chr->chrnum + 1u) * 2654435761u ^ (u32) bossGetStageNum() * 40503u;

            h = (h >> 13) & 3;
            b->role = (h == 0) ? AIR_RUSHER : (h == 1) ? AIR_ANCHOR : AIR_RIFLE;
        }
    }
    return b;
}

static void coopAiSetState(CoopAiBrain *b, s32 state, s32 secs)
{
    if (b->state != state && getenv("GE_GUARDAIDBG") != NULL && b->chr != NULL && b->chr->prop != NULL)
    {
        portConLogf("ge: ai: guard %d %s -> %s at (%.0f,%.0f) t=%d\n", b->chrnum, sStateNames[b->state], sStateNames[state],
                    b->chr->prop->pos.x, b->chr->prop->pos.z, g_GlobalTimer);
    }
    b->state = (u8) state;
    b->since60 = g_GlobalTimer;
    b->until60 = g_GlobalTimer + secs * COOPAI_SEC;
}

/* the chr runs its post's list from the top: nothing for Classic to resume */
static void coopAiParkList(ChrRecord *chr, s32 id)
{
    chr->ailist = ailistFindById(id);
    chr->aioffset = 0;
    chr->aireturnlist = (s16) id;
}

static s32 coopAiSeesTarget(ChrRecord *chr)
{
    s32 seen = chrCheckTargetInSight(chr);
    s32 k;

    for (k = 0; !seen && coopChrAltBegin(chr, k); k++)
    {   /* co-op: any player it can see, as Classic's "see Bond" */
        seen = chrCheckTargetInSight(chr);
        if (seen)
        {
            coopChrAltKeep(chr, 1);
        }
        else
        {
            coopChrAltEnd(chr);
        }
    }
    return seen;
}

static s32 coopAiGo(ChrRecord *chr, coord3d *pos, StandTile *stan, SPEED speed)
{
    if (stan == NULL || !chrIsNotDeadOrShot(chr))
    {
        return 0;
    }
    return plot_course_for_actor(chr, pos, stan, speed) != 0;
}

static s32 coopAiGoPad(ChrRecord *chr, s32 pad, SPEED speed)
{
    return pad >= 0 && chrGoToPad(chr, pad, speed);
}

static s32 coopAiStopped(ChrRecord *chr)
{
    return chrHasStoppedOrPatroling(chr);
}

/* stopped, and not just because the move it was given hasn't begun yet */
static s32 coopAiArrived(ChrRecord *chr, CoopAiBrain *b)
{
    return coopAiStopped(chr) && g_GlobalTimer > b->since60 + 10;
}

/* would a guard standing at pos (on stan) see this player? the sight ray of
 * chrCanSeeBond, from there */
static s32 coopAiLineFrom(ChrRecord *chr, coord3d *pos, StandTile *stan, PropRecord *target)
{
    StandTile *st = stan;
    f32 h = chr->chrheight - 20.0f;
    s32 clear;

    if (target == NULL || target->stan == NULL || stan == NULL)
    {
        return 0;
    }
    chrSetMoving(chr, FALSE);
    clear = stanTestLineUnobstructed(&st, pos->x, pos->z, target->pos.x, target->pos.z,
                                     CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PATHBLOCKER | CDTYPE_AIOPAQUE, h, h, 0.0f, 1.0f);
    chrSetMoving(chr, TRUE);
    return clear && st == target->stan;
}

/* would it see its target from there, or (co-op) any other player? cover
 * from one player in full view of another is no cover */
static s32 coopAiLineToTarget(ChrRecord *chr, coord3d *pos, StandTile *stan)
{
    PropRecord *target = getCurrentPlayerProp();
    s32 t;

    if (target == NULL || stan == NULL)
    {
        return 0;
    }
    if (coopAiLineFrom(chr, pos, stan, target))
    {
        return 1;
    }
    for (t = 0; coopEnabled() && t < COOP_MAX_PLAYERS; t++)
    {
        PropRecord *other = coopTargetablePlayerProp(t);

        if (other != NULL && other != target && coopAiLineFrom(chr, pos, stan, other))
        {
            return 1;
        }
    }
    return 0;
}

static s32 coopAiPadTaken(ChrRecord *chr, s32 pad)
{
    s32 i;

    for (i = 0; i < COOPAI_MAX_CHRS && i < g_NumChrSlots; i++)
    {
        if (sBrain[i].chr != NULL && sBrain[i].chr != chr && sBrain[i].coverpad == pad
            && sBrain[i].chr->model != NULL && !chrIsDead(sBrain[i].chr))
        {
            return 1;
        }
    }
    return 0;
}

/* how much of its health is gone, 0 to 1 */
static f32 coopAiHurt(ChrRecord *chr)
{
    return (chr->maxdamage > 0.0f) ? chr->damage / chr->maxdamage : 0.0f;
}

/* badly hurt, it can't run: it walks (GoldenEye has no limp; a slow walk
 * under fire reads as one) */
static SPEED coopAiPace(ChrRecord *chr, CoopAiBrain *b, SPEED want)
{
    if (want != SPEED_RUN || coopAiHurt(chr) < COOPAI_LIMP)
    {
        return want;
    }
    if (!b->limping)
    {
        b->limping = 1;
        portConLogf("ge: ai: guard %d is hurt and walks (%.0f%% gone)\n", chr->chrnum, coopAiHurt(chr) * 100.0f);
    }
    return SPEED_WALK;
}

/* the nearest waypoint pad from 1.5 to 9 m away that the target can't see
 * from where it is; away: not nearer the target than we are now */
static s32 coopAiFindCover(ChrRecord *chr, s32 away)
{
    PropRecord *target = getCurrentPlayerProp();
    waypoint *wp;
    s32 best = -1, tests = 0;
    f32 bestd2 = 1.0e30f, mytd2;

    if (g_CurrentSetup.pathwaypoints == NULL || target == NULL || chr->prop == NULL)
    {
        return -1;
    }
    if (sSearchTick != g_GlobalTimer)
    {
        sSearchTick = g_GlobalTimer;
        sSearches = 0;
    }
    if (sSearches >= COOPAI_SEARCHES)
    {
        return -2;  /* over this tick's budget: ask again next tick */
    }
    sSearches++;
    mytd2 = coopAiDist2(&chr->prop->pos, &target->pos);
    for (wp = g_CurrentSetup.pathwaypoints; wp->padID >= 0 && tests < COOPAI_COVER_TESTS; wp++)
    {
        PadRecord *pad = &g_CurrentSetup.pads[wp->padID];
        f32 d2, td2;

        if (pad->stan == NULL)
        {
            continue;
        }
        d2 = coopAiDist2(&pad->pos, &chr->prop->pos);
        if (d2 < COOPAI_COVER_NEAR * COOPAI_COVER_NEAR || d2 > COOPAI_COVER_FAR * COOPAI_COVER_FAR || d2 >= bestd2)
        {
            continue;
        }
        td2 = coopAiDist2(&pad->pos, &target->pos);
        if (td2 < 300.0f * 300.0f || (away && td2 < mytd2))
        {
            continue;
        }
        if (coopAiPadTaken(chr, wp->padID))
        {
            continue;
        }
        tests++;
        if (!coopAiLineToTarget(chr, &pad->pos, pad->stan))
        {
            best = wp->padID;
            bestd2 = d2;
        }
    }
    return best;
}

/* ---- barks ------------------------------------------------------------------
 * The host decides them (only it runs guards) and sends each to the joiners
 * as COOPEV_BARK; every game, the host's too, shows the callout over the
 * guard's head and plays the sting at it, unless its own Guard Barks is off.
 * The text is the port's; the stings are the game's own sounds. */

const char *coopAiBarkText(s32 kind)
{
    return (kind >= 0 && kind < COOPBARK_COUNT) ? sBarkText[kind] : "\n";
}

static void coopAiBarkShow(s32 chrnum, s32 kind)
{
    ChrRecord *chr = chrFindByLiteralId(chrnum);
    PropRecord *bond = getCurrentPlayerProp();
    s32 i, slot = 0;

    if (!COOP_SMARTER_AI || kind < 0 || kind >= COOPBARK_COUNT || !portGuardBarks() || chr == NULL || chr->prop == NULL)
    {
        return;
    }
    for (i = 0; i < COOPAI_BARKS; i++)
    {   /* the guard's own slot, else a free one, else the oldest */
        if (sBarks[i].chrnum == chrnum && sBarks[i].until > g_GlobalTimer)
        {
            slot = i;
            break;
        }
        if (sBarks[i].until < sBarks[slot].until)
        {
            slot = i;
        }
    }
    sBarks[slot].chrnum = (s16) chrnum;
    sBarks[slot].kind = (u8) kind;
    sBarks[slot].until = g_GlobalTimer + COOPAI_BARK_TICKS;
    if (getenv("GE_GUARDAI_BARKSHOT") != NULL && sBarkShotAt == 0 && sBarkShots < 12)
    {   /* harness: a screenshot a quarter second into each of the first 12 */
        sBarkShotAt = g_GlobalTimer + 15;
    }
    if (bond == NULL || coopAiDist2(&bond->pos, &chr->prop->pos) < COOPAI_BARK_RANGE * COOPAI_BARK_RANGE)
    {
        coopAudioAtPos(sBarkSting[kind], &chr->prop->pos);
    }
}

/* host (or solo): a guard calls out, at most once in COOPAI_BARK_GAP s */
static void coopAiBark(ChrRecord *chr, CoopAiBrain *b, s32 kind)
{
    u8 raw[4];

    if (g_GlobalTimer < b->bark60)
    {
        return;
    }
    b->bark60 = g_GlobalTimer + COOPAI_BARK_GAP * COOPAI_SEC;
    portConLogf("ge: ai: guard %d barks %.*s\n", chr->chrnum, (int) strlen(sBarkText[kind]) - 1, sBarkText[kind]);
    coopAiBarkShow(chr->chrnum, kind);
    if (coopEnabled() && coopNetIsHost())
    {
        raw[0] = (u8) (chr->chrnum & 0xFF);
        raw[1] = (u8) ((chr->chrnum >> 8) & 0xFF);
        raw[2] = (u8) kind;
        raw[3] = 0;
        coopExtSend(COOPEV_BARK, COOP_ALL, raw, sizeof(raw));
    }
}

void coopAiBarkApply(const u8 *raw, s32 from)
{
    s32 chrnum = (s16) (raw[0] | (raw[1] << 8));

    (void) from;
    if (coopNetIsHost())
    {
        return;     /* the host made it */
    }
    if (getenv("GE_GUARDAIDBG") != NULL)
    {
        portConLogf("ge: ai: host's guard %d barks %d\n", chrnum, raw[2]);
    }
    coopAiBarkShow(chrnum, raw[2]);
}

s32 coopAiBarkAt(s32 i, CoopBarkMark *out)
{
    s32 k, n = 0;

    if (i == 0 && sBarkShotAt != 0 && g_GlobalTimer >= sBarkShotAt)
    {
        sBarkShotAt = 0;
        snprintf(ge_shot_path, sizeof(ge_shot_path), "bark_p%d_%02d.bmp", coopEnabled() ? coopExtSelf() + 1 : 1, sBarkShots++);
        ge_shot_force = 1;
    }

    for (k = 0; k < COOPAI_BARKS; k++)
    {
        s32 left = sBarks[k].until - g_GlobalTimer;

        if (left <= 0 || left > COOPAI_BARK_TICKS || n++ != i)
        {
            continue;
        }
        out->chrnum = sBarks[k].chrnum;
        out->kind = sBarks[k].kind;
        out->alpha = (left < COOPAI_BARK_FADE) ? (f32) left / COOPAI_BARK_FADE : 1.0f;
        return 1;
    }
    return 0;
}

/* ---- the mission's alert level, and the players ---------------------------- */

static void coopAiRaise(s32 level, const char *why)
{
    if (level > sAlert)
    {
        portConLogf("ge: ai: alert %s -> %s (%s)\n", sAlertNames[sAlert], sAlertNames[level], why);
        sAlert = level;
    }
    if (level >= sAlert)
    {
        sAlertRaised60 = g_GlobalTimer;
    }
    if (level == AIA_LOCKDOWN)
    {
        sAlertFloor = AIA_LOCKDOWN;
    }
    else if (level >= AIA_ALERTED && sAlertFloor < AIA_SUSPICIOUS)
    {   /* once they have seen someone they stay on edge */
        sAlertFloor = AIA_SUSPICIOUS;
    }
}

static s32 coopAiSearchCount(void)
{
    return 2 + (sAlert >= AIA_SUSPICIOUS) + (sAlert >= AIA_LOCKDOWN);
}

static SPEED coopAiLookSpeed(void)
{
    return (sAlert >= AIA_ALERTED) ? SPEED_RUN : SPEED_WALK;
}

/* how long out of sight before it gives a target up */
static s32 coopAiLoseSecs(void)
{
    return (sAlert >= AIA_LOCKDOWN) ? 60 : (sAlert >= AIA_ALERTED) ? 45 : 30;
}

static CoopAiPlayer *coopAiTargetInfo(CoopAiBrain *b)
{
    if (b->target < 0 || b->target >= COOP_MAX_PLAYERS || !sPlayers[(s32) b->target].valid)
    {
        return NULL;
    }
    return &sPlayers[(s32) b->target];
}

/* lockdown: now and then a calm guard of ours checks the waypoints round its post */
static void coopAiSweepTick(void)
{
    s32 i, k, n, start, out = 0;

    if (sAlert < AIA_LOCKDOWN || g_GlobalTimer < sNextSweep60 || g_ChrSlots == NULL || coopAiAfford(1) < 1)
    {
        return;
    }
    sNextSweep60 = g_GlobalTimer + COOPAI_SWEEP_GAP * COOPAI_SEC;
    n = (g_NumChrSlots < COOPAI_MAX_CHRS) ? g_NumChrSlots : COOPAI_MAX_CHRS;
    for (i = 0; i < n; i++)
    {
        out += sBrain[i].chr != NULL && sBrain[i].sweeping && sBrain[i].state != AIS_POST;
    }
    if (out >= COOPAI_SWEEPERS || n <= 0)
    {
        return;
    }
    start = coopAiRandom(n);
    for (k = 0; k < n; k++)
    {
        ChrRecord *chr = &g_ChrSlots[(start + k) % n];
        CoopAiBrain *b;
        s32 id;

        if (chr->model == NULL || chr->prop == NULL || chr->prop->stan == NULL || !chrIsNotDeadOrShot(chr))
        {
            continue;
        }
        id = coopAiListId(chr);
        if ((id != GAILIST_STANDARD_GUARD && id != GAILIST_SIMPLE_GUARD) || !coopAiOwns(chr, id))
        {
            continue;
        }
        b = coopAiBrainOf(chr);
        if (b == NULL || b->state != AIS_POST || !coopAiStopped(chr))
        {
            continue;
        }
        b->origin = (u8) id;
        coopAiParkList(chr, b->origin);
        b->home = b->goal = chr->prop->pos;
        b->homestan = b->goalstan = chr->prop->stan;
        b->searchleft = (u8) coopAiSearchCount();
        b->sweeping = 1;
        coopAiSetState(b, AIS_SEARCH, 8);
        portConLogf("ge: ai: guard %d sets out on a sweep\n", chr->chrnum);
        coopAiSpend(1, "a sweep");
        return;
    }
}

/* once a tick: what each player is doing, and the alert level */
static void coopAiPlayersTick(void)
{
    s32 t, quiet;

    if (sPlayersTick == g_GlobalTimer)
    {
        return;
    }
    sPlayersTick = g_GlobalTimer;
    for (t = 0; t < COOP_MAX_PLAYERS; t++)
    {
        CoopAiPlayer *p = &sPlayers[t];
        CoopAiPlayerView v;

        if (!coopAiPlayerView(t, &v) || v.prop == NULL)
        {
            p->valid = 0;
            p->reviving = 0;
            continue;
        }
        if (!p->valid)
        {
            p->valid = 1;
            p->shotseq = v.shotseq;
            p->cue = v.cue;
            p->camppos = v.prop->pos;
            p->camp60 = g_GlobalTimer;
        }
        if (v.shotseq != p->shotseq || v.flash)
        {
            p->fired60 = g_GlobalTimer;
        }
        if ((v.cue & 0x1F) != (p->cue & 0x1F) && ((v.cue >> 5) & 3) == COOPCUE_RELOAD)
        {
            p->reload60 = g_GlobalTimer;
            if (getenv("GE_GUARDAIDBG") != NULL)
            {
                portConLogf("ge: ai: player %d reloads\n", t + 1);
            }
        }
        p->shotseq = v.shotseq;
        p->cue = v.cue;
        if (coopAiDist2(&v.prop->pos, &p->camppos) > COOPAI_CAMP_DIST * COOPAI_CAMP_DIST)
        {
            p->camppos = v.prop->pos;
            p->camp60 = g_GlobalTimer;
        }
        p->reviving = v.reviving;
        p->yaw = v.yaw;
        p->pos = v.prop->pos;
        p->stan = v.prop->stan;
        if (g_GlobalTimer - p->step60 >= COOPAI_STEP_TICKS || g_GlobalTimer < p->step60)
        {
            f32 dx = v.prop->pos.x - p->steppos.x, dz = v.prop->pos.z - p->steppos.z;
            s32 ticks = g_GlobalTimer - p->step60;

            p->speed = (p->step60 > 0 && ticks > 0) ? sqrtf(dx * dx + dz * dz) / (f32) ticks : 0.0f;
            if (p->speed > 30.0f)
            {   /* a respawn, or the harness moving them: not a step */
                p->speed = 0.0f;
            }
            p->steppos = v.prop->pos;
            p->step60 = g_GlobalTimer;
            if (getenv("GE_GUARDAI_STEPDBG") != NULL && g_GlobalTimer % 300 < COOPAI_STEP_TICKS)
            {
                portConLogf("ge: ai: player %d at (%.0f,%.0f,%.0f)\n", t + 1, v.prop->pos.x, v.prop->pos.y, v.prop->pos.z);
            }
            if (getenv("GE_GUARDAI_STEPDBG") != NULL && p->speed > 0.5f)
            {
                portConLogf("ge: ai: player %d moves %.2f a tick\n", t + 1, p->speed);
            }
        }
    }
    coopAiWreckScan();

    if (alarmIsActive())
    {
        coopAiRaise(AIA_LOCKDOWN, "the alarm");
    }
    quiet = g_GlobalTimer - ((sAlertRaised60 > sLastFight60) ? sAlertRaised60 : sLastFight60);
    if (sAlert == AIA_ALERTED && sAlertFloor < AIA_ALERTED && quiet > (s32) (90 * COOPAI_SEC * coopAiAlertness()))
    {
        portConLogf("ge: ai: alert alerted -> suspicious (quiet %d s)\n", quiet / COOPAI_SEC);
        sAlert = AIA_SUSPICIOUS;
        sAlertRaised60 = g_GlobalTimer;
    }
    else if (sAlert == AIA_SUSPICIOUS && sAlertFloor < AIA_SUSPICIOUS && quiet > (s32) (45 * COOPAI_SEC * coopAiAlertness()))
    {
        portConLogf("ge: ai: alert suspicious -> calm (quiet %d s)\n", quiet / COOPAI_SEC);
        sAlert = AIA_CALM;
    }
    coopAiSweepTick();
}

/* ---- calls for help, bodies, alarms ---------------------------------------- */

static void coopAiInvestigate(ChrRecord *chr, CoopAiBrain *b, coord3d *pos, StandTile *stan, s32 answering)
{
    if (stan == NULL)
    {
        return;
    }
    b->goal = *pos;
    b->goalstan = stan;
    b->answering = (u8) answering;
    b->searchleft = (u8) coopAiSearchCount();
    if (b->state == AIS_POST)
    {
        b->home = chr->prop->pos;
        b->homestan = chr->prop->stan;
    }
    if (answering)
    {
        coopAiSetState(b, AIS_INVESTIGATE, 15);
        if (!coopAiGo(chr, &b->goal, b->goalstan, coopAiPace(chr, b, SPEED_RUN)))
        {
            coopAiSetState(b, AIS_SEARCH, 8);
        }
    }
    else
    {
        coopAiRaise(AIA_SUSPICIOUS, "a guard looks into something");
        coopAiSetState(b, AIS_LOOK, 3);
        chrTrySurprisedLookAround(chr);
    }
}

/* the guard who saw the player first calls; calm guards of ours close
 * enough to hear (20 m in the open, 8 m round a corner) come running */
static void coopAiCallForHelp(ChrRecord *caller, CoopAiBrain *cb)
{
    s32 i, answered = 0;

    s32 most = coopAiAfford(COOPAI_CALL_MAX + (sAlert >= AIA_ALERTED));

    for (i = 0; i < g_NumChrSlots && i < COOPAI_MAX_CHRS && answered < most; i++)
    {
        ChrRecord *chr = &g_ChrSlots[i];
        CoopAiBrain *b;
        f32 d2;

        if (chr == caller || chr->model == NULL || chr->prop == NULL || !chrIsNotDeadOrShot(chr)
            || !coopAiOwns(chr, coopAiListId(chr)))
        {
            continue;
        }
        d2 = coopAiDist2(&chr->prop->pos, &caller->prop->pos);
        if (d2 > COOPAI_CALL_RANGE * COOPAI_CALL_RANGE)
        {
            continue;
        }
        if (d2 > COOPAI_CALL_NEAR * COOPAI_CALL_NEAR
            && !check_if_position_in_same_room(chr, &caller->prop->pos, caller->prop->stan))
        {
            continue;
        }
        b = coopAiBrainOf(chr);
        if (b == NULL || b->state != AIS_POST)
        {
            continue;
        }
        b->origin = (coopAiListId(chr) == GAILIST_SIMPLE_GUARD) ? GAILIST_SIMPLE_GUARD : GAILIST_STANDARD_GUARD;
        coopAiParkList(chr, b->origin);
        coopAiInvestigate(chr, b, &cb->lastpos, cb->laststan, 1);
        answered++;
    }
    if (answered > 0)
    {
        portConLogf("ge: ai: guard %d calls for help, %d come\n", caller->chrnum, answered);
        coopAiSpend(answered, "a call for help");
    }
    coopAiBark(caller, cb, answered > 0 ? COOPBARK_HELP : COOPBARK_SPOTTED);
}

static s32 coopAiKnowsBody(CoopAiBrain *b, ChrRecord *dead)
{
    s32 k;

    for (k = 0; k < COOPAI_BODIES; k++)
    {
        if (b->bodies[k] == dead->chrnum + 1)
        {
            return 1;
        }
    }
    return 0;
}

static void coopAiNoteBody(CoopAiBrain *b, ChrRecord *dead)
{
    if (!coopAiKnowsBody(b, dead))
    {
        b->bodies[b->nextbodyslot] = (s16) (dead->chrnum + 1);
        b->nextbodyslot = (u8) ((b->nextbodyslot + 1) % COOPAI_BODIES);
    }
}

/* a body within 15 m that the guard can see from where it stands; NULL none */
static ChrRecord *coopAiBodyInView(ChrRecord *chr, CoopAiBrain *b)
{
    s32 i;

    for (i = 0; i < g_NumChrSlots; i++)
    {
        ChrRecord *dead = &g_ChrSlots[i];

        if (dead == chr || dead->model == NULL || dead->prop == NULL || dead->actiontype != ACT_DEAD
            || dead->chrnum >= 30000 || dead->prop->stan == NULL || coopAiKnowsBody(b, dead))
        {
            continue;
        }
        if (coopAiDist2(&dead->prop->pos, &chr->prop->pos) > COOPAI_BODY_RANGE * COOPAI_BODY_RANGE * coopAiAlertness() * coopAiAlertness())
        {
            continue;
        }
        if (check_if_position_in_same_room(chr, &dead->prop->pos, dead->prop->stan))
        {
            return dead;
        }
    }
    return NULL;
}

/* a calm guard of ours looking round for a body it hasn't seen yet: once a
 * second (a timer, as g_GlobalTimer can step over any one tick) */
static ChrRecord *coopAiFindBody(ChrRecord *chr, CoopAiBrain *b)
{
    ChrRecord *dead;

    if (g_GlobalTimer < b->nextbody60)
    {
        return NULL;
    }
    b->nextbody60 = g_GlobalTimer + COOPAI_SEC;
    dead = coopAiBodyInView(chr, b);
    if (dead != NULL)
    {
        coopAiNoteBody(b, dead);
    }
    return dead;
}

static s32 coopAiFindAlarm(ChrRecord *chr)
{
    PropRecord *prop;
    s32 best = -1;
    f32 bestd2 = COOPAI_ALARM_RANGE * COOPAI_ALARM_RANGE;

    for (prop = chrpropGetActiveTail(); prop != NULL; prop = prop->prev)
    {
        ObjectRecord *obj;
        f32 d2;

        if (prop->type != PROP_TYPE_OBJ || prop->obj == NULL)
        {
            continue;
        }
        obj = prop->obj;
        if (obj->type != PROPDEF_ALARM || obj->pad < 0 || !objIsHealthy(obj))
        {
            continue;
        }
        d2 = coopAiDist2(&prop->pos, &chr->prop->pos);
        if (d2 < bestd2)
        {
            bestd2 = d2;
            best = obj->pad;
        }
    }
    return best;
}

/* ---- clues: doors left open, things smashed, footsteps --------------------
 * A calm guard of ours looks round once a second (with its look for bodies):
 * a door a player opened that is still open, or something smashed (a window,
 * a camera, a monitor), within 15 m and in sight, is worth a look; so are
 * running footsteps within 5 m (2.5 m through a wall). Walking, or crouched,
 * a player makes no sound. GoldenEye has no lights to shoot out; a smashed
 * object stands in for them. */

static void coopAiAddClue(s32 kind, coord3d *pos, StandTile *stan, void *what)
{
    s32 slot = sNextClue, i;

    if (stan == NULL)
    {
        return;
    }
    sNextClue = (sNextClue + 1) % COOPAI_CLUES;
    sClues[slot].kind = (u8) kind;
    sClues[slot].at60 = g_GlobalTimer;
    sClues[slot].pos = *pos;
    sClues[slot].stan = stan;
    sClues[slot].what = what;
    for (i = 0; i < COOPAI_MAX_CHRS; i++)
    {   /* news to everyone */
        sBrain[i].clueseen &= ~(1u << slot);
    }
}

/* propobj.c doorSetOpenState: a shut door starts to open. A player beside it,
 * and no guard of ours (guards open doors on their way through): the player
 * opened it, and where they stood is worth a look while it stays open */
void coopAiDoorOpened(DoorRecord *door, s32 state)
{
    s32 t, i;

    if (state != DOORSTATE_OPENING || door == NULL || door->prop == NULL || door->openPosition > 0.0f
        || !coopAiSmarter() || (coopSessionActive() && !coopNetIsHost()))
    {
        return;
    }
    for (i = 0; i < g_NumChrSlots; i++)
    {
        ChrRecord *c = &g_ChrSlots[i];

        if (c->model != NULL && c->prop != NULL && c->chrnum < 30000 && !chrIsDead(c)
            && coopAiDist2(&c->prop->pos, &door->prop->pos) < 250.0f * 250.0f)
        {
            if (getenv("GE_GUARDAIDBG") != NULL)
            {
                portConLogf("ge: ai: door at (%.0f,%.0f) opened by chr %d's side, not a clue\n", door->prop->pos.x,
                            door->prop->pos.z, c->chrnum);
            }
            return;
        }
    }
    for (t = 0; t < COOP_MAX_PLAYERS; t++)
    {
        CoopAiPlayerView v;
        f32 dx, dy, dz;

        if (!coopAiPlayerView(t, &v) || v.prop == NULL || v.prop->stan == NULL)
        {
            continue;
        }
        dx = v.prop->pos.x - door->prop->pos.x;
        dy = v.prop->pos.y - door->prop->pos.y;
        dz = v.prop->pos.z - door->prop->pos.z;
        if (dx * dx + dz * dz < 300.0f * 300.0f && dy > -250.0f && dy < 250.0f)
        {
            coopAiAddClue(AIC_DOOR, &v.prop->pos, v.prop->stan, door);
            portConLogf("ge: ai: player %d opened a door at (%.0f,%.0f)\n", t + 1, door->prop->pos.x, door->prop->pos.z);
            return;
        }
    }
    if (getenv("GE_GUARDAIDBG") != NULL)
    {
        CoopAiPlayerView v;

        for (t = 0; t < COOP_MAX_PLAYERS; t++)
        {
            if (coopAiPlayerView(t, &v) && v.prop != NULL)
            {
                portConLogf("ge: ai: door at (%.0f,%.0f,%.0f) opened, player %d at (%.0f,%.0f,%.0f)\n", door->prop->pos.x,
                            door->prop->pos.y, door->prop->pos.z, t + 1, v.prop->pos.x, v.prop->pos.y, v.prop->pos.z);
            }
        }
    }
}

static StandTile *coopAiPadStan(s32 pad)
{
    if (pad < 0 || g_CurrentSetup.pads == NULL)
    {
        return NULL;
    }
    if (isNotBoundPad(pad))
    {
        return g_CurrentSetup.pads[pad].stan;
    }
    return (g_CurrentSetup.boundpads != NULL) ? g_CurrentSetup.boundpads[getBoundPadNum(pad)].stan : NULL;
}

/* once a second: anything newly smashed is a clue (what was smashed from the
 * start of the mission isn't) */
static void coopAiWreckScan(void)
{
    PropRecord *prop;
    s32 prime = sNumWrecks < 0;

    if (g_GlobalTimer < sNextWreckScan60)
    {
        return;
    }
    sNextWreckScan60 = g_GlobalTimer + COOPAI_SEC;
    if (prime)
    {
        sNumWrecks = 0;
    }
    for (prop = chrpropGetActiveTail(); prop != NULL; prop = prop->prev)
    {
        ObjectRecord *obj;
        s32 k, known = 0;

        if (prop->type != PROP_TYPE_OBJ || prop->obj == NULL)
        {
            continue;
        }
        obj = prop->obj;
        if (obj->type == PROPDEF_DOOR || obj->type == PROPDEF_COLLECTABLE || obj->type == PROPDEF_MAGAZINE
            || obj->type == PROPDEF_AMMO
            || obj->type == PROPDEF_KEY || obj->type == PROPDEF_ARMOUR || objIsHealthy(obj))
        {
            continue;
        }
        for (k = 0; k < sNumWrecks && !known; k++)
        {
            known = sWrecks[k] == prop;
        }
        if (known)
        {
            continue;
        }
        if (sNumWrecks < COOPAI_WRECKS)
        {
            sWrecks[sNumWrecks++] = prop;
        }
        else
        {
            memmove(&sWrecks[0], &sWrecks[1], sizeof(sWrecks[0]) * (COOPAI_WRECKS - 1));
            sWrecks[COOPAI_WRECKS - 1] = prop;
        }
        if (!prime)
        {
            StandTile *stan = (prop->stan != NULL) ? prop->stan : coopAiPadStan(obj->pad);

            if (stan != NULL)
            {
                coopAiAddClue(AIC_WRECK, &prop->pos, stan, NULL);
                if (getenv("GE_GUARDAIDBG") != NULL)
                {
                    portConLogf("ge: ai: object type %d smashed at (%.0f,%.0f)\n", obj->type, prop->pos.x, prop->pos.z);
                }
            }
        }
    }
}

/* a calm guard's look round for a clue (1: it goes to see) */
static s32 coopAiNotice(ChrRecord *chr, CoopAiBrain *b)
{
    s32 i, t;
    f32 range;

    for (i = 0; i < COOPAI_CLUES; i++)
    {
        if (sClues[i].kind == AIC_NONE || (b->clueseen & (1u << i)))
        {
            continue;
        }
        if (g_GlobalTimer - sClues[i].at60 > COOPAI_CLUE_SECS * COOPAI_SEC
            || (sClues[i].kind == AIC_DOOR && doorIsClosed((DoorRecord *) sClues[i].what)))
        {   /* old news, or shut again */
            continue;
        }
        if (coopAiDist2(&sClues[i].pos, &chr->prop->pos) > COOPAI_CLUE_RANGE * COOPAI_CLUE_RANGE * coopAiAlertness() * coopAiAlertness()
            || !check_if_position_in_same_room(chr, &sClues[i].pos, sClues[i].stan))
        {
            continue;
        }
        b->clueseen |= 1u << i;
        portConLogf("ge: ai: guard %d notices %s\n", chr->chrnum,
                    sClues[i].kind == AIC_DOOR ? "a door left open" : "something smashed");
        coopAiInvestigate(chr, b, &sClues[i].pos, sClues[i].stan, 0);
        return 1;
    }
    range = COOPAI_STEP_RANGE * ((sAlert >= AIA_LOCKDOWN) ? 1.25f : 1.0f) * coopAiAlertness();
    for (t = 0; t < COOP_MAX_PLAYERS; t++)
    {
        CoopAiPlayer *p = &sPlayers[t];
        f32 d2;

        if (!p->valid || p->stan == NULL || p->speed < COOPAI_STEP_SPEED)
        {
            continue;
        }
        d2 = coopAiDist2(&p->pos, &chr->prop->pos);
        if (d2 > range * range)
        {
            continue;
        }
        if (d2 > range * range * 0.25f && !check_if_position_in_same_room(chr, &p->pos, p->stan))
        {   /* through a wall: only close by */
            continue;
        }
        portConLogf("ge: ai: guard %d hears player %d running (%.0f m off)\n", chr->chrnum, t + 1, sqrtf(d2) / 100.0f);
        coopAiInvestigate(chr, b, &p->pos, p->stan, 0);
        return 1;
    }
    return 0;
}

/* ---- the fight ----------------------------------------------------------- */

/* a Classic Attack Bond burst, from where it stands */
static s32 coopAiBurst(ChrRecord *chr, CoopAiBrain *b)
{
    chr->ailist = ailistFindById(GAILIST_ATTACK_BOND);
    chr->aioffset = 0;
    chr->aireturnlist = GAILIST_STANDARD_GUARD;    /* waits for the action to end, unlike Simple Guard */
    b->delegated = 1;
    b->bursts++;
    coopAiSetState(b, AIS_COMBAT, 6);
    return 0;   /* Classic runs it now */
}

/* co-op: more than its share of the fight on one player turns it to another
 * player it can see */
static void coopAiSplit(ChrRecord *chr, CoopAiBrain *b)
{
    s32 count[COOP_MAX_PLAYERS];
    s32 i, k, mine, fewest = 999;

    if (!coopEnabled() || g_GlobalTimer < b->nextsplit60)
    {
        return;
    }
    b->nextsplit60 = g_GlobalTimer + 4 * COOPAI_SEC;
    if (coopAiTargetInfo(b) != NULL && coopAiTargetInfo(b)->reviving)
    {
        return;     /* the one to stop */
    }
    memset(count, 0, sizeof(count));
    for (i = 0; i < g_NumChrSlots && i < COOPAI_MAX_CHRS; i++)
    {
        if (sBrain[i].chr != NULL && sBrain[i].chr->model != NULL && sBrain[i].state >= AIS_COMBAT
            && sBrain[i].target >= 0 && sBrain[i].target < COOP_MAX_PLAYERS && !chrIsDead(sBrain[i].chr))
        {
            count[(s32) sBrain[i].target]++;
        }
    }
    mine = (b->target >= 0 && b->target < COOP_MAX_PLAYERS) ? count[(s32) b->target] : 0;
    for (k = 0; k < COOP_MAX_PLAYERS; k++)
    {
        if (k != b->target && coopTargetablePlayer(k) && count[k] < fewest)
        {
            fewest = count[k];
        }
    }
    if (fewest == 999 || mine <= fewest + 1)
    {
        return;
    }
    for (k = 0; coopChrAltBegin(chr, k); k++)
    {
        s32 t = coopChrCurrentTarget();

        if (count[t] < mine - 1 && chrCheckTargetInSight(chr))
        {
            coopChrAltKeep(chr, 1);
            b->target = (s8) t;
            portConLogf("ge: ai: guard %d turns to player %d (%d on its old target, %d on this one)\n",
                        chr->chrnum, t + 1, mine, count[t]);
            return;
        }
        coopChrAltEnd(chr);
    }
}

/* co-op: a player it can see holding Use over a downed partner is the one to
 * shoot (a careless revive costs) */
static s32 coopAiPunishRevive(ChrRecord *chr, CoopAiBrain *b)
{
    s32 k, any = 0;

    if (!coopEnabled() || g_GlobalTimer < b->nextrevive60)
    {
        return 0;
    }
    b->nextrevive60 = g_GlobalTimer + COOPAI_SEC / 2;
    for (k = 0; k < COOP_MAX_PLAYERS; k++)
    {
        any |= sPlayers[k].valid && sPlayers[k].reviving;
    }
    if (!any || (coopAiTargetInfo(b) != NULL && coopAiTargetInfo(b)->reviving))
    {
        return 0;
    }
    for (k = 0; coopChrAltBegin(chr, k); k++)
    {
        s32 t = coopChrCurrentTarget();

        if (t >= 0 && t < COOP_MAX_PLAYERS && sPlayers[t].reviving && chrCheckTargetInSight(chr))
        {
            PropRecord *target;

            coopChrAltKeep(chr, 1);
            portConLogf("ge: ai: guard %d turns to player %d, who is reviving a partner\n", chr->chrnum, t + 1);
            b->target = (s8) t;
            b->lastsight60 = g_GlobalTimer;
            target = getCurrentPlayerProp();
            if (target != NULL)
            {
                b->lastpos = target->pos;
                b->laststan = target->stan;
            }
            return 1;
        }
        coopChrAltEnd(chr);
    }
    return 0;
}

/* its target fired just now, within 20 m, facing its way (cover or not: a
 * burst into the crate it hides behind keeps it down too) */
static s32 coopAiShotAt(ChrRecord *chr, CoopAiBrain *b)
{
    CoopAiPlayer *p = coopAiTargetInfo(b);
    PropRecord *target = getCurrentPlayerProp();
    f32 diff;

    if (p == NULL || target == NULL || p->fired60 <= 0 || g_GlobalTimer - p->fired60 >= COOPAI_SUPPRESS
        || coopAiDist2(&chr->prop->pos, &target->pos) > COOPAI_SUPPRESS_RANGE * COOPAI_SUPPRESS_RANGE
        || !(p->yaw > -20.0f && p->yaw < 20.0f))
    {
        return 0;
    }
    diff = atan2f(chr->prop->pos.x - target->pos.x, chr->prop->pos.z - target->pos.z) - p->yaw;
    while (diff < -M_PI_F)
    {
        diff += M_TAU_F;
    }
    while (diff > M_PI_F)
    {
        diff -= M_TAU_F;
    }
    return diff > -COOPAI_SUPPRESS_ARC && diff < COOPAI_SUPPRESS_ARC;
}

/* its target began a reload just now, and it hasn't pushed on that one yet */
static s32 coopAiTargetReloading(CoopAiBrain *b)
{
    CoopAiPlayer *p = coopAiTargetInfo(b);

    if (p == NULL || p->reload60 <= 0 || g_GlobalTimer - p->reload60 >= COOPAI_RELOAD_PUSH || p->reload60 == b->pushed60)
    {
        return 0;
    }
    b->pushed60 = p->reload60;
    portConLogf("ge: ai: guard %d pushes while player %d reloads\n", b->chrnum, b->target + 1);
    return 1;
}

/* anyone not one of our guards (a hostage, a scientist, Natalya, a scripted
 * guard) within a grenade's reach of pos: no grenade, so no objective is lost */
static s32 coopAiBystander(coord3d *pos)
{
    s32 i;

    for (i = 0; i < g_NumChrSlots; i++)
    {
        ChrRecord *c = &g_ChrSlots[i];

        if (c->model == NULL || c->prop == NULL || c->chrnum >= 30000 || chrIsDead(c)
            || coopAiOwns(c, coopAiListId(c)))
        {
            continue;
        }
        if (coopAiDist2(&c->prop->pos, pos) < COOPAI_NADE_CLEAR * COOPAI_NADE_CLEAR)
        {
            if (getenv("GE_GUARDAIDBG") != NULL)
            {
                portConLogf("ge: ai: no grenade: chr %d is near the target\n", c->chrnum);
            }
            return 1;
        }
    }
    return 0;
}

/* a target that has held one spot a while, seen there just now, 4 to 20 m off:
 * the guard points first (the warning), then throws */
static s32 coopAiCamperGrenade(ChrRecord *chr, CoopAiBrain *b)
{
    CoopAiPlayer *p = coopAiTargetInfo(b);
    PropRecord *target = getCurrentPlayerProp();
    f32 d2;

    if (p == NULL || target == NULL || b->threw || g_GlobalTimer < sNextNade60 || coopAiAfford(1) < 1
        || g_GlobalTimer - b->fight60 < COOPAI_NADE_FIGHT * COOPAI_SEC
        || (p->nade60 > 0 && g_GlobalTimer - p->nade60 < COOPAI_NADE_AGAIN * COOPAI_SEC)
        || g_GlobalTimer - p->camp60 < COOPAI_CAMP_SECS * COOPAI_SEC
        || g_GlobalTimer - b->lastsight60 > 3 * COOPAI_SEC
        || coopAiDist2(&target->pos, &p->camppos) > COOPAI_CAMP_DIST * COOPAI_CAMP_DIST)
    {
        return 0;
    }
    d2 = coopAiDist2(&chr->prop->pos, &target->pos);
    if (d2 < COOPAI_NADE_NEAR * COOPAI_NADE_NEAR || d2 > COOPAI_NADE_FAR * COOPAI_NADE_FAR || coopAiBystander(&target->pos))
    {
        return 0;
    }
    b->threw = 1;
    coopAiSpend(1, "a grenade");
    p->nade60 = g_GlobalTimer;
    sNextNade60 = g_GlobalTimer + COOPAI_NADE_GAP * COOPAI_SEC;
    chrTrySurprisedOneHand(chr);
    b->bark60 = 0;      /* the warning always gets said */
    coopAiBark(chr, b, COOPBARK_GRENADE);
    portConLogf("ge: ai: guard %d points out player %d (held one spot %d s, %.0f m off)\n", chr->chrnum, b->target + 1,
                (g_GlobalTimer - p->camp60) / COOPAI_SEC, sqrtf(d2) / 100.0f);
    coopAiSetState(b, AIS_GRENADE, 2);
    return 1;
}

/* two or more of ours have died within 20 m of it in the last 20 s */
static s32 coopAiLosing(ChrRecord *chr)
{
    s32 k, n = 0;

    for (k = 0; k < COOPAI_FALLEN; k++)
    {
        if (sFallen[k].at60 > 0 && g_GlobalTimer - sFallen[k].at60 < COOPAI_LOSING_SECS * COOPAI_SEC
            && coopAiDist2(&sFallen[k].pos, &chr->prop->pos) < COOPAI_LOSING_RANGE * COOPAI_LOSING_RANGE)
        {
            n++;
        }
    }
    return n >= 2;
}

/* no other guard of ours fighting within 15 m */
static s32 coopAiAlone(ChrRecord *chr)
{
    s32 i;

    for (i = 0; i < g_NumChrSlots && i < COOPAI_MAX_CHRS; i++)
    {
        CoopAiBrain *o = &sBrain[i];

        if (o->chr != NULL && o->chr != chr && o->chr->model != NULL && o->chr->prop != NULL && !o->surrendered
            && o->state >= AIS_COMBAT && !chrIsDead(o->chr)
            && coopAiDist2(&o->chr->prop->pos, &chr->prop->pos) < COOPAI_ALONE * COOPAI_ALONE)
        {
            return 0;
        }
    }
    return 1;
}

/* where to fall back to when the fight goes badly: the calm guards of ours
 * (the ones still on post) standing closest together, within 40 m, farther
 * from the target than we are and 12 m from it at least: the room they hold */
static s32 coopAiRegroupSpot(ChrRecord *chr, PropRecord *target, coord3d *out, StandTile **outstan)
{
    s32 i, k, best = 0;
    f32 bestd2 = 0.0f, mytd2;
    s32 n = (g_NumChrSlots < COOPAI_MAX_CHRS) ? g_NumChrSlots : COOPAI_MAX_CHRS;

    mytd2 = coopAiDist2(&chr->prop->pos, &target->pos);
    for (i = 0; i < n; i++)
    {
        ChrRecord *c = &g_ChrSlots[i];
        f32 d2, td2;
        s32 score = 0;

        if (c == chr || c->model == NULL || c->prop == NULL || c->prop->stan == NULL || !chrIsNotDeadOrShot(c)
            || !coopAiOwns(c, coopAiListId(c)) || sBrain[i].chr != c || sBrain[i].state != AIS_POST)
        {
            continue;
        }
        d2 = coopAiDist2(&c->prop->pos, &chr->prop->pos);
        td2 = coopAiDist2(&c->prop->pos, &target->pos);
        if (d2 > COOPAI_REGROUP_FAR * COOPAI_REGROUP_FAR || td2 < 1200.0f * 1200.0f || td2 <= mytd2)
        {
            continue;
        }
        for (k = 0; k < n; k++)
        {
            ChrRecord *o = &g_ChrSlots[k];

            if (o->model != NULL && o->prop != NULL && sBrain[k].chr == o && sBrain[k].state == AIS_POST
                && !chrIsDead(o) && coopAiOwns(o, coopAiListId(o))
                && coopAiDist2(&o->prop->pos, &c->prop->pos) < COOPAI_REGROUP_NEAR * COOPAI_REGROUP_NEAR)
            {
                score++;
            }
        }
        if (score > best || (score == best && d2 < bestd2))
        {
            best = score;
            bestd2 = d2;
            *out = c->prop->pos;
            *outstan = c->prop->stan;
        }
    }
    return best > 0;
}

/* co-op: the partner nearest the guard's target, and how far apart they are */
static PropRecord *coopAiNearestMate(s32 target, PropRecord *tprop, f32 *d2out)
{
    PropRecord *best = NULL;
    s32 t;

    *d2out = 1.0e30f;
    for (t = 0; t < COOP_MAX_PLAYERS; t++)
    {
        PropRecord *m = (t != target) ? coopTargetablePlayerProp(t) : NULL;

        if (m != NULL && m != tprop && coopAiDist2(&m->pos, &tprop->pos) < *d2out)
        {
            *d2out = coopAiDist2(&m->pos, &tprop->pos);
            best = m;
        }
    }
    return best;
}

static s32 coopAiCountState(s32 state)
{
    s32 i, n = 0;

    for (i = 0; i < g_NumChrSlots && i < COOPAI_MAX_CHRS; i++)
    {
        n += sBrain[i].chr != NULL && sBrain[i].state == state && sBrain[i].chr->model != NULL && !chrIsDead(sBrain[i].chr);
    }
    return n;
}

static void coopAiEnterCombat(ChrRecord *chr, CoopAiBrain *b)
{
    PropRecord *target = getCurrentPlayerProp();

    if (b->state == AIS_POST)
    {
        b->home = chr->prop->pos;
        b->homestan = chr->prop->stan;
    }
    if (b->state < AIS_COMBAT)
    {
        b->fight60 = g_GlobalTimer;
        portConLogf("ge: ai: guard %d fights as %s\n", chr->chrnum, sRoleNames[b->role]);
    }
    b->regrouped = 0;
    b->clueseen = ~0u;      /* whatever it walked past in the fight is no news after it */
    b->called = 0;
    b->fellback = 0;
    b->threw = 0;
    b->sweeping = 0;
    b->bursts = 0;
    b->coverpad = -1;
    b->lastdamage = chr->damage;
    b->lastsight60 = g_GlobalTimer;
    if (target != NULL)
    {
        b->lastpos = target->pos;
        b->laststan = target->stan;
    }
    coopAiRaise(AIA_ALERTED, "a guard is fighting");
    coopAiSetState(b, AIS_COMBAT, 6);
}

static s32 coopAiCombat(ChrRecord *chr, CoopAiBrain *b, s32 seen)
{
    PropRecord *target = getCurrentPlayerProp();
    s32 stopped = coopAiStopped(chr);
    s32 pad;

    if (coopAiShotAt(chr, b))
    {
        b->supp60 = g_GlobalTimer;
    }
    if (b->delegated)
    {   /* a burst: Classic Attack Bond started an action and returned to the post's list */
        s32 id = coopAiListId(chr);

        if (id == GAILIST_ATTACK_BOND || id == GAILIST_RUN_TO_BOND)
        {
            return 0;
        }
        b->delegated = 0;
        chr->aioffset = 0;
    }
    if (chr->damage > b->lastdamage)
    {
        b->lastdamage = chr->damage;
        b->hit60 = g_GlobalTimer;
    }

    switch (b->state)
    {
        case AIS_REGROUP:
            if (seen && g_GlobalTimer > b->since60 + 2 * COOPAI_SEC)
            {   /* they followed: make the stand here */
                chraiStopAnimation(chr);
                b->regrouped = 2;
                b->home = chr->prop->pos;
                b->homestan = chr->prop->stan;
                return coopAiBurst(chr, b);
            }
            if (coopAiArrived(chr, b) || g_GlobalTimer > b->until60)
            {
                b->regrouped = 2;
                b->home = chr->prop->pos;
                b->homestan = chr->prop->stan;
                portConLogf("ge: ai: guard %d has regrouped and holds\n", chr->chrnum);
                coopAiSetState(b, AIS_DUCK, 2);
                b->heldown = 0;
                check_if_able_to_then_kneel(chr);
            }
            return 1;
        case AIS_COVER:
        case AIS_FALLBACK:
            if (coopAiArrived(chr, b) || g_GlobalTimer > b->until60)
            {
                coopAiSetState(b, AIS_DUCK, (b->role == AIR_ANCHOR) ? 2 + coopAiRandom(2)
                                            : (b->role == AIR_RUSHER) ? 1 : 1 + coopAiRandom(2));
                b->heldown = 0;
                check_if_able_to_then_kneel(chr);
            }
            return 1;
        case AIS_DUCK:
            if (seen && stopped)
            {   /* they came round to us: fire from here */
                b->coverpad = -1;
                return coopAiBurst(chr, b);
            }
            if (b->firestan != NULL && coopAiTargetReloading(b))
            {   /* their gun is empty: out now */
                b->until60 = g_GlobalTimer - 1;
            }
            else if (g_GlobalTimer - b->supp60 < COOPAI_SUPPRESS && g_GlobalTimer < b->since60 + 5 * COOPAI_SEC)
            {   /* still shot at: heads down a little longer (5 s at most) */
                if (b->until60 < g_GlobalTimer + COOPAI_SEC / 2)
                {
                    b->until60 = g_GlobalTimer + COOPAI_SEC / 2;
                }
                if (!b->heldown && b->supp60 > 0)
                {
                    b->heldown = 1;
                    portConLogf("ge: ai: guard %d keeps its head down under fire\n", chr->chrnum);
                }
            }
            if (g_GlobalTimer > b->until60)
            {
                b->coverpad = -1;
                b->bursts = 0;
                if (b->firestan != NULL && coopAiGo(chr, &b->firepos, b->firestan, coopAiPace(chr, b, SPEED_RUN)))
                {
                    coopAiSetState(b, AIS_PEEK, 4);
                }
                else
                {
                    coopAiSetState(b, AIS_COMBAT, 6);
                }
            }
            return 1;
        case AIS_PEEK:
        case AIS_ADVANCE:
        case AIS_FLANK:
        case AIS_CUTOFF:
            if (seen)
            {   /* in sight: stop and fire */
                chraiStopAnimation(chr);
                return coopAiBurst(chr, b);
            }
            if (!coopAiArrived(chr, b) && g_GlobalTimer <= b->until60
                && !(b->state == AIS_ADVANCE && coopAiDist2(&chr->prop->pos, &b->lastpos) < 200.0f * 200.0f))
            {
                return 1;
            }
            if (b->state == AIS_ADVANCE)
            {   /* got there and no one: look round the place */
                b->goal = b->lastpos;
                b->goalstan = b->laststan;
                b->searchleft = (u8) coopAiSearchCount();
                coopAiSetState(b, AIS_SEARCH, 8);
                return 1;
            }
            coopAiSetState(b, AIS_COMBAT, 6);
            return 1;
        case AIS_GRENADE:
            if (g_GlobalTimer < b->since60 + COOPAI_SEC)
            {   /* the point, a second's warning */
                return 1;
            }
            if (target == NULL || coopAiBystander(&target->pos))
            {   /* someone wandered in during the warning */
                coopAiSetState(b, AIS_COMBAT, 1);
                return 1;
            }
            {
                u8 prob = chr->grenadeprob;
                s32 thrown;

                chr->grenadeprob = 0xFF;    /* Classic's throw, without its roll */
                thrown = actor_draws_throws_grenade_at_player_if_possible(chr);
                chr->grenadeprob = prob;
                portConLogf("ge: ai: guard %d %s at player %d\n", chr->chrnum,
                            thrown ? "throws a grenade" : "could not throw a grenade", b->target + 1);
            }
            coopAiSetState(b, AIS_COMBAT, 5);   /* the throw runs to its end before the next move */
            return 1;
        case AIS_ALARM:
            if (b->coverpad >= 0 && chrGetDistanceToPad(chr, b->coverpad) < 150.0f)
            {
                if (chrTryStartAlarm(chr, b->coverpad))
                {
                    portConLogf("ge: ai: guard %d sets off the alarm\n", chr->chrnum);
                }
                b->coverpad = -1;
                coopAiSetState(b, AIS_COMBAT, 6);
                return 1;
            }
            if (g_GlobalTimer > b->until60 || (stopped && g_GlobalTimer > b->since60 + COOPAI_SEC))
            {
                b->coverpad = -1;
                coopAiSetState(b, AIS_COMBAT, 6);
            }
            return 1;
        default:
            break;
    }

    /* AIS_COMBAT: decide once the last action is over */
    if (!stopped && g_GlobalTimer <= b->until60)
    {
        return 1;
    }
    if (coopAiPunishRevive(chr, b))
    {
        seen = 1;
    }
    coopAiSplit(chr, b);
    if (!b->called && seen)
    {
        b->called = 1;
        coopAiCallForHelp(chr, b);
        if (!alarmIsActive() && g_GlobalTimer > sAlarmRunUntil && coopAiAfford(1) >= 1
            && (getenv("GE_GUARDAI_TESTALARM") != NULL || coopAiRandom(4) == 0))
        {
            pad = coopAiFindAlarm(chr);
            if (pad < 0 && getenv("GE_GUARDAIDBG") != NULL)
            {
                portConLogf("ge: ai: guard %d would run for an alarm, none working within 30 m\n", chr->chrnum);
            }
            if (pad >= 0 && coopAiGoPad(chr, pad, coopAiPace(chr, b, SPEED_RUN)))
            {
                b->coverpad = pad;
                sAlarmRunUntil = g_GlobalTimer + 60 * COOPAI_SEC;
                coopAiSpend(1, "an alarm run");
                coopAiBark(chr, b, COOPBARK_ALARM);
                coopAiSetState(b, AIS_ALARM, 12);
                portConLogf("ge: ai: guard %d runs for the alarm at pad %d\n", chr->chrnum, pad);
                return 1;
            }
        }
        chrTrySurprisedOneHand(chr);   /* the call: Classic's point at Bond */
        coopAiSetState(b, AIS_COMBAT, 2);
        return 1;
    }
    if (!b->fellback && chr->maxdamage > 0.0f && chr->damage >= chr->maxdamage * 0.65f)
    {
        pad = coopAiFindCover(chr, 1);
        if (pad == -2)
        {
            return 1;
        }
        b->fellback = 1;
        if (pad >= 0 && coopAiGoPad(chr, pad, coopAiPace(chr, b, SPEED_RUN)))
        {
            b->coverpad = pad;
            b->firestan = NULL;     /* it stays back */
            coopAiSetState(b, AIS_FALLBACK, 6);
            coopAiBark(chr, b, COOPBARK_RETREAT);
            portConLogf("ge: ai: guard %d falls back to pad %d\n", chr->chrnum, pad);
            return 1;
        }
    }
    if (!b->surrendered && seen && target != NULL && b->role != AIR_RUSHER && coopAiHurt(chr) >= COOPAI_GIVEUP
        && coopAiDist2(&chr->prop->pos, &target->pos) < COOPAI_GIVEUP_NEAR * COOPAI_GIVEUP_NEAR && coopAiAlone(chr)
        && (getenv("GE_GUARDAI_TESTGIVEUP") != NULL || coopAiRandom(2) == 0))
    {   /* hurt, on its own, a player right there: hands up (once a fight: it
         * rolls once, as fellback) */
        b->fellback = 1;
        if (chrTrySurrender(chr))
        {
            b->surrendered = 1;
            b->delegated = 0;
            b->coverpad = -1;
            b->bark60 = 0;
            coopAiBark(chr, b, COOPBARK_GIVEUP);
            coopAiParkList(chr, b->origin);
            coopAiSetState(b, AIS_POST, 0);
            portConLogf("ge: ai: guard %d surrenders to player %d (%.0f%% hurt, alone)\n", chr->chrnum, b->target + 1,
                        coopAiHurt(chr) * 100.0f);
            return 1;
        }
    }
    if (!b->regrouped && target != NULL && b->role != AIR_RUSHER && sAlert >= AIA_ALERTED && coopAiAfford(1) >= 1
        && coopAiLosing(chr))
    {   /* two of its own down near it: back to where the others are */
        coord3d spot;
        StandTile *stan = NULL;

        b->regrouped = 1;
        if (coopAiRegroupSpot(chr, target, &spot, &stan) && coopAiGo(chr, &spot, stan, coopAiPace(chr, b, SPEED_RUN)))
        {
            b->firestan = NULL;
            b->coverpad = -1;
            coopAiBark(chr, b, COOPBARK_RETREAT);
            coopAiSetState(b, AIS_REGROUP, 15);
            coopAiSpend(1, "a regroup");
            portConLogf("ge: ai: guard %d regroups at (%.0f,%.0f), %.0f m back\n", chr->chrnum, spot.x, spot.z,
                        sqrtf(coopAiDist2(&spot, &chr->prop->pos)) / 100.0f);
            return 1;
        }
    }
    if (coopAiCamperGrenade(chr, b))
    {
        return 1;
    }
    if (seen)
    {
        s32 most = (b->role == AIR_RUSHER) ? 3 : (b->role == AIR_ANCHOR) ? 1 : 2;
        s32 hitmost = (b->role == AIR_RUSHER) ? 2 : (b->role == AIR_ANCHOR) ? 0 : 1;

        if (b->bursts >= most + coopAiRandom(2) || ((b->hit60 > b->since60 || b->supp60 > b->since60) && b->bursts >= hitmost))
        {
            pad = coopAiFindCover(chr, 0);
            if (pad == -2)
            {
                return 1;
            }
            if (pad >= 0)
            {
                b->firepos = chr->prop->pos;
                b->firestan = chr->prop->stan;
                if (coopAiGoPad(chr, pad, coopAiPace(chr, b, SPEED_RUN)))
                {
                    b->coverpad = pad;
                    if (coopAiRandom(2) == 0)
                    {
                        coopAiBark(chr, b, COOPBARK_COVER);
                    }
                    coopAiSetState(b, AIS_COVER, 5);
                    return 1;
                }
            }
            b->bursts = 0;
        }
        return coopAiBurst(chr, b);
    }

    /* out of sight */
    if (b->laststan != NULL && coopAiTargetReloading(b) && coopAiGo(chr, &b->lastpos, b->laststan, coopAiPace(chr, b, SPEED_RUN)))
    {   /* reloading round the corner: go now */
        coopAiSetState(b, AIS_ADVANCE, 10);
        return 1;
    }
    if (b->regrouped == 2)
    {   /* it made its stand here: it holds until it gives them up */
        coopAiSetState(b, AIS_COMBAT, 2);
        return 1;
    }
    if (g_GlobalTimer - b->lastsight60 < ((b->role == AIR_ANCHOR) ? 3 * COOPAI_SEC : (b->role == AIR_RUSHER) ? 0 : COOPAI_SEC))
    {   /* just now: hold a moment, as a player ducking out may be back (an
         * anchor longer, a rusher not at all) */
        coopAiSetState(b, AIS_COMBAT, 1);
        return 1;
    }
    if (coopEnabled() && target != NULL && target->stan != NULL && b->role != AIR_ANCHOR
        && coopAiCountState(AIS_CUTOFF) == 0 && coopAiAfford(1) >= 1 && (getenv("GE_GUARDAI_TESTCUTOFF") != NULL || coopAiRandom(2) == 0))
    {   /* a player off on their own: get between them and the others */
        f32 md2;
        PropRecord *mate = coopAiNearestMate(b->target, target, &md2);

        if (mate != NULL && md2 > COOPAI_SPLIT_DIST * COOPAI_SPLIT_DIST)
        {
            f32 rot = atan2f(mate->pos.x - target->pos.x, mate->pos.z - target->pos.z);

            if (rot < 0.0f)
            {
                rot += M_TAU_F;
            }
            pad = chrlvFindPathNeighborRelated(&target->pos, target->stan, rot, QUADRANT_FRONT);
            if (pad >= 0 && coopAiGoPad(chr, pad, coopAiPace(chr, b, SPEED_RUN)))
            {
                portConLogf("ge: ai: guard %d cuts player %d off from the others (%.0f m apart), to pad %d\n", chr->chrnum,
                            b->target + 1, sqrtf(md2) / 100.0f, pad);
                coopAiSetState(b, AIS_CUTOFF, 10);
                coopAiSpend(1, "a cut-off");
                return 1;
            }
        }
    }
    if (target != NULL && target->stan != NULL && coopAiRandom(3) < ((b->role == AIR_RUSHER) ? 2 : (b->role == AIR_ANCHOR) ? 0 : 1))
    {   /* round their side, along the waypoints by them */
        f32 rot = atan2f(chr->prop->pos.x - target->pos.x, chr->prop->pos.z - target->pos.z);

        if (rot < 0.0f)
        {
            rot += M_TAU_F;
        }
        pad = chrlvFindPathNeighborRelated(&target->pos, target->stan, rot,
                                           coopAiRandom(2) ? QUADRANT_SIDE1 : QUADRANT_SIDE2);
        if (pad >= 0 && coopAiGoPad(chr, pad, coopAiPace(chr, b, SPEED_RUN)))
        {
            coopAiSetState(b, AIS_FLANK, 8);
            return 1;
        }
    }
    if (b->laststan != NULL && coopAiGo(chr, &b->lastpos, b->laststan, coopAiPace(chr, b, SPEED_RUN)))
    {
        if (coopAiRandom(2) == 0)
        {
            coopAiBark(chr, b, COOPBARK_LOST);
        }
        coopAiSetState(b, AIS_ADVANCE, 10);
        return 1;
    }
    /* nowhere it can path to: Classic's run at Bond */
    chr->ailist = ailistFindById(GAILIST_RUN_TO_BOND);
    chr->aioffset = 0;
    chr->aireturnlist = GAILIST_STANDARD_GUARD;
    b->delegated = 1;
    coopAiSetState(b, AIS_COMBAT, 4);
    return 0;
}

/* ---- calm side: looking into things --------------------------------------- */

static void coopAiCalmDown(ChrRecord *chr, CoopAiBrain *b)
{
    s32 k;

    chr->lastheartarget60 = 0;
    chr->lastseetarget60 = 0;
    chr->hidden &= ~CHRHIDDEN_ALERT_GUARD_RELATED;
    b->called = b->answering = b->delegated = b->sweeping = 0;
    b->clueseen = ~0u;
    b->coverpad = -1;
    b->seen60 = 0;
    b->arghs = chr->numarghs;
    coopAiSetState(b, AIS_POST, 0);
    coopAiParkList(chr, b->origin);
    for (k = 0; k < COOPAI_BODIES; k++)
    {   /* back on post: what it can see from here it has seen (no walking
         * back out to a body it just stood beside in the fight or search) */
        ChrRecord *dead = coopAiBodyInView(chr, b);

        if (dead == NULL)
        {
            break;
        }
        coopAiNoteBody(b, dead);
    }
    b->nextbody60 = g_GlobalTimer + COOPAI_SEC;
}

static s32 coopAiLookInto(ChrRecord *chr, CoopAiBrain *b)
{
    s32 stopped = coopAiStopped(chr);

    if (chrHeardTargetRecently(chr) && chr->lastheartarget60 > b->heard60 && b->state != AIS_RETURN)
    {   /* another shot: that is where to look now */
        b->heard60 = chr->lastheartarget60;
        if (chr->targetTile != NULL)
        {
            b->goal = chr->lastknowntargetpos;
            b->goalstan = (StandTile *) chr->targetTile;
            coopAiRaise(AIA_SUSPICIOUS, "another shot");
            if (b->state != AIS_LOOK && coopAiGo(chr, &b->goal, b->goalstan, coopAiPace(chr, b, coopAiLookSpeed())))
            {
                coopAiSetState(b, AIS_INVESTIGATE, 15);
            }
        }
    }

    switch (b->state)
    {
        case AIS_LOOK:
            if ((stopped && g_GlobalTimer > b->since60 + COOPAI_SEC / 2) || g_GlobalTimer > b->until60)
            {
                if (coopAiGo(chr, &b->goal, b->goalstan, coopAiPace(chr, b, coopAiLookSpeed())))
                {
                    coopAiSetState(b, AIS_INVESTIGATE, 15);
                }
                else
                {
                    coopAiSetState(b, AIS_SEARCH, 8);
                }
            }
            return 1;
        case AIS_INVESTIGATE:
            if (!coopAiArrived(chr, b) && g_GlobalTimer <= b->until60
                && coopAiDist2(&chr->prop->pos, &b->goal) > 200.0f * 200.0f)
            {
                return 1;
            }
            coopAiSetState(b, AIS_SEARCH, 8);
            /* fall through */
        case AIS_SEARCH:
            if (!coopAiArrived(chr, b) && g_GlobalTimer <= b->until60)
            {   /* (not arrived in its first ticks either, so a new waypoint
                 * isn't picked before the walk to the last one has begun) */
                return 1;
            }
            if (b->searchleft > 0 && b->goalstan != NULL)
            {
                waypoint *wp = chrlvStanPathRelated(&b->goal, b->goalstan);

                if (getenv("GE_GUARDAIDBG") != NULL)
                {
                    portConLogf("ge: ai: guard %d searches from waypoint pad %d\n", chr->chrnum, wp ? wp->padID : -1);
                }
                b->searchleft--;
                if (wp != NULL)
                {
                    s32 n = 0, pick;

                    while (wp->neighbours[n] >= 0)
                    {
                        n++;
                    }
                    pick = (n > 0) ? g_CurrentSetup.pathwaypoints[wp->neighbours[coopAiRandom(n)]].padID : wp->padID;
                    if (coopAiGoPad(chr, pick, SPEED_WALK))
                    {
                        coopAiSetState(b, AIS_SEARCH, 8);
                        return 1;
                    }
                }
            }
            if (sAlert >= AIA_ALERTED && !b->sweeping && (sAlert >= AIA_LOCKDOWN || b->role == AIR_ANCHOR || coopAiRandom(2) == 0))
            {   /* on edge: it holds here rather than walking back to post */
                portConLogf("ge: ai: guard %d holds where it searched (%s)\n", chr->chrnum, sAlertNames[sAlert]);
                coopAiCalmDown(chr, b);
                return 1;
            }
            if (b->homestan != NULL && coopAiGo(chr, &b->home, b->homestan, SPEED_WALK))
            {
                coopAiSetState(b, AIS_RETURN, 25);
                return 1;
            }
            coopAiCalmDown(chr, b);
            return 1;
        case AIS_RETURN:
            if (coopAiArrived(chr, b) || g_GlobalTimer > b->until60)
            {
                coopAiCalmDown(chr, b);
            }
            return 1;
        default:
            break;
    }
    return 1;
}

/* ---- the hook -------------------------------------------------------------- */

/* GE_GUARDAI_TESTNOISE=<s>[,<loudness>]: once, <s> seconds into the
 * mission, a gunshot where Bond stands (harness only; works in Classic too) */
static void coopAiTestNoise(void)
{
    static s32 at = -1;
    static f32 loud;
    static s32 doneStage = -1;

    if (at == -1)
    {
        const char *e = getenv("GE_GUARDAI_TESTNOISE");
        float l = 20.0f;

        at = 0;
        if (e != NULL)
        {
            at = atoi(e) * COOPAI_SEC;
            if (strchr(e, ',') != NULL)
            {
                l = (float) atof(strchr(e, ',') + 1);
            }
        }
        loud = l;
    }
    if (at > 0 && g_GlobalTimer >= at && doneStage != bossGetStageNum())
    {
        doneStage = bossGetStageNum();
        portConLogf("ge: ai: test gunshot (loudness %.0f) at Bond, t=%d\n", loud, g_GlobalTimer);
        chrCheckGuardsHeardSound(loud);
    }
}

/* GE_GUARDAI_LISTCHRS=<s>: once, <s> seconds in, where each of our guards
 * stands and faces (harness only, to place a test player) */
static void coopAiTestList(void)
{
    static s32 at = -2, doneStage = -1;
    s32 i;

    if (at == -2)
    {
        const char *e = getenv("GE_GUARDAI_LISTCHRS");

        at = (e != NULL) ? atoi(e) * COOPAI_SEC : -1;
    }
    if (at < 0 || g_GlobalTimer < at || doneStage == bossGetStageNum() || g_ChrSlots == NULL)
    {
        return;
    }
    doneStage = bossGetStageNum();
    for (i = 0; i < g_NumChrSlots; i++)
    {
        ChrRecord *c = &g_ChrSlots[i];

        if (c->model != NULL && c->prop != NULL && coopAiOwns(c, coopAiListId(c)))
        {
            portConLogf("ge: ai: list: guard %d at %.0f %.0f %.0f facing %.2f\n", c->chrnum, c->prop->pos.x, c->prop->pos.y,
                        c->prop->pos.z, getsubroty(c->model));
        }
    }
    {   /* each shut door with one of ours within 12 m of it, and the waypoint pad nearest the door */
        PropRecord *prop;

        for (prop = chrpropGetActiveTail(); prop != NULL; prop = prop->prev)
        {
            waypoint *wp;
            s32 pad = -1, near = -1;
            f32 pd2 = 1.0e30f, nd2 = 1200.0f * 1200.0f;

            if (prop->type != PROP_TYPE_DOOR || prop->door == NULL || !doorIsClosed(prop->door))
            {
                continue;
            }
            for (i = 0; i < g_NumChrSlots; i++)
            {
                ChrRecord *c = &g_ChrSlots[i];

                if (c->model != NULL && c->prop != NULL && coopAiOwns(c, coopAiListId(c))
                    && coopAiDist2(&c->prop->pos, &prop->pos) < nd2)
                {
                    nd2 = coopAiDist2(&c->prop->pos, &prop->pos);
                    near = c->chrnum;
                }
            }
            if (near < 0 || g_CurrentSetup.pathwaypoints == NULL)
            {
                continue;
            }
            for (wp = g_CurrentSetup.pathwaypoints; wp->padID >= 0; wp++)
            {
                f32 d2 = coopAiDist2(&g_CurrentSetup.pads[wp->padID].pos, &prop->pos);

                if (d2 < pd2)
                {
                    pd2 = d2;
                    pad = wp->padID;
                }
            }
            portConLogf("ge: ai: list: door at %.0f %.0f %.0f, guard %d %.1f m, waypoint pad %d %.1f m\n", prop->pos.x,
                        prop->pos.y, prop->pos.z, near, sqrtf(nd2) / 100.0f, pad, sqrtf(pd2) / 100.0f);
        }
    }
}

/* GE_GUARDAI_TESTCLUE=<chrnum>:<s>: once, <s> seconds in, a smashed-object
 * clue on a waypoint 4 to 12 m from that guard that it can see from where it
 * stands (harness only: the noticing, without a player having to smash
 * something just there) */
static void coopAiTestClue(void)
{
    static s32 chrnum = -2, at, doneStage = -1;
    ChrRecord *c;
    waypoint *wp;

    if (chrnum == -2)
    {
        const char *e = getenv("GE_GUARDAI_TESTCLUE");

        if (e == NULL || sscanf(e, "%d:%d", &chrnum, &at) != 2)
        {
            chrnum = -1;
        }
    }
    if (chrnum < 0 || g_GlobalTimer < at * COOPAI_SEC || doneStage == bossGetStageNum() || g_CurrentSetup.pathwaypoints == NULL)
    {
        return;
    }
    doneStage = bossGetStageNum();
    c = chrFindByLiteralId(chrnum);
    if (c == NULL || c->prop == NULL || c->prop->stan == NULL)
    {
        portConLogf("ge: ai: testclue: no guard %d\n", chrnum);
        return;
    }
    for (wp = g_CurrentSetup.pathwaypoints; wp->padID >= 0; wp++)
    {
        PadRecord *pad = &g_CurrentSetup.pads[wp->padID];
        f32 d2 = coopAiDist2(&pad->pos, &c->prop->pos);

        if (pad->stan != NULL && d2 > 400.0f * 400.0f && d2 < 1200.0f * 1200.0f
            && check_if_position_in_same_room(c, &pad->pos, pad->stan))
        {
            coopAiAddClue(AIC_WRECK, &pad->pos, pad->stan, NULL);
            portConLogf("ge: ai: testclue: something smashed at pad %d, %.0f m from guard %d\n", wp->padID,
                        sqrtf(d2) / 100.0f, chrnum);
            return;
        }
    }
    portConLogf("ge: ai: testclue: no waypoint in sight of guard %d\n", chrnum);
}

/* GE_GUARDAI_TESTHURT=<chrnum>:<s>:<part>: once, <s> seconds in, that guard
 * has <part> (0 to 1) of its health gone, as if shot (harness only) */
static void coopAiTestHurt(void)
{
    static s32 chrnum = -2, at, doneStage = -1;
    static float part;
    ChrRecord *c;

    if (chrnum == -2)
    {
        const char *e = getenv("GE_GUARDAI_TESTHURT");

        if (e == NULL || sscanf(e, "%d:%d:%f", &chrnum, &at, &part) != 3)
        {
            chrnum = -1;
        }
    }
    if (chrnum < 0 || g_GlobalTimer < at * COOPAI_SEC || doneStage == bossGetStageNum())
    {
        return;
    }
    doneStage = bossGetStageNum();
    c = chrFindByLiteralId(chrnum);
    if (c != NULL && c->maxdamage > 0.0f)
    {
        c->damage = c->maxdamage * part;
        portConLogf("ge: ai: testhurt: guard %d at %.0f%% hurt\n", chrnum, part * 100.0f);
    }
}

/* chrai.c ai(): 1 when the brain ran this guard's tick, 0 to run its list */
s32 coopAiTick(ChrRecord *chr)
{
    CoopAiBrain *b;
    s32 id, seen;

    coopAiTestNoise();
    coopAiTestList();
    if (coopAiSmarter())
    {
        coopAiTestClue();
        coopAiTestHurt();
    }
    if (!coopAiSmarter() || chr == NULL)
    {
        return 0;
    }
    coopAiPlayersTick();
    id = coopAiListId(chr);
    b = coopAiBrainOf(chr);
    if (b == NULL)
    {
        return 0;
    }
    if (!b->dead && chrIsDead(chr) && (coopAiTerminal(id) || coopAiReaction(id) || b->state != AIS_POST || b->surrendered))
    {   /* one of ours down: counted for the others' "going badly" */
        b->dead = 1;
        sFallen[sNextFallen].pos = chr->prop->pos;
        sFallen[sNextFallen].at60 = g_GlobalTimer;
        sNextFallen = (sNextFallen + 1) % COOPAI_FALLEN;
    }
    if (b->surrendered)
    {
        if (chrIsDead(chr) || id != b->origin)
        {   /* dead, or a level script has it now */
            b->surrendered = 0;
            return 0;
        }
        if (chr->actiontype != ACT_SURRENDER && chrIsNotDeadOrShot(chr))
        {   /* up again after a hit: hands back up */
            chrTrySurrender(chr);
        }
        return 1;   /* its list stays still: it doesn't pick a gun back up */
    }
    if (!coopAiOwns(chr, id))
    {
        if (b->state != AIS_POST)
        {   /* a level script took it over */
            b->state = AIS_POST;
            b->delegated = 0;
            b->coverpad = -1;
        }
        return 0;
    }
    if (chrIsDead(chr) || !chrIsNotDeadOrShot(chr))
    {   /* dying, or taking a hit: Classic's lists handle both */
        return (b->state == AIS_POST || chrIsDead(chr)) ? 0 : 1;
    }
    b->target = (s8) coopChrCurrentTarget();

    if (b->state == AIS_POST)
    {
        if ((id == GAILIST_STANDARD_GUARD || id == GAILIST_SIMPLE_GUARD)
            && (chr->lastseetarget60 > b->seen60 || chr->numarghs > b->arghs))
        {   /* Classic saw them, or was shot, and its Attack Bond call already
             * returned: the fight is on */
            b->origin = (u8) id;
            coopAiEnterCombat(chr, b);
            b->bursts = 1;
        }
        else if (id == GAILIST_STANDARD_GUARD || id == GAILIST_SIMPLE_GUARD)
        {
            ChrRecord *dead;

            b->origin = (u8) id;
            s32 look = g_GlobalTimer >= b->nextbody60;  /* (the look below resets the timer) */

            dead = coopAiStopped(chr) ? coopAiFindBody(chr, b) : NULL;
            if (dead != NULL)
            {
                portConLogf("ge: ai: guard %d found a body (guard %d)\n", chr->chrnum, dead->chrnum);
                coopAiBark(chr, b, COOPBARK_BODY);
                b->heard60 = chr->lastheartarget60;
                coopAiInvestigate(chr, b, &dead->prop->pos, dead->prop->stan, 0);
                coopAiRaise(++sBodiesFound >= 2 ? AIA_LOCKDOWN : AIA_ALERTED, sBodiesFound >= 2 ? "a second body" : "a body");
                return 1;
            }
            if (look && coopAiStopped(chr) && chr->prop->stan != NULL)
            {
                b->origin = (u8) id;
                b->heard60 = chr->lastheartarget60;
                if (coopAiNotice(chr, b))
                {
                    coopAiParkList(chr, b->origin);
                    coopAiBark(chr, b, COOPBARK_CLUE);
                    return 1;
                }
            }
            return 0;   /* calm: Classic's own checks run */
        }
        else if (!coopAiReaction(id))
        {
            return 0;
        }
        else
        {   /* Classic just reacted: to what? */
            if (chrCheckTargetInSight(chr) || chrIfNearMiss(chr) || chr->numarghs > 0
                || g_GlobalTimer - chr->seen_bond_time < CHRLV_SEEN_RECENT_CHECK
                || id == GAILIST_STANDARD_CLONE || id == GAILIST_PERSISTENTLY_CHASE_AND_ATTACK_BOND)
            {
                coopAiEnterCombat(chr, b);
                coopAiParkList(chr, b->origin);
            }
            else if (chrHeardTargetRecently(chr) && chr->targetTile != NULL)
            {
                b->heard60 = chr->lastheartarget60;
                coopAiParkList(chr, b->origin);
                coopAiInvestigate(chr, b, &chr->lastknowntargetpos, (StandTile *) chr->targetTile, 0);
                return 1;
            }
            else
            {   /* saw someone shot or die: where they were */
                ChrRecord *other = NULL;

                if (chr->chrseedie >= 0)
                {
                    other = chrFindByLiteralId(chr->chrseedie);
                }
                else if (chr->chrseeshot >= 0)
                {
                    other = chrFindByLiteralId(chr->chrseeshot);
                }
                coopAiParkList(chr, b->origin);
                if (other != NULL && other->prop != NULL && other->prop->stan != NULL)
                {
                    coopAiNoteBody(b, other);   /* it saw this one fall: not a body to find later */
                    coopAiInvestigate(chr, b, &other->prop->pos, other->prop->stan, 0);
                    coopAiRaise(AIA_ALERTED, "saw a guard shot");
                    return 1;
                }
                coopAiEnterCombat(chr, b);
            }
        }
    }

    seen = coopAiSeesTarget(chr);
    if (seen)
    {
        PropRecord *target = getCurrentPlayerProp();

        b->lastsight60 = g_GlobalTimer;
        if (target != NULL)
        {
            b->lastpos = target->pos;
            b->laststan = target->stan;
        }
        if (b->state < AIS_COMBAT)
        {   /* found them while looking */
            coopAiEnterCombat(chr, b);
            chraiStopAnimation(chr);
        }
    }
    if (b->state >= AIS_COMBAT)
    {
        sLastFight60 = g_GlobalTimer;
        if (!seen && g_GlobalTimer - b->lastsight60 > coopAiLoseSecs() * COOPAI_SEC && b->state != AIS_ALARM)
        {   /* lost them for good: look round, then back to post (one that
             * regrouped looks round where it made its stand) */
            b->goal = (b->regrouped == 2) ? b->home : b->lastpos;
            b->goalstan = (b->regrouped == 2) ? b->homestan : b->laststan;
            b->searchleft = (u8) coopAiSearchCount();
            b->delegated = 0;
            coopAiParkList(chr, b->origin);
            coopAiBark(chr, b, COOPBARK_LOST);
            coopAiSetState(b, AIS_SEARCH, 8);
            return 1;
        }
        return coopAiCombat(chr, b, seen);
    }
    return coopAiLookInto(chr, b);
}

/* chr.c chrCheckGuardsHeardSound: a shot through a wall carries half as far
 * for smarter guards; 1 (Classic's range) otherwise */
f32 coopAiHearScale(ChrRecord *chr, f32 dist, f32 range)
{
    PropRecord *bond;
    StandTile *st;
    f32 h;
    s32 open;
    f32 keen;

    if (!coopAiSmarter() || chr->prop == NULL || chr->prop->stan == NULL || !coopAiOwns(chr, coopAiListId(chr)))
    {
        return 1.0f;
    }
    keen = ((sAlert >= AIA_LOCKDOWN) ? 1.25f : 1.0f) * coopAiAlertness();  /* lockdown: listening for it */
    if (dist < range * COOPAI_MUFFLED)
    {
        return keen;
    }
    if (dist >= range * keen)
    {   /* out of reach either way */
        return keen;
    }
    bond = getCurrentPlayerProp();
    if (bond == NULL || bond->stan == NULL)
    {
        return keen;
    }
    /* walls and shut doors muffle a shot; crates and guards don't */
    st = chr->prop->stan;
    h = chr->chrheight - 20.0f;
    chrSetMoving(chr, FALSE);
    open = stanTestLineUnobstructed(&st, chr->prop->pos.x, chr->prop->pos.z, bond->pos.x, bond->pos.z,
                                    CDTYPE_DOORS, h, h, 0.0f, 1.0f) && st == bond->stan;
    chrSetMoving(chr, TRUE);
    if (open)
    {
        return keen;
    }
    if (getenv("GE_GUARDAIDBG") != NULL)
    {
        portConLogf("ge: ai: guard %d hears it through a wall (%.0f of %.0f, tile %p bond %p)\n", chr->chrnum, dist, range, (void *) st, (void *) bond->stan);
    }
    return COOPAI_MUFFLED * coopAiAlertness();
}
