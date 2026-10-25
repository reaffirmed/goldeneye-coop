/**
 * Story co-op: every setup object's state, replicated from the host.
 *
 * The events in coop.c (doors, object damage, pickups, knocks, Use) carry
 * each change as it happens, but only for the changes someone remembered to
 * hook, and only to players already in the mission. Level scripts change
 * objects in ways no event covers (a door unlocked, a crate made visible, an
 * object destroyed or moved), and anything that happened before a joiner
 * arrived was never sent to it. This file makes those gaps converge instead
 * of last: the host describes each setup object (prop def) as a record, and
 * a joiner compares each record with its own copy and corrects the
 * difference.
 *
 *  - Join snapshot: when a player's Bond first shows up in the mission (and
 *    after a host migration, for everyone), the host sends every record,
 *    COOPREPL_SNAP_PER_TICK packets a tick. Those apply at once.
 *  - Periodic resync: the rest of the time the host walks the list a packet
 *    at a time (COOPREPL_CYCLE_TICKS apart). A difference has to still be
 *    there COOPREPL_PERSIST_TICKS after it was first seen before it is
 *    corrected, so an event in flight or a door closing on its own timer a
 *    moment later on one side is not fought.
 *
 * Corrections only move a joiner's world towards the host's and never undo
 * anything: an object the host still has is not brought back here, damage
 * is only added, a pickup only taken. What a record holds:
 *
 *  - present: the object has a prop. Gone on the host and not in our hands
 *    here: freed. In the host's inventory: a shared pickup (coop.c), so we
 *    take it too (armour just leaves the world).
 *  - destroyed level (objGetDestroyedLevel): the host's is higher, ours is
 *    damaged up to it, explosion and debris included, the way AI_DestroyObject
 *    does it.
 *  - flags and flags2, the bits level scripts set (AI_SetObjectFlags...),
 *    minus the ones the engine keeps per world (in motion, force collisions).
 *  - doors: lock (keyflags), and open or shut once both sides' door is still.
 *  - position and rotation, for loose objects at rest (not held, attached,
 *    moving, or a vehicle that drives itself).
 *
 * Runtime objects (thrown items, dropped guns) are coop_items.c's; chrs are
 * the guard snapshots'; stage flags, alarm and objective criteria the WORLD
 * packet's. Harness: GE_COOP_TESTREPL=<s> on the host flips the first
 * unlocked door's lock, hides a visible loose object, moves another and
 * destroys a third <s> seconds in, all without events, so only this file can
 * carry them; the joiner logs each correction.
 */
#include <ultra64.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include <boss.h>
#include "player.h"
#include "bondinv.h"
#include "bondview.h"
#include "propobj.h"
#include "coop.h"
#include "coop_net.h"

extern void portConLogf(const char *fmt, ...);
extern s32 sizepropdef(PropDefHeaderRecord *pdef);
extern s32 objGetDestroyedLevel(ObjectRecord *obj);
extern void objFreePermanently(ObjectRecord *obj, bool freeprop);
extern void setupUpdateObjectRoomPosition(ObjectRecord *obj);
extern StandTile *stanFindTileBelowPos(coord3d *pos, u8 *rooms, f32 *yRtn);

#define COOPREPL_MAX            1024    /* setup objects tracked per stage (Dam has ~300) */
#define COOPREPL_PER_PKT        16
#define COOPREPL_SNAP_PER_TICK  2
#define COOPREPL_CYCLE_TICKS    6       /* 10 packets a second while nothing is pending */
#define COOPREPL_PERSIST_TICKS  90
#define COOPREPL_POS_EPS        24.0f   /* units: closer than this counts as the same spot */

/* CoopReplRec.bits */
enum
{
    RB_PRESENT  = 1,    /* it has a prop */
    RB_HELD     = 2,    /* in the sender's inventory */
    RB_LOOSE    = 4,    /* at rest, free standing: position and rotation count */
    RB_DOOROPEN = 8,    /* door: open (any amount) */
    RB_DOORSTILL = 16,  /* door: not moving */
    RB_DOORDEAD = 32    /* door: destroyed */
};

typedef struct
{
    u16 idx;        /* the object's place among the stage's setup objects */
    u8 type;        /* PROPDEF_*: the same on both sides, or the lists differ */
    u8 bits;        /* RB_* */
    u8 destroyed;   /* objGetDestroyedLevel, capped */
    u8 pad[3];
    u32 flags;      /* COOPREPL_FLAGMASK of obj->flags */
    u32 flags2;
    u32 keyflags;   /* doors: the lock */
    f32 pos[3];
    f32 rot[9];     /* obj->mtx's 3x3 */
} CoopReplRec;

typedef struct
{
    u32 magic;
    u16 version;
    u16 type;       /* COOPPKT_REPL */
    s32 stage;
    u16 total;      /* records in the stage */
    u8 snap;        /* part of a join snapshot: apply now */
    u8 count;
    u32 snapid;     /* which snapshot (a joiner may get a second one) */
    CoopReplRec rec[COOPREPL_PER_PKT];
} CoopReplPkt;

/* engine-owned bits that differ between worlds and must not be copied */
#define COOPREPL_FLAGMASK (~(u32) (PROPFLAG_INMOTION | PROPFLAG_00000100))

static s32 sOfs[COOPREPL_MAX];  /* byte offsets of the setup objects, in setup order */
static s32 sNum;
static void *sListBase;         /* the prop defs sOfs was built from */
static s32 sListStage;

/* host: per player */
static struct
{
    s32 seen;       /* their Bond is in this mission */
    s32 snapAt;     /* next record of the snapshot being sent, -1 none */
    u32 snapid;
    s32 cursor;     /* the periodic walk */
} sOut[COOP_MAX_PLAYERS];
static u32 sSnapSerial;
static u32 sTick;

/* joiner */
static u32 sFirstMiss[COOPREPL_MAX];    /* sTick a difference was first seen, 0 none */
static u8 sSnapGot[COOPREPL_MAX];
static u32 sSnapId;
static s32 sSnapLeft = -1;      /* records of the snapshot not in yet; 0 done; -1 none yet */
static s32 sFixes;

static s32 coopReplIsObject(s32 type)
{
    switch (type)
    {
        case PROPDEF_DOOR:
        case PROPDEF_PROP:
        case PROPDEF_KEY:
        case PROPDEF_ALARM:
        case PROPDEF_CCTV:
        case PROPDEF_MAGAZINE:
        case PROPDEF_COLLECTABLE:
        case PROPDEF_MONITOR:
        case PROPDEF_MULTI_MONITOR:
        case PROPDEF_RACK:
        case PROPDEF_AUTOGUN:
        case PROPDEF_HAT:
        case PROPDEF_AMMO:
        case PROPDEF_ARMOUR:
        case PROPDEF_GAS_RELEASING:
        case PROPDEF_VEHICHLE:
        case PROPDEF_AIRCRAFT:
        case PROPDEF_UNK41:
        case PROPDEF_GLASS:
        case PROPDEF_SAFE:
        case PROPDEF_TANK:
        case PROPDEF_TINTED_GLASS:
            return 1;
    }
    return 0;
}

/* the stage's setup objects, in setup order: the same list on every game */
static s32 coopReplList(void)
{
    PropDefHeaderRecord *it = (PropDefHeaderRecord *) g_CurrentSetup.propDefs;

    if (it == NULL)
    {
        sNum = 0;
        return 0;
    }
    if ((void *) it == sListBase && sListStage == bossGetStageNum())
    {
        return sNum;
    }
    sNum = 0;
    for (; it->type != PROPDEF_END; it = (PropDefHeaderRecord *) ((u32 *) it + sizepropdef(it)))
    {
        if (coopReplIsObject(it->type) && sNum < COOPREPL_MAX)
        {
            sOfs[sNum++] = (s32) ((u8 *) it - (u8 *) g_CurrentSetup.propDefs);
        }
    }
    sListBase = g_CurrentSetup.propDefs;
    sListStage = bossGetStageNum();
    portConLogf("ge: coop: repl: %d setup objects in stage %d\n", sNum, sListStage);
    return sNum;
}

static ObjectRecord *coopReplObj(s32 i)
{
    return (ObjectRecord *) ((u8 *) g_CurrentSetup.propDefs + sOfs[i]);
}

static s32 coopReplDrivesItself(s32 type)
{
    return type == PROPDEF_VEHICHLE || type == PROPDEF_AIRCRAFT || type == PROPDEF_TANK || type == PROPDEF_DOOR;
}

/* this world's record for object i */
static void coopReplCapture(s32 i, CoopReplRec *r)
{
    ObjectRecord *obj = coopReplObj(i);
    PropRecord *prop = obj->prop;
    s32 lvl;

    memset(r, 0, sizeof(*r));
    r->idx = (u16) i;
    r->type = obj->type;
    r->flags = obj->flags & COOPREPL_FLAGMASK;
    r->flags2 = obj->flags2;
    lvl = (prop != NULL) ? objGetDestroyedLevel(obj) : 0;
    r->destroyed = (u8) (lvl > 255 ? 255 : lvl);
    if (prop == NULL)
    {
        return;
    }
    r->bits |= RB_PRESENT;
    if (bondinvHasPropInInv(prop))
    {
        r->bits |= RB_HELD;
    }
    if (obj->type == PROPDEF_DOOR)
    {
        DoorRecord *door = (DoorRecord *) obj;

        r->keyflags = door->keyflags;
        if (door->openPosition > 0.0f)
        {
            r->bits |= RB_DOOROPEN;
        }
        if (door->openstate == DOORSTATE_STATIONARY)
        {
            r->bits |= RB_DOORSTILL;
        }
        if (obj->runtime_bitflags & RUNTIMEBITFLAG_DESTROYED)
        {
            r->bits |= RB_DOORDEAD;
        }
    }
    if (!(r->bits & RB_HELD) && prop->parent == NULL && !(obj->flags & PROPFLAG_INMOTION)
        && !(obj->runtime_bitflags & RUNTIMEBITFLAG_HASPROJECTILE) && !coopReplDrivesItself(obj->type))
    {
        s32 a, b;

        r->bits |= RB_LOOSE;
        r->pos[0] = prop->pos.f[0];
        r->pos[1] = prop->pos.f[1];
        r->pos[2] = prop->pos.f[2];
        for (a = 0; a < 3; a++)
        {
            for (b = 0; b < 3; b++)
            {
                r->rot[a * 3 + b] = obj->mtx.m[a][b];
            }
        }
    }
}

/* ---- host ------------------------------------------------------------------ */

void coopReplSendSnapshot(s32 player)
{
    if (player <= 0 || player >= COOP_MAX_PLAYERS)
    {
        return;
    }
    sOut[player].snapAt = 0;
    sOut[player].snapid = ++sSnapSerial;
    portConLogf("ge: coop: repl: full snapshot to player %d (%d objects)\n", player + 1, sNum);
}

static void coopReplSend(s32 player, s32 from, s32 snap, u32 snapid)
{
    CoopReplPkt p;
    s32 k;

    memset(&p, 0, sizeof(p));
    p.type = COOPPKT_REPL;
    p.stage = bossGetStageNum();
    p.total = (u16) sNum;
    p.snap = (u8) snap;
    p.snapid = snapid;
    for (k = 0; k < COOPREPL_PER_PKT && from + k < sNum; k++)
    {
        coopReplCapture(from + k, &p.rec[k]);
    }
    p.count = (u8) k;
    coopExtSendPacket(&p, (s32) ((u8 *) &p.rec[k] - (u8 *) &p), player);
}

static void coopReplHostTick(void)
{
    s32 p;

    for (p = 1; p < COOP_MAX_PLAYERS; p++)
    {
        s32 n;

        if (!coopReplPlayerHere(p))
        {
            sOut[p].seen = 0;
            continue;
        }
        if (!sOut[p].seen)
        {   /* they just arrived: everything the world went through without them */
            sOut[p].seen = 1;
            coopReplSendSnapshot(p);
        }
        if (sOut[p].snapAt >= 0)
        {
            for (n = 0; n < COOPREPL_SNAP_PER_TICK && sOut[p].snapAt < sNum; n++)
            {
                coopReplSend(p, sOut[p].snapAt, 1, sOut[p].snapid);
                sOut[p].snapAt += COOPREPL_PER_PKT;
            }
            if (sOut[p].snapAt >= sNum)
            {
                sOut[p].snapAt = -1;
            }
            continue;
        }
        if ((sTick % COOPREPL_CYCLE_TICKS) == (u32) p % COOPREPL_CYCLE_TICKS)
        {
            if (sOut[p].cursor >= sNum)
            {
                sOut[p].cursor = 0;
            }
            coopReplSend(p, sOut[p].cursor, 0, 0);
            sOut[p].cursor += COOPREPL_PER_PKT;
        }
    }
}

/* the host changed hands: the new host's world is the truth now, so everyone gets it */
void coopReplMigrated(void)
{
    s32 p;

    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        sOut[p].seen = 0;
        sOut[p].snapAt = -1;
        sOut[p].cursor = 0;
    }
    memset(sFirstMiss, 0, sizeof(sFirstMiss));
}

/* ---- joiner ---------------------------------------------------------------- */

static s32 coopReplPosDiffers(const CoopReplRec *a, const CoopReplRec *b)
{
    f32 dx = a->pos[0] - b->pos[0];
    f32 dy = a->pos[1] - b->pos[1];
    f32 dz = a->pos[2] - b->pos[2];

    return dx * dx + dy * dy + dz * dz > COOPREPL_POS_EPS * COOPREPL_POS_EPS;
}

/* what about ours differs from the host's that we can and may correct (COOPFIX_*) */
enum { COOPFIX_GONE = 1, COOPFIX_TAKE = 2, COOPFIX_DESTROY = 4, COOPFIX_FLAGS = 8, COOPFIX_LOCK = 16, COOPFIX_DOOR = 32, COOPFIX_MOVE = 64 };

static s32 coopReplDiff(const CoopReplRec *host, const CoopReplRec *ours)
{
    s32 fix = 0;

    if (!(ours->bits & RB_PRESENT) || (ours->bits & RB_HELD))
    {   /* nothing here to correct: gone, or in our hands (a pickup on its way to the host) */
        return 0;
    }
    if (!(host->bits & RB_PRESENT))
    {
        return COOPFIX_GONE;
    }
    if (host->bits & RB_HELD)
    {
        return COOPFIX_TAKE;
    }
    if (host->destroyed > ours->destroyed)
    {
        fix |= COOPFIX_DESTROY;
    }
    if (host->flags != ours->flags || host->flags2 != ours->flags2)
    {
        fix |= COOPFIX_FLAGS;
    }
    if (host->type == PROPDEF_DOOR)
    {
        if (host->keyflags != ours->keyflags)
        {
            fix |= COOPFIX_LOCK;
        }
        if ((host->bits & RB_DOORSTILL) && (ours->bits & RB_DOORSTILL) && !(ours->bits & RB_DOORDEAD)
            && ((host->bits ^ ours->bits) & RB_DOOROPEN))
        {
            fix |= COOPFIX_DOOR;
        }
    }
    if ((host->bits & RB_LOOSE) && (ours->bits & RB_LOOSE) && !(fix & COOPFIX_DESTROY) && coopReplPosDiffers(host, ours))
    {
        fix |= COOPFIX_MOVE;
    }
    return fix;
}

static void coopReplMove(ObjectRecord *obj, const CoopReplRec *host)
{
    PropRecord *prop = obj->prop;
    coord3d pos;
    StandTile *stan;
    s32 a, b;

    pos.f[0] = host->pos[0];
    pos.f[1] = host->pos[1];
    pos.f[2] = host->pos[2];
    stan = stanFindTileBelowPos(&pos, NULL, NULL);
    if (stan == NULL)
    {
        return;
    }
    for (a = 0; a < 3; a++)
    {
        for (b = 0; b < 3; b++)
        {
            obj->mtx.m[a][b] = host->rot[a * 3 + b];
        }
    }
    obj->runtime_pos = pos;
    prop->pos = pos;
    prop->stan = stan;
    chrobjCollisionRelated(obj);
    setupUpdateObjectRoomPosition(obj);
}

static void coopReplFix(s32 i, const CoopReplRec *host, s32 fix)
{
    ObjectRecord *obj = coopReplObj(i);

    sFixes++;
    portConLogf("ge: coop: repl: obj %d (ofs %d type %d) differs from the host's (fix %02x, flags ^%08x ^%08x): corrected\n", i, sOfs[i],
                obj->type, fix, (obj->flags & COOPREPL_FLAGMASK) ^ host->flags, obj->flags2 ^ host->flags2);
    if (fix & COOPFIX_GONE)
    {
        objFreePermanently(obj, TRUE);
        return;
    }
    if (fix & COOPFIX_TAKE)
    {
        coopReplTeamPick(obj);
        return;
    }
    if (fix & COOPFIX_FLAGS)
    {
        obj->flags = (obj->flags & ~COOPREPL_FLAGMASK) | (host->flags & COOPREPL_FLAGMASK);
        obj->flags2 = host->flags2;
    }
    if (fix & COOPFIX_LOCK)
    {
        ((DoorRecord *) obj)->keyflags = host->keyflags;
    }
    if (fix & COOPFIX_DOOR)
    {
        coopReplDoorSet((DoorRecord *) obj, (host->bits & RB_DOOROPEN) ? DOORSTATE_OPENING : DOORSTATE_CLOSING);
    }
    if (fix & COOPFIX_MOVE)
    {
        coopReplMove(obj, host);
    }
    if (fix & COOPFIX_DESTROY)
    {
        s32 tries;

        for (tries = 0; tries < 8 && obj->prop != NULL && objGetDestroyedLevel(obj) < host->destroyed; tries++)
        {
            coopReplDestroy(obj);
        }
    }
}

static void coopReplApply(const CoopReplRec *host, s32 snap)
{
    CoopReplRec ours;
    s32 i = host->idx;
    s32 fix;

    if (i >= sNum || coopReplObj(i)->type != host->type)
    {
        return;
    }
    coopReplCapture(i, &ours);
    fix = coopReplDiff(host, &ours);
    if (fix == 0)
    {
        sFirstMiss[i] = 0;
        return;
    }
    if (!snap)
    {   /* it has to last: an event may be on its way, or a door closing a moment later here */
        if (sFirstMiss[i] == 0)
        {
            sFirstMiss[i] = sTick;
            return;
        }
        if (sTick - sFirstMiss[i] < COOPREPL_PERSIST_TICKS)
        {
            return;
        }
    }
    sFirstMiss[i] = 0;
    coopReplFix(i, host, fix);
}

void coopReplPacket(const void *buf, s32 len, s32 from)
{
    const CoopReplPkt *p = (const CoopReplPkt *) buf;
    s32 hdr = (s32) ((const u8 *) &p->rec[0] - (const u8 *) p);
    s32 k;

    if (from != 0 || coopNetIsHost() || len < hdr || p->count > COOPREPL_PER_PKT
        || len < hdr + p->count * (s32) sizeof(CoopReplRec) || !coopReplHostHere())
    {
        return;
    }
    if (p->stage != bossGetStageNum() || coopReplList() != p->total)
    {
        if (p->stage == bossGetStageNum() && (sTick % 300) == 0)
        {
            portConLogf("ge: coop: repl: host has %d setup objects, we have %d: not applied\n", p->total, sNum);
        }
        return;
    }
    if (p->snap && p->snapid != sSnapId)
    {
        sSnapId = p->snapid;
        sSnapLeft = sNum;
        sFixes = 0;
        memset(sSnapGot, 0, sizeof(sSnapGot));
    }
    for (k = 0; k < p->count; k++)
    {
        s32 i = p->rec[k].idx;

        coopReplApply(&p->rec[k], p->snap);
        if (p->snap && p->snapid == sSnapId && i < sNum && !sSnapGot[i])
        {
            sSnapGot[i] = 1;
            if (--sSnapLeft == 0)
            {
                portConLogf("ge: coop: repl: join snapshot applied, %d objects, %d corrected\n", sNum, sFixes);
            }
        }
    }
}

s32 coopReplSnapshotDone(void)
{
    return sSnapLeft == 0;
}

/* ---- harness ---------------------------------------------------------------- */

/* GE_COOP_TESTREPL=<s>: the host changes four things near its Bond behind the
 * events' back: the nearest loose object destroyed, the next moved 1 m, the
 * next one's flags2 changed, the nearest door's lock flipped */
static s32 coopReplNearest(s32 type, s32 loose, const u8 *used)
{
    PropRecord *bond = getCurrentPlayerProp();
    f32 best = 0.0f;
    s32 i, pick = -1;

    for (i = 0; i < sNum; i++)
    {
        ObjectRecord *obj = coopReplObj(i);
        CoopReplRec r;
        f32 dx, dz;

        if (used[i] || obj->type != type || obj->prop == NULL)
        {
            continue;
        }
        coopReplCapture(i, &r);
        if (loose && (!(r.bits & RB_LOOSE) || r.destroyed || (obj->flags & PROPFLAG_INVINCIBLE)))
        {
            continue;
        }
        dx = obj->prop->pos.f[0] - bond->pos.f[0];
        dz = obj->prop->pos.f[2] - bond->pos.f[2];
        if (pick < 0 || dx * dx + dz * dz < best)
        {
            best = dx * dx + dz * dz;
            pick = i;
        }
    }
    return pick;
}

/* harness: put object i on the floor <ahead> units in front of our Bond, <side> to the right */
static void coopReplPlaceAhead(s32 i, f32 ahead, f32 side)
{
    PropRecord *bond = getCurrentPlayerProp();
    Mtxf *v2w = currentPlayerGetViewToWorldMtxf();
    CoopReplRec to;
    coord3d pos;
    f32 fx = -v2w->m[2][0], fz = -v2w->m[2][2];
    f32 len = sqrtf(fx * fx + fz * fz);
    f32 floory = bond->pos.f[1];

    if (len < 0.01f)
    {
        return;
    }
    fx /= len;
    fz /= len;
    coopReplCapture(i, &to);
    pos.f[0] = bond->pos.f[0] + fx * ahead - fz * side;
    pos.f[1] = bond->pos.f[1] + 100.0f;
    pos.f[2] = bond->pos.f[2] + fz * ahead + fx * side;
    if (stanFindTileBelowPos(&pos, NULL, &floory) == NULL)
    {
        return;
    }
    to.pos[0] = pos.f[0];
    to.pos[1] = floory;
    to.pos[2] = pos.f[2];
    coopReplMove(coopReplObj(i), &to);
}

static void coopReplHarness(u32 missionticks)
{
    static s32 at = -2;
    static u8 used[COOPREPL_MAX];
    s32 i;

    if (at == -2)
    {
        const char *e = getenv("GE_COOP_TESTREPL");

        at = (e != NULL) ? atoi(e) * 60 : -1;
    }
    if (at < 0 || missionticks != (u32) at)
    {
        return;
    }
    at = -1;
    memset(used, 0, sizeof(used));
    if ((i = coopReplNearest(PROPDEF_PROP, 1, used)) >= 0)
    {   /* in front of us, on the left, then wrecked */
        used[i] = 1;
        coopReplPlaceAhead(i, 500.0f, -120.0f);
        coopReplDestroy(coopReplObj(i));
        portConLogf("ge: coop: repl test: obj %d moved ahead and destroyed (level %d)\n", i, objGetDestroyedLevel(coopReplObj(i)));
    }
    if ((i = coopReplNearest(PROPDEF_PROP, 1, used)) >= 0)
    {   /* in front of us, on the right */
        used[i] = 1;
        coopReplPlaceAhead(i, 500.0f, 120.0f);
        portConLogf("ge: coop: repl test: obj %d moved ahead to %.0f %.0f %.0f\n", i, coopReplObj(i)->prop->pos.f[0],
                    coopReplObj(i)->prop->pos.f[1], coopReplObj(i)->prop->pos.f[2]);
    }
    if ((i = coopReplNearest(PROPDEF_PROP, 1, used)) >= 0)
    {
        used[i] = 1;
        coopReplObj(i)->flags2 ^= 0x00000010;
        portConLogf("ge: coop: repl test: obj %d flags2 now %x\n", i, coopReplObj(i)->flags2);
    }
    if ((i = coopReplNearest(PROPDEF_DOOR, 0, used)) >= 0)
    {
        ((DoorRecord *) coopReplObj(i))->keyflags ^= 0x40;
        portConLogf("ge: coop: repl test: door obj %d lock now %x\n", i, ((DoorRecord *) coopReplObj(i))->keyflags);
    }
}

void coopReplStageStart(void)
{
    s32 p;

    sListBase = NULL;
    sNum = 0;
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        sOut[p].seen = 0;
        sOut[p].snapAt = -1;
        sOut[p].cursor = 0;
    }
    memset(sFirstMiss, 0, sizeof(sFirstMiss));
    sSnapLeft = -1;
    sSnapId = 0;
}

void coopReplTick(u32 missionticks)
{
    sTick++;
    if (coopReplList() == 0)
    {
        return;
    }
    if (coopNetIsHost())
    {
        coopReplHarness(missionticks);
        coopReplHostTick();
    }
}
