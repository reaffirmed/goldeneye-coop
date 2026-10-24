/**
 * Story co-op: the other player's Bond as a puppet character.
 *
 * Each field tick this sends the local Bond's position and facing to the peer,
 * and moves a puppet chr to the peer's last reported state. The puppet runs the
 * dead AI list and an action type the tick dispatcher ignores, so only this file
 * moves it.
 *
 * Guard targeting (host): each guard keeps a target, the local Bond or the
 * puppet. While a guard targeting the puppet runs its action and fire ticks,
 * getCurrentPlayerProp() returns the puppet, so the retail "Bond" AI sees,
 * chases and shoots the remote player; damage it deals goes to the peer as a
 * hit packet. The acquire/commit/release rules are ported from GoldenEye 007
 * Plus (src/game/chr.c chrCoopGetTargetPlayer), cut down to two targets.
 */
#include <ultra64.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <bondaicommands.h>
#include <bondgame.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <boss.h>
#include "bondview.h"
#include "chr.h"
#include "chrai.h"
#include "chraction.h"
#include "initanitable.h"
#include "model.h"
#include "stan.h"
#include "player.h"
#include "lv.h"
#include "coop.h"
#include "coop_net.h"
#include "propobj.h"

void doorSetOpenState(DoorRecord *door, s32 newstate);
bool doorIsClosed(DoorRecord *door);

extern void portConLogf(const char *fmt, ...);

#define COOP_MAGIC   0x50434547u /* "GECP" */
#define COOP_VERSION 1

enum { COOPPKT_STATE = 1, COOPPKT_GUARDS = 3, COOPPKT_EVENTS = 5 };

typedef struct
{
    u32 magic;
    u16 version;
    u16 type;
    u32 tick;
    s32 stage;
    f32 pos[3];
    f32 yaw;        /* radians, model convention */
    s32 bodynum;
    s32 headnum;
    s32 dead;
} CoopStatePkt;

/* Reliable, in-order events (hits, shots, doors, ...). Every EVENTS packet
 * carries all unacked events plus an ack of what we have received. */
enum { COOPEV_HIT = 1, COOPEV_SHOTCHR = 2, COOPEV_DOOR = 3 };

typedef struct
{
    u8 kind;
    u8 pad[3];
    union
    {
        struct { f32 damage; f32 angle; } hit;                                  /* host -> client */
        struct { s16 chrnum; s16 hitpart; s32 weaponid; f32 vector[3]; } shot;  /* client -> host */
        struct { s32 doorofs; s32 state; } door;                                /* both ways */
        u8 raw[28];
    } u;
} CoopEvent;

#define COOP_EVENTS_PER_PKT 32
#define COOP_EVENT_RING     256

typedef struct
{
    u32 magic;
    u16 version;
    u16 type;
    u32 ack;        /* highest in-order seq received from the peer */
    u32 firstseq;   /* seq of ev[0] */
    u32 count;
    CoopEvent ev[COOP_EVENTS_PER_PKT];
} CoopEventsPkt;

/* host -> client guard snapshot, one entry per chr */
enum { GSF_HIDDEN = 1, GSF_FLIP = 2, GSF_LOOP = 4 };

typedef struct
{
    s16 chrnum;
    u8 flags;
    u8 pad;
    f32 pos[3];
    f32 yaw;
    s32 animoff;    /* byte offset into ptr_animation_table, -1 none */
    f32 frame;
    f32 speed;
} CoopGuardEnt;

#define COOP_GUARDS_PER_PKT 32

typedef struct
{
    u32 magic;
    u16 version;
    u16 type;
    u32 snap;       /* snapshot id; one snapshot spans several packets */
    u16 first;      /* index of the first entry in the snapshot */
    u16 count;
    CoopGuardEnt ent[COOP_GUARDS_PER_PKT];
} CoopGuardsPkt;


static s32 sEnabled = -1;
static u32 sTick;

static CoopStatePkt sRemote;
static s32 sRemoteValid;
static u32 sRemoteSeenTick;

static PropRecord *sPuppet;
static coord3d sPuppetPrev;
static s32 sPuppetAnim = -1;
static s32 sPuppetGhost;

/* host guard targeting */
enum { COOPTGT_LOCAL = 0, COOPTGT_REMOTE = 1, COOPTGT_COUNT };

typedef struct
{
    s32 lastupdate60;
    s32 laststimulus60;
    s8 target;
} CoopChrTarget;

#define COOP_MAX_CHRS 256
static CoopChrTarget sChrTarget[COOP_MAX_CHRS];
static ChrRecord *sTickChr;         /* guard whose tick has the puppet as Bond */
PropRecord *g_CoopPropOverride;     /* read by getCurrentPlayerProp() */

/* reliable events */
static CoopEvent sOut[COOP_EVENT_RING];
static u32 sOutNext = 1;    /* seq the next queued event gets */
static u32 sOutAcked;       /* peer has everything up to here */
static u32 sInDone;         /* we have applied everything up to here */
static u32 sEvLastSend;
static s32 sAckDirty;

/* client guard mirroring */
static u32 sGuardSnap;
static u32 sChrLastSync[COOP_MAX_CHRS]; /* sTick of the last snapshot entry */
static u32 sLastSnapTick;

enum { PANIM_IDLE, PANIM_WALK, PANIM_RUN };

static s32 coopInMission(void);
static void coopApplyGuard(CoopGuardEnt *e);
static void coopApplyEvent(CoopEvent *ev);
static void coopQueueEvent(CoopEvent *ev);
static void coopFlushEvents(void);
static void coopApplyDoor(s32 doorofs, s32 state);
static StandTile *coopFindStan(coord3d *pos, StandTile *from, coord3d *frompos);
static s32 coopIsMirroringClient(void);
static void coopSendGuards(void);
static void coopReapGuards(void);

static s32 coopLocalBody(void)
{
    switch (g_CurrentPlayer->bondtype)
    {
        case CUFF_BOILER: return BODY_Special_Operations_Uniform;
        case CUFF_JUNGLE: return BODY_Jungle_Fatigues;
        case CUFF_SNOW:   return BODY_Parka;
        case CUFF_BLUE:   return BODY_Formal_Wear;
        default:          return BODY_Brosnan_Tuxedo;
    }
}

static s32 coopInMission(void)
{
    PropRecord *bond = getCurrentPlayerProp();

    return bond != NULL && bond->stan != NULL && g_CameraMode == CAMERAMODE_FP;
}

static void coopSendState(void)
{
    CoopStatePkt p;
    PropRecord *bond = getCurrentPlayerProp();

    memset(&p, 0, sizeof(p));
    p.magic = COOP_MAGIC;
    p.version = COOP_VERSION;
    p.type = COOPPKT_STATE;
    p.tick = sTick;
    p.stage = bossGetStageNum();
    p.pos[0] = bond->pos.f[0];
    p.pos[1] = bond->pos.f[1];
    p.pos[2] = bond->pos.f[2];
    p.yaw = bondviewGetPlayerYawRadians();
    p.bodynum = coopLocalBody();
    p.headnum = HEAD_Male_Brosnan_Default;
    p.dead = g_CurrentPlayer->bonddead;
    coopNetSend(&p, sizeof(p));

    if ((sTick % 300) == 1)
    {
        portConLogf("ge: coop: local pos %.0f %.0f %.0f yaw %.2f\n", p.pos[0], p.pos[1], p.pos[2], p.yaw);
    }
}

static void coopPollNet(void)
{
    u8 buf[1500];
    s32 n;

    while ((n = coopNetRecv(buf, sizeof(buf))) > 0)
    {
        CoopStatePkt *p = (CoopStatePkt *) buf;

        if (n < (s32) sizeof(u32) * 2 || p->magic != COOP_MAGIC || p->version != COOP_VERSION)
        {
            continue;
        }
        if (p->type == COOPPKT_EVENTS)
        {
            CoopEventsPkt *e = (CoopEventsPkt *) buf;
            s32 hdr = (s32) ((u8 *) &e->ev[0] - (u8 *) e);
            u32 k;

            if (n < hdr || e->count > COOP_EVENTS_PER_PKT || n < hdr + (s32) (e->count * sizeof(CoopEvent)))
            {
                continue;
            }
            if ((s32) (e->ack - sOutAcked) > 0)
            {
                sOutAcked = e->ack;
            }
            for (k = 0; k < e->count; k++)
            {
                u32 seq = e->firstseq + k;

                if (seq == sInDone + 1)
                {
                    coopApplyEvent(&e->ev[k]);
                    sInDone = seq;
                }
            }
            sAckDirty = 1;
            continue;
        }
        if (p->type == COOPPKT_GUARDS && !coopNetIsHost())
        {
            CoopGuardsPkt *g = (CoopGuardsPkt *) buf;
            s32 hdr = (s32) ((u8 *) &g->ent[0] - (u8 *) g);
            s32 k;

            if (n < hdr || g->count > COOP_GUARDS_PER_PKT || n < hdr + g->count * (s32) sizeof(CoopGuardEnt))
            {
                continue;
            }
            if (coopInMission() && sRemoteValid && sRemote.stage == bossGetStageNum())
            {
                for (k = 0; k < g->count; k++)
                {
                    coopApplyGuard(&g->ent[k]);
                }
                sLastSnapTick = sTick;
            }
            continue;
        }
        if (p->type == COOPPKT_STATE && n >= (s32) sizeof(CoopStatePkt))
        {
            /* datagrams can arrive out of order; keep the newest */
            if (!sRemoteValid || (s32) (p->tick - sRemote.tick) > 0)
            {
                if (!sRemoteValid)
                {
                    portConLogf("ge: coop: first state from peer (stage %d)\n", p->stage);
                }
                sRemote = *p;
                sRemoteValid = 1;
                sRemoteSeenTick = sTick;
            }
        }
    }
}

static StandTile *coopFindStan(coord3d *pos, StandTile *from, coord3d *frompos)
{
    StandTile *tile = from;

    if (tile != NULL && walkTilesBetweenPoints_NoCallback(&tile, frompos->f[0], frompos->f[2], pos->f[0], pos->f[2]) && tile != NULL)
    {
        return tile;
    }
    return stanFindTileBelowPos(pos, NULL, NULL);
}

static void coopPuppetSetAnim(ChrRecord *chr, s32 which, f32 speed)
{
    struct ModelAnimation *anim;
    f32 rate;

    switch (which)
    {
        case PANIM_RUN:
            anim = (struct ModelAnimation *) &ptr_animation_table->data[PTR_ANIM_running];
            rate = speed / D_80030988;
            break;
        case PANIM_WALK:
            anim = (struct ModelAnimation *) &ptr_animation_table->data[PTR_ANIM_walking_unarmed];
            rate = speed / D_80030990;
            break;
        default:
            anim = (struct ModelAnimation *) &ptr_animation_table->data[PTR_ANIM_idle_unarmed];
            rate = 0.25f;
            break;
    }
    if (rate > 1.0f)
    {
        rate = 1.0f;
    }

    if (which != sPuppetAnim)
    {
        modelSetAnimation(chr->model, anim, 0, 0.0f, rate, 16.0f);
        if (which == PANIM_IDLE)
        {
            modelSetAnimLooping(chr->model, 0, 16.0f);
        }
        sPuppetAnim = which;
    }
    else
    {
        modelSetAnimSpeed(chr->model, rate, 0.0f);
    }
}

static void coopPuppetSpawn(void)
{
    coord3d pos;
    StandTile *stan;
    Model *header;
    PropRecord *prop;
    s32 head = sRemote.headnum;

    pos.f[0] = sRemote.pos[0];
    pos.f[1] = sRemote.pos[1];
    pos.f[2] = sRemote.pos[2];
    stan = stanFindTileBelowPos(&pos, NULL, NULL);
    if (stan == NULL)
    {
        return; /* peer is somewhere this stage has no floor; retry next tick */
    }

    header = retrieve_header_for_body_and_head(sRemote.bodynum, head, 0);
    if (header == NULL)
    {
        return;
    }
    prop = chrAllocate(header, &pos, sRemote.yaw, stan, (s32) ailistFindById(GAILIST_DEAD_AI));
    if (prop == NULL)
    {
        return;
    }
    chrpropActivateThisFrame(prop);
    chrpropEnable(prop);
    prop->chr->bodynum = sRemote.bodynum;
    prop->chr->headnum = head;
    prop->chr->actiontype = ACT_BONDMULTI; /* not dispatched by chrlvActionTick */
    prop->chr->sleep = 0;

    sPuppet = prop;
    sPuppetPrev = pos;
    sPuppetAnim = -1;
    sPuppetGhost = 1;
    coopPuppetSetAnim(prop->chr, PANIM_IDLE, 0.0f);
    portConLogf("ge: coop: puppet spawned at %.0f %.0f %.0f\n", pos.f[0], pos.f[1], pos.f[2]);
}

static void coopPuppetUpdate(void)
{
    ChrRecord *chr = sPuppet->chr;
    coord3d pos;
    StandTile *stan;
    f32 dx, dz, speed;

    pos.f[0] = sRemote.pos[0];
    pos.f[1] = sRemote.pos[1];
    pos.f[2] = sRemote.pos[2];

    stan = coopFindStan(&pos, sPuppet->stan, &sPuppet->pos);
    if (stan == NULL)
    {
        return;
    }

    dx = pos.f[0] - sPuppetPrev.f[0];
    dz = pos.f[2] - sPuppetPrev.f[2];
    speed = sqrtf(dx * dx + dz * dz) / (g_ClockTimer > 0 ? (f32) g_ClockTimer : 1.0f);
    sPuppetPrev = pos;

    sPuppet->stan = stan;
    sPuppet->pos = pos;
    setsuboffset(chr->model, &pos);
    setsubroty(chr->model, sRemote.yaw);
    sub_GAME_7F01FC10(chr->model, &sPuppet->pos, &sPuppet->pos, &chr->ground);
    chrDetectRooms(chr);

    if (speed > 4.0f)
    {
        coopPuppetSetAnim(chr, PANIM_RUN, speed);
    }
    else if (speed > 0.5f)
    {
        coopPuppetSetAnim(chr, PANIM_WALK, speed);
    }
    else
    {
        coopPuppetSetAnim(chr, PANIM_IDLE, 0.0f);
    }
}

/* Both Bonds start on the same pad, so the puppet stays non-solid until the
 * players have moved apart; after that they block each other like MP players. */
static void coopPuppetUpdateGhost(void)
{
    PropRecord *bond = getCurrentPlayerProp();
    f32 dx = sPuppet->pos.f[0] - bond->pos.f[0];
    f32 dz = sPuppet->pos.f[2] - bond->pos.f[2];
    f32 d2 = dx * dx + dz * dz;

    if (d2 < 60.0f * 60.0f)
    {
        sPuppetGhost = 1;
    }
    else if (d2 > 90.0f * 90.0f)
    {
        sPuppetGhost = 0;
    }
}

s32 coopPropIsGhost(PropRecord *prop)
{
    return prop != NULL && prop == sPuppet && sPuppetGhost;
}

void coopStageStart(void)
{
    s32 i;

    for (i = 0; i < COOP_MAX_CHRS; i++)
    {
        sChrTarget[i].lastupdate60 = -1;
        sChrTarget[i].laststimulus60 = -1;
        sChrTarget[i].target = -1;
    }
    for (i = 0; i < COOP_MAX_CHRS; i++)
    {
        sChrLastSync[i] = 0;
    }
    sLastSnapTick = 0;
    sTickChr = NULL;
    g_CoopPropOverride = NULL;
    sPuppet = NULL;
    sPuppetGhost = 0;
    sPuppetAnim = -1;
    sRemoteValid = 0;
}

void coopTick(void)
{
    if (sEnabled < 0)
    {
        sEnabled = coopNetInit();
    }
    if (!sEnabled || g_ClockTimer <= 0)
    {
        return;
    }

    sTick++;
    coopPollNet();
    coopFlushEvents();

    if (!coopInMission())
    {
        return;
    }

    coopSendState();

    {   /* GE_COOP_TRACE=<chrnum>: log one chr on both sides to compare */
        static s32 trace = -2;
        if (trace == -2)
        {
            const char *e = getenv("GE_COOP_TRACE");
            trace = (e != NULL) ? atoi(e) : -1;
        }
        if (trace >= 0 && (sTick % 60) == 0)
        {
            ChrRecord *c = chrFindByLiteralId(trace);
            if (c != NULL && c->prop != NULL)
            {
                portConLogf("ge: coop: trace chr %d pos %.0f %.0f %.0f act %d anim %d frame %.1f\n", trace,
                            c->prop->pos.f[0], c->prop->pos.f[1], c->prop->pos.f[2], c->actiontype,
                            (s32) ((u8 *) objecthandlerGetModelAnim(c->model) - (u8 *) ptr_animation_table), c->model->animframe1);
            }
        }
    }

    if (coopNetIsHost() && coopNetHasPeer() && sRemoteValid && sRemote.stage == bossGetStageNum() && (sTick % 3) == 0)
    {
        coopSendGuards();
    }
    if (coopIsMirroringClient())
    {
        coopReapGuards();
        {   /* GE_COOP_TESTSHOT=<chrnum>: a chest shot every 2 s (harness only) */
            static s32 testchr = -2;
            if (testchr == -2)
            {
                const char *e = getenv("GE_COOP_TESTSHOT");
                testchr = (e != NULL) ? atoi(e) : -1;
            }
            if (testchr >= 0 && (sTick % 120) == 0)
            {
                ChrRecord *c = chrFindByLiteralId(testchr);
                coord3d v = { 0.0f, 0.0f, 1.0f };
                if (c != NULL && c->prop != NULL)
                {
                    coopClientForwardShot(c, HIT_CHEST, &v, ITEM_WPPK);
                }
            }
        }
    }

    if (sRemoteValid && sRemote.stage == bossGetStageNum())
    {   /* GE_COOP_TESTDOOR=1: toggle the door nearest Bond every 10 s (harness only) */
        static s32 testdoor = -1;
        if (testdoor == -1)
        {
            testdoor = getenv("GE_COOP_TESTDOOR") != NULL;
        }
        if (testdoor && (sTick % 600) == 300)
        {
            PropRecord *bond = getCurrentPlayerProp();
            PropRecord *it, *best = NULL;
            f32 bestd = 1e30f;

            for (it = g_ActivePropsHead; it != NULL; it = it->next)
            {
                if (it->type == PROP_TYPE_DOOR && it->door != NULL)
                {
                    f32 dx = it->pos.f[0] - bond->pos.f[0];
                    f32 dz = it->pos.f[2] - bond->pos.f[2];
                    if (dx * dx + dz * dz < bestd)
                    {
                        bestd = dx * dx + dz * dz;
                        best = it;
                    }
                }
            }
            if (best != NULL)
            {
                s32 st = doorIsClosed(best->door) ? DOORSTATE_OPENING : DOORSTATE_CLOSING;
                portConLogf("ge: coop: testdoor %d -> state %d (dist %.0f)\n",
                    (s32) ((u8 *) best->door - (u8 *) g_CurrentSetup.propDefs), st, sqrtf(bestd));
                doorActivate(best->door, st);
            }
        }
    }

    if (!sRemoteValid || sRemote.stage != bossGetStageNum())
    {
        return;
    }

    if (sPuppet == NULL)
    {
        coopPuppetSpawn();
    }
    else if (sPuppet->chr == NULL)
    {
        sPuppet = NULL; /* freed under us (stage cleanup); respawn */
    }
    else
    {
        coopPuppetUpdate();
        coopPuppetUpdateGhost();
        if ((sTick % 60) == 0)
        {
            ChrRecord *c = sPuppet->chr;
            portConLogf("ge: coop: puppet pos %.0f %.0f %.0f stan %p chrflags %x hidden %x propflags %x anim %d ghost %d\n",
                        sPuppet->pos.f[0], sPuppet->pos.f[1], sPuppet->pos.f[2], (void *) sPuppet->stan,
                        c->chrflags, c->hidden, sPuppet->flags, sPuppetAnim, sPuppetGhost);
        }
    }
}


/* ---- host guard targeting ---------------------------------------------- */

static s32 coopTargetable(s32 t)
{
    if (t == COOPTGT_LOCAL)
    {
        return !g_CurrentPlayer->bonddead;
    }
    return t == COOPTGT_REMOTE && sPuppet != NULL && sPuppet->chr != NULL
        && sRemoteValid && !sRemote.dead;
}

static PropRecord *coopTargetProp(s32 t)
{
    return (t == COOPTGT_REMOTE) ? sPuppet : g_CurrentPlayer->prop;
}

static CoopChrTarget *coopChrState(ChrRecord *chr)
{
    s32 slot;

    if (g_ChrSlots == NULL || chr == NULL)
    {
        return NULL;
    }
    slot = chr - g_ChrSlots;
    if (slot < 0 || slot >= g_NumChrSlots || slot >= COOP_MAX_CHRS)
    {
        return NULL;
    }
    return &sChrTarget[slot];
}

static s32 coopNearestTarget(ChrRecord *chr)
{
    s32 best = -1;
    f32 bestd2 = 1.0e30f;
    s32 t;

    for (t = 0; t < COOPTGT_COUNT; t++)
    {
        PropRecord *p;
        f32 dx, dy, dz, d2;

        if (!coopTargetable(t))
        {
            continue;
        }
        p = coopTargetProp(t);
        dx = p->pos.f[0] - chr->prop->pos.f[0];
        dy = p->pos.f[1] - chr->prop->pos.f[1];
        dz = p->pos.f[2] - chr->prop->pos.f[2];
        d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestd2)
        {
            bestd2 = d2;
            best = t;
        }
    }
    return best;
}

/* Passive guards may rotate candidates; once a player-relative action runs,
 * changing Bond under it makes the retail AI list contradict itself. */
static s32 coopActionAllowsScan(ChrRecord *chr)
{
    switch (chr->actiontype)
    {
        case ACT_INIT:
        case ACT_STAND:
        case ACT_PATROL:
            return TRUE;
        default:
            return FALSE;
    }
}

/* Acquire -> commit -> release (from GE+): a passive guard scans players a
 * few times a second, a real stimulus commits it, and it lets go only after
 * ten seconds without one and back in a passive action. A dead target is
 * dropped at once. */
static s32 coopChrTarget(ChrRecord *chr)
{
    CoopChrTarget *st = coopChrState(chr);
    s32 next;

    if (st == NULL || !coopTargetable(COOPTGT_REMOTE))
    {
        return COOPTGT_LOCAL;
    }

    if (st->target >= 0 && coopTargetable(st->target))
    {
        if (st->laststimulus60 >= 0)
        {
            if (g_GlobalTimer - st->laststimulus60 < CHRLV_10_SEC_TIMER || !coopActionAllowsScan(chr))
            {
                return st->target;
            }
            st->laststimulus60 = -1;
            st->lastupdate60 = g_GlobalTimer;
        }
        else if (!coopActionAllowsScan(chr))
        {
            return st->target;
        }

        if (g_GlobalTimer - st->lastupdate60 < 12)
        {
            return st->target;
        }

        next = (st->target + 1) % COOPTGT_COUNT;
        if (coopTargetable(next))
        {
            st->target = next;
            st->lastupdate60 = g_GlobalTimer;
        }
        return st->target;
    }

    next = coopNearestTarget(chr);
    if (next < 0)
    {
        next = COOPTGT_LOCAL;
    }
    st->target = next;
    st->lastupdate60 = g_GlobalTimer;
    st->laststimulus60 = -1;
    return next;
}

static s32 coopIsMirroringClient(void)
{
    return sEnabled && !coopNetIsHost() && sRemoteValid && sLastSnapTick != 0
        && sRemote.stage == bossGetStageNum();
}

s32 coopChrTickBegin(ChrRecord *chr)
{
    sTickChr = NULL;
    g_CoopPropOverride = NULL;

    if (!sEnabled || chr->prop == sPuppet)
    {
        return 1;
    }
    if (!coopNetIsHost())
    {
        return !coopIsMirroringClient();
    }
    if (sPuppet == NULL)
    {
        return 1;
    }
    if (coopChrTarget(chr) == COOPTGT_REMOTE)
    {
        static ChrRecord *logged[8];
        s32 k;

        for (k = 0; k < 8 && logged[k] != NULL && logged[k] != chr; k++)
        {
        }
        if (k < 8 && logged[k] == NULL)
        {
            logged[k] = chr;
            portConLogf("ge: coop: guard %d now targets the peer\n", chr->chrnum);
        }
        sTickChr = chr;
        g_CoopPropOverride = sPuppet;
    }
    return 1;
}

void coopChrTickEnd(void)
{
    sTickChr = NULL;
    g_CoopPropOverride = NULL;
}

/* A real stimulus (seen, heard, shot by) commits the guard to whichever
 * player is "Bond" right now. Ownership doesn't ping-pong while the current
 * target is still fresh. */
void coopChrStimulus(ChrRecord *chr)
{
    CoopChrTarget *st;
    s32 t = (g_CoopPropOverride != NULL) ? COOPTGT_REMOTE : COOPTGT_LOCAL;

    if (!sEnabled || !coopNetIsHost() || chr == NULL || chr->prop == sPuppet)
    {
        return;
    }
    st = coopChrState(chr);
    if (st == NULL)
    {
        return;
    }
    if (st->target != t && st->target >= 0 && coopTargetable(st->target)
        && st->laststimulus60 >= 0 && g_GlobalTimer - st->laststimulus60 < CHRLV_SEEN_RECENT_CHECK)
    {
        return;
    }
    st->target = t;
    st->lastupdate60 = g_GlobalTimer;
    st->laststimulus60 = g_GlobalTimer;
}

s32 coopTargetIsRemote(void)
{
    return g_CoopPropOverride != NULL;
}

/* bondviewUpdateGuardTankFlagsRelated for whichever Bond the guard is on:
 * takes the target out of the line-of-sight test against itself. */
void coopGuardTankFlags(s32 flag)
{
    if (g_CoopPropOverride != NULL)
    {
        chrSetMoving(g_CoopPropOverride->chr, flag);
        return;
    }
    bondviewUpdateGuardTankFlagsRelated(g_CurrentPlayer->prop, flag);
}

f32 coopTargetDuckingHeight(void)
{
    if (g_CoopPropOverride != NULL)
    {
        return g_CoopPropOverride->chr->chrheight * 0.5f;
    }
    return bondviewGetPlayerDuckingHeightRelated(g_CurrentPlayer);
}

s32 coopTargetDamageShowTime(void)
{
    if (g_CoopPropOverride != NULL)
    {
        return 0;
    }
    return bondviewGetIfCurrentPlayerDamageShowTime();
}

s32 coopTargetIsDead(void)
{
    if (g_CoopPropOverride != NULL)
    {
        return sRemote.dead;
    }
    return g_CurrentPlayer->bonddead;
}

/* Damage a guard dealt to "Bond". Returns 1 when it belonged to the peer. */
s32 coopTargetRecordDamage(f32 damage, f32 angle)
{
    CoopEvent ev;

    if (g_CoopPropOverride == NULL)
    {
        return 0;
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_HIT;
    ev.u.hit.damage = damage;
    ev.u.hit.angle = angle;
    coopQueueEvent(&ev);
    portConLogf("ge: coop: guard %d hit the peer: %.3f\n", sTickChr ? sTickChr->chrnum : -1, damage);
    return 1;
}


/* ---- guard mirroring (host sends, client applies) ----------------------- */

static void coopSendGuards(void)
{
    CoopGuardsPkt pkt;
    s32 i;
    s32 index = 0;
    s32 hdr = (s32) ((u8 *) &pkt.ent[0] - (u8 *) &pkt);

    sGuardSnap++;
    memset(&pkt, 0, hdr);
    pkt.magic = COOP_MAGIC;
    pkt.version = COOP_VERSION;
    pkt.type = COOPPKT_GUARDS;
    pkt.snap = sGuardSnap;

    for (i = 0; i < g_NumChrSlots; i++)
    {
        ChrRecord *chr = &g_ChrSlots[i];
        CoopGuardEnt *e;
        ModelAnimation *anim;

        if (chr->model == NULL || chr->prop == NULL || chr->prop == sPuppet)
        {
            continue;
        }

        if (pkt.count == 0)
        {
            pkt.first = (u16) index;
        }
        e = &pkt.ent[pkt.count++];
        index++;

        e->chrnum = chr->chrnum;
        e->flags = 0;
        if (chr->chrflags & CHRFLAG_HIDDEN)
        {
            e->flags |= GSF_HIDDEN;
        }
        if (objecthandlerGetModelGunhand(chr->model))
        {
            e->flags |= GSF_FLIP;
        }
        if (chr->model->animlooping)
        {
            e->flags |= GSF_LOOP;
        }
        e->pos[0] = chr->prop->pos.f[0];
        e->pos[1] = chr->prop->pos.f[1];
        e->pos[2] = chr->prop->pos.f[2];
        e->yaw = getsubroty(chr->model);
        anim = objecthandlerGetModelAnim(chr->model);
        e->animoff = (anim != NULL) ? (s32) ((u8 *) anim - (u8 *) ptr_animation_table) : -1;
        e->frame = chr->model->animframe1;
        e->speed = chr->model->speed;

        if (pkt.count == COOP_GUARDS_PER_PKT)
        {
            coopNetSend(&pkt, hdr + pkt.count * (s32) sizeof(CoopGuardEnt));
            pkt.count = 0;
        }
    }
    if (pkt.count > 0)
    {
        coopNetSend(&pkt, hdr + pkt.count * (s32) sizeof(CoopGuardEnt));
    }
}

static void coopApplyGuard(CoopGuardEnt *e)
{
    ChrRecord *chr = chrFindByLiteralId(e->chrnum);
    PropRecord *prop;
    coord3d pos;
    StandTile *stan;
    s32 slot;

    if (chr == NULL || chr->prop == NULL || chr->prop == sPuppet)
    {
        return; /* TODO: chrs the host spawned that this side has not */
    }
    prop = chr->prop;
    slot = chr - g_ChrSlots;
    if (slot >= 0 && slot < COOP_MAX_CHRS)
    {
        sChrLastSync[slot] = sTick;
    }

    if (e->flags & GSF_HIDDEN)
    {
        chr->chrflags |= CHRFLAG_HIDDEN;
    }
    else
    {
        chr->chrflags &= ~CHRFLAG_HIDDEN;
    }

    pos.f[0] = e->pos[0];
    pos.f[1] = e->pos[1];
    pos.f[2] = e->pos[2];
    stan = coopFindStan(&pos, prop->stan, &prop->pos);
    if (stan != NULL)
    {
        prop->stan = stan;
        prop->pos = pos;
        setsuboffset(chr->model, &pos);
        chrDetectRooms(chr);
    }
    setsubroty(chr->model, e->yaw);
    chr->chrflags |= CHRFLAG_INIT;

    if (e->animoff >= 0)
    {
        ModelAnimation *anim = (ModelAnimation *) ((u8 *) ptr_animation_table + e->animoff);
        s32 flip = (e->flags & GSF_FLIP) != 0;

        if (objecthandlerGetModelAnim(chr->model) != anim || (objecthandlerGetModelGunhand(chr->model) != 0) != flip)
        {
            modelSetAnimation(chr->model, anim, flip, e->frame, e->speed, 8.0f);
            if (e->flags & GSF_LOOP)
            {
                modelSetAnimLooping(chr->model, 0, 8.0f);
            }
        }
        else
        {
            f32 d = chr->model->animframe1 - e->frame;

            if (d > 6.0f || d < -6.0f)
            {
                modelSetAnimation(chr->model, anim, flip, e->frame, e->speed, 0.0f);
                if (e->flags & GSF_LOOP)
                {
                    modelSetAnimLooping(chr->model, 0, 0.0f);
                }
            }
            else if (chr->model->speed != e->speed)
            {
                modelSetAnimSpeed(chr->model, e->speed, 0.0f);
            }
        }
    }
}

/* Chrs the host freed stop showing up in snapshots; free ours after 2 s. */
static void coopReapGuards(void)
{
    s32 i;

    if (sTick - sLastSnapTick > 30)
    {
        return; /* snapshots stalled: don't read silence as removal */
    }
    for (i = 0; i < g_NumChrSlots && i < COOP_MAX_CHRS; i++)
    {
        ChrRecord *chr = &g_ChrSlots[i];

        if (chr->model == NULL || chr->prop == NULL || chr->prop == sPuppet)
        {
            continue;
        }
        if (sChrLastSync[i] == 0)
        {
            sChrLastSync[i] = sTick; /* start the clock when we first see it */
        }
        else if (sTick - sChrLastSync[i] > 120 && !(chr->hidden & CHRHIDDEN_REMOVE))
        {
            portConLogf("ge: coop: chr %d gone on the host, removing\n", chr->chrnum);
            chr->hidden |= CHRHIDDEN_REMOVE;
        }
    }
}

s32 coopClientForwardShot(ChrRecord *chr, s32 hitpart, coord3d *vector, s32 weaponid)
{
    CoopEvent ev;

    if (!coopIsMirroringClient() || chr == NULL || chr->prop == sPuppet)
    {
        return 0;
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_SHOTCHR;
    ev.u.shot.chrnum = chr->chrnum;
    ev.u.shot.hitpart = (s16) hitpart;
    ev.u.shot.weaponid = weaponid;
    ev.u.shot.vector[0] = vector->f[0];
    ev.u.shot.vector[1] = vector->f[1];
    ev.u.shot.vector[2] = vector->f[2];
    coopQueueEvent(&ev);
    portConLogf("ge: coop: shot guard %d (part %d, weapon %d), sent to host\n", chr->chrnum, hitpart, weaponid);
    return 1;
}

/* Host: apply a client's shot as if the remote player fired it. */
static void coopHostApplyShot(CoopEvent *ev)
{
    ChrRecord *chr = chrFindByLiteralId(ev->u.shot.chrnum);
    coord3d vec;

    if (chr == NULL || chr->prop == NULL || sPuppet == NULL)
    {
        return;
    }
    vec.f[0] = ev->u.shot.vector[0];
    vec.f[1] = ev->u.shot.vector[1];
    vec.f[2] = ev->u.shot.vector[2];
    g_CoopPropOverride = sPuppet; /* the stimulus belongs to the peer */
    handles_shot_actors(chr, ev->u.shot.hitpart, &vec, ev->u.shot.weaponid, TRUE);
    g_CoopPropOverride = NULL;
    portConLogf("ge: coop: peer shot guard %d (part %d)\n", ev->u.shot.chrnum, ev->u.shot.hitpart);
}

/* ---- reliable events -------------------------------------------------- */

static void coopQueueEvent(CoopEvent *ev)
{
    if (sOutNext - sOutAcked > COOP_EVENT_RING)
    {
        portConLogf("ge: coop: event queue full, dropping kind %d\n", ev->kind);
        return;
    }
    sOut[sOutNext % COOP_EVENT_RING] = *ev;
    sOutNext++;
}

/* Send every unacked event at most every 4 ticks (at once when there is
 * something new or an ack owed). Repeats cover loss; seq numbers drop the
 * duplicates on the other side. */
static u32 sOutSentUpTo;

static void coopFlushEvents(void)
{
    CoopEventsPkt pkt;
    s32 hdr = (s32) ((u8 *) &pkt.ev[0] - (u8 *) &pkt);
    u32 seq;
    s32 pending = (s32) (sOutNext - 1 - sOutAcked);
    s32 fresh = (s32) (sOutNext - 1 - sOutSentUpTo) > 0;

    if (!sAckDirty && (pending <= 0 || (!fresh && sTick - sEvLastSend < 4)))
    {
        return;
    }

    memset(&pkt, 0, hdr);
    pkt.magic = COOP_MAGIC;
    pkt.version = COOP_VERSION;
    pkt.type = COOPPKT_EVENTS;
    pkt.ack = sInDone;
    pkt.firstseq = sOutAcked + 1;
    for (seq = sOutAcked + 1; seq < sOutNext && pkt.count < COOP_EVENTS_PER_PKT; seq++)
    {
        pkt.ev[pkt.count++] = sOut[seq % COOP_EVENT_RING];
    }
    coopNetSend(&pkt, hdr + (s32) (pkt.count * sizeof(CoopEvent)));
    sOutSentUpTo = seq - 1;
    sEvLastSend = sTick;
    sAckDirty = 0;
}

static void coopApplyEvent(CoopEvent *ev)
{
    switch (ev->kind)
    {
        case COOPEV_HIT:
            if (coopInMission() && !g_CurrentPlayer->bonddead)
            {
                portConLogf("ge: coop: hit from a host guard: %.3f\n", ev->u.hit.damage);
                bondviewCallRecordDamageKills(ev->u.hit.damage, ev->u.hit.angle, -1, 1);
            }
            break;
        case COOPEV_SHOTCHR:
            if (coopNetIsHost() && coopInMission())
            {
                coopHostApplyShot(ev);
            }
            break;
        case COOPEV_DOOR:
            coopApplyDoor(ev->u.door.doorofs, ev->u.door.state);
            break;
    }
}


/* ---- doors ---------------------------------------------------------------
 * Both sides run door motion; only state changes cross the wire. A door is
 * named by its byte offset in the stage's prop defs, which both sides load
 * from the same setup file. */

static s32 sApplyingRemoteDoor;

void coopDoorStateChanged(DoorRecord *door, s32 state)
{
    CoopEvent ev;

    if (!sEnabled || sApplyingRemoteDoor || !coopNetHasPeer() || !sRemoteValid
        || sRemote.stage != bossGetStageNum() || g_CurrentSetup.propDefs == NULL)
    {
        return;
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_DOOR;
    ev.u.door.doorofs = (s32) ((u8 *) door - (u8 *) g_CurrentSetup.propDefs);
    ev.u.door.state = state;
    coopQueueEvent(&ev);
}

static void coopApplyDoor(s32 doorofs, s32 state)
{
    DoorRecord *door;

    if (g_CurrentSetup.propDefs == NULL || doorofs < 0 || doorofs > 0x100000)
    {
        return;
    }
    door = (DoorRecord *) ((u8 *) g_CurrentSetup.propDefs + doorofs);
    if (door->type != PROPDEF_DOOR)
    {
        portConLogf("ge: coop: door event for offset %d is not a door here\n", doorofs);
        return;
    }
    sApplyingRemoteDoor = 1;
    doorSetOpenState(door, state);
    sApplyingRemoteDoor = 0;
    portConLogf("ge: coop: peer door %d -> state %d\n", doorofs, state);
}
