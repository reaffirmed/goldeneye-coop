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
extern int ge_shot_force;               /* gfx_sdl2.cpp: a screenshot this frame, to ge_shot_path */
extern char ge_shot_path[512];
extern s32 portOptFind(const char *key);
extern s32 portOptSetValue(s32 i, s32 v);
extern int coopNetIsHost(void);

extern s32 plot_course_for_actor(ChrRecord *self, coord3d *pos, StandTile *stan, SPEED speed);
extern s32 chrIsNotDeadOrShot(ChrRecord *chr);
extern s32 chrlvFindPathNeighborRelated(coord3d *bondpos, StandTile *stan, f32 rot, u8 quadrant);
extern waypoint *chrlvStanPathRelated(coord3d *pos, StandTile *stan);

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

enum { AIA_CALM = 0, AIA_SUSPICIOUS, AIA_ALERTED, AIA_LOCKDOWN, AIA_COUNT };

/* barks (COOPBARK_*): what a guard calls out, and the game's own sound that goes with it */
static const char *const sBarkText[COOPBARK_COUNT] = {
    "OVER THERE!\n", "WHERE DID THEY GO?\n", "NEED BACKUP!\n", "COVER ME!\n", "GRENADE!\n",
    "MAN DOWN!\n", "SOUND THE ALARM!\n", "FALL BACK!\n",
};

static const s16 sBarkSting[COOPBARK_COUNT] = {
    CAMERA_BEEP1_SFX, BEEP_QUIET_SFX, RADIO_SFX, GUN_RIFLECOCK_SFX, BING_SFX,
    RADIO_SFX, ALARM_SWITCH_SFX, BEEP_QUIET_SFX,
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
    AIS_COUNT
};

static const char *const sStateNames[AIS_COUNT] = {
    "post", "look", "investigate", "search", "return", "combat", "advance",
    "cover", "duck", "peek", "flank", "alarm", "fall back", "grenade",
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

/* ---- the setting -------------------------------------------------------- */

static s32 coopAiDecideOn(void)
{
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

    if (e != NULL)
    {
        return atoi(e) == COOPAI_SMARTER ? COOPAI_SMARTER : COOPAI_CLASSIC;
    }
    return portGuardAiSmarter() ? COOPAI_SMARTER : COOPAI_CLASSIC;
}

/* the host's lobby row edits the same Port Option (saved to midas.ini) */
void coopAiSetSmarterOption(s32 mode)
{
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
        if (sLatchOn)
        {
            portConLogf("ge: ai: smarter guards on (stage %d)\n", stage);
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
    slot = chr - g_ChrSlots;
    if (slot < 0 || slot >= g_NumChrSlots || slot >= COOPAI_MAX_CHRS)
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

    if (kind < 0 || kind >= COOPBARK_COUNT || !portGuardBarks() || chr == NULL || chr->prop == NULL)
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

    if (sAlert < AIA_LOCKDOWN || g_GlobalTimer < sNextSweep60 || g_ChrSlots == NULL)
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
    }

    if (alarmIsActive())
    {
        coopAiRaise(AIA_LOCKDOWN, "the alarm");
    }
    quiet = g_GlobalTimer - ((sAlertRaised60 > sLastFight60) ? sAlertRaised60 : sLastFight60);
    if (sAlert == AIA_ALERTED && sAlertFloor < AIA_ALERTED && quiet > 90 * COOPAI_SEC)
    {
        portConLogf("ge: ai: alert alerted -> suspicious (quiet 90 s)\n");
        sAlert = AIA_SUSPICIOUS;
        sAlertRaised60 = g_GlobalTimer;
    }
    else if (sAlert == AIA_SUSPICIOUS && sAlertFloor < AIA_SUSPICIOUS && quiet > 45 * COOPAI_SEC)
    {
        portConLogf("ge: ai: alert suspicious -> calm (quiet 45 s)\n");
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
        if (!coopAiGo(chr, &b->goal, b->goalstan, SPEED_RUN))
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

    s32 most = COOPAI_CALL_MAX + (sAlert >= AIA_ALERTED);

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
        if (coopAiDist2(&dead->prop->pos, &chr->prop->pos) > COOPAI_BODY_RANGE * COOPAI_BODY_RANGE)
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

    if (p == NULL || target == NULL || b->threw || g_GlobalTimer < sNextNade60
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
    }
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
        case AIS_COVER:
        case AIS_FALLBACK:
            if (coopAiArrived(chr, b) || g_GlobalTimer > b->until60)
            {
                coopAiSetState(b, AIS_DUCK, 1 + coopAiRandom(2));
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
                if (b->firestan != NULL && coopAiGo(chr, &b->firepos, b->firestan, SPEED_RUN))
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
        if (!alarmIsActive() && g_GlobalTimer > sAlarmRunUntil
            && (getenv("GE_GUARDAI_TESTALARM") != NULL || coopAiRandom(4) == 0))
        {
            pad = coopAiFindAlarm(chr);
            if (pad < 0 && getenv("GE_GUARDAIDBG") != NULL)
            {
                portConLogf("ge: ai: guard %d would run for an alarm, none working within 30 m\n", chr->chrnum);
            }
            if (pad >= 0 && coopAiGoPad(chr, pad, SPEED_RUN))
            {
                b->coverpad = pad;
                sAlarmRunUntil = g_GlobalTimer + 60 * COOPAI_SEC;
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
        if (pad >= 0 && coopAiGoPad(chr, pad, SPEED_RUN))
        {
            b->coverpad = pad;
            b->firestan = NULL;     /* it stays back */
            coopAiSetState(b, AIS_FALLBACK, 6);
            coopAiBark(chr, b, COOPBARK_RETREAT);
            portConLogf("ge: ai: guard %d falls back to pad %d\n", chr->chrnum, pad);
            return 1;
        }
    }
    if (coopAiCamperGrenade(chr, b))
    {
        return 1;
    }
    if (seen)
    {
        if (b->bursts >= 2 + coopAiRandom(2) || ((b->hit60 > b->since60 || b->supp60 > b->since60) && b->bursts >= 1))
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
                if (coopAiGoPad(chr, pad, SPEED_RUN))
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
    if (b->laststan != NULL && coopAiTargetReloading(b) && coopAiGo(chr, &b->lastpos, b->laststan, SPEED_RUN))
    {   /* reloading round the corner: go now */
        coopAiSetState(b, AIS_ADVANCE, 10);
        return 1;
    }
    if (g_GlobalTimer - b->lastsight60 < COOPAI_SEC)
    {   /* just now: hold a moment, as a player ducking out may be back */
        coopAiSetState(b, AIS_COMBAT, 1);
        return 1;
    }
    if (target != NULL && target->stan != NULL && coopAiRandom(3) == 0)
    {   /* round their side, along the waypoints by them */
        f32 rot = atan2f(chr->prop->pos.x - target->pos.x, chr->prop->pos.z - target->pos.z);

        if (rot < 0.0f)
        {
            rot += M_TAU_F;
        }
        pad = chrlvFindPathNeighborRelated(&target->pos, target->stan, rot,
                                           coopAiRandom(2) ? QUADRANT_SIDE1 : QUADRANT_SIDE2);
        if (pad >= 0 && coopAiGoPad(chr, pad, SPEED_RUN))
        {
            coopAiSetState(b, AIS_FLANK, 8);
            return 1;
        }
    }
    if (b->laststan != NULL && coopAiGo(chr, &b->lastpos, b->laststan, SPEED_RUN))
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
            if (b->state != AIS_LOOK && coopAiGo(chr, &b->goal, b->goalstan, coopAiLookSpeed()))
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
                if (coopAiGo(chr, &b->goal, b->goalstan, coopAiLookSpeed()))
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
            if (sAlert >= AIA_ALERTED && !b->sweeping && (sAlert >= AIA_LOCKDOWN || coopAiRandom(2) == 0))
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

/* chrai.c ai(): 1 when the brain ran this guard's tick, 0 to run its list */
s32 coopAiTick(ChrRecord *chr)
{
    CoopAiBrain *b;
    s32 id, seen;

    coopAiTestNoise();
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
        {   /* lost them for good: look round, then back to post */
            b->goal = b->lastpos;
            b->goalstan = b->laststan;
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
    keen = (sAlert >= AIA_LOCKDOWN) ? 1.25f : 1.0f;    /* lockdown: listening for it */
    if (dist < range * COOPAI_MUFFLED)
    {
        return keen;
    }
    if (dist >= range * keen)
    {
        return 1.0f;
    }
    bond = getCurrentPlayerProp();
    if (bond == NULL || bond->stan == NULL)
    {
        return 1.0f;
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
    return COOPAI_MUFFLED;
}
