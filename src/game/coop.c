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
#include "objecthandler.h"
#include "objective_status.h"

void doorSetOpenState(DoorRecord *door, s32 newstate);
bool doorIsClosed(DoorRecord *door);
s32 chrGetNumFree(void);
s32 sizepropdef(PropDefHeaderRecord *pdef);
bool objIsMortal(ObjectRecord *obj);
void alarmActivate(void);
void alarmDeactivate(void);
bool alarmIsActive(void);
s32 objGetDestroyedLevel(ObjectRecord *obj);
void objApplyDamage(ObjectRecord *obj, f32 damage, coord3d *pos, ITEM_IDS itemnum, s32 playernum);
void chrSetFiring(ChrRecord *self, s32 hand, s32 firing);
void sub_GAME_7F02BFE4(ChrRecord *self, s32 arg1, s32 arg2);
PropRecord *chrSpawnAtCoord(s32 bodynum, s32 headnum, coord3d *pos, StandTile *stan, f32 angle, AIListRecord *ailist, s32 spawnflags);

extern void portConLogf(const char *fmt, ...);

#define COOP_MAGIC   0x50434547u /* "GECP" */
#define COOP_VERSION 1

enum { COOPPKT_STATE = 1, COOPPKT_GUARDS = 3, COOPPKT_EVENTS = 5, COOPPKT_WORLD = 6 };

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

/* Mission state, both ways twice a second. Everything in it is a level
 * (not an edge), so a lost packet costs nothing. */
typedef struct
{
    u32 magic;
    u16 version;
    u16 type;
    u32 stageflags;     /* host's objectiveregisters1 (set by level scripts) */
    u32 roombits;       /* objective criteria met, one bit per list entry */
    u32 depositbits;
    u32 photobits;
    u8 keycopied;
    u8 alarm;
    u8 pad[2];
} CoopWorldPkt;

/* Reliable, in-order events (hits, shots, doors, ...). Every EVENTS packet
 * carries all unacked events plus an ack of what we have received. */
enum { COOPEV_HIT = 1, COOPEV_SHOTCHR = 2, COOPEV_DOOR = 3, COOPEV_OBJDMG = 4 };

typedef struct
{
    u8 kind;
    u8 pad[3];
    union
    {
        struct { f32 damage; f32 angle; } hit;                                  /* host -> client */
        struct { s16 chrnum; s16 hitpart; s32 weaponid; f32 vector[3]; } shot;  /* client -> host */
        struct { s32 doorofs; s32 state; } door;                                /* both ways */
        struct { s32 objofs; f32 damage; f32 pos[3]; s16 item; s16 player; } obj; /* both ways */
        u8 raw[28];
    } u;
} CoopEvent;

#define COOP_EVENTS_PER_PKT 32
#define COOP_PUPPET_CHRNUM  30000
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
enum { GSF_HIDDEN = 1, GSF_FLIP = 2, GSF_LOOP = 4, GSF_FIRE_R = 8, GSF_FIRE_L = 16 };

typedef struct
{
    s16 chrnum;
    u8 flags;
    u8 body;
    u8 head;
    s8 gunitem;     /* right hand ITEM_IDS, 0 none */
    s16 gunmodel;   /* PROP_ model of that gun */
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
static s32 sDefsBytes;  /* size of the stage's setup prop defs */
static void *sDefsBase;
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
static u8 sChrFireSeen[COOP_MAX_CHRS][2]; /* host: firecount at the last snapshot */
static u8 sChrFiring[COOP_MAX_CHRS];      /* client: GSF_FIRE_* from the last snapshot */
static u32 sLastSnapTick;

enum { PANIM_IDLE, PANIM_WALK, PANIM_RUN };

static s32 coopInMission(void);
static void coopApplyGuard(CoopGuardEnt *e);
static void coopApplyEvent(CoopEvent *ev);
static void coopQueueEvent(CoopEvent *ev);
static void coopFlushEvents(void);
static void coopApplyDoor(s32 doorofs, s32 state);
static void coopApplyObjDamage(CoopEvent *ev);
static void coopSendWorld(void);
static void coopApplyWorld(CoopWorldPkt *p);
static s32 coopSetupOfs(void *rec);
static StandTile *coopFindStan(coord3d *pos, StandTile *from, coord3d *frompos);
static s32 coopIsMirroringClient(void);
static void coopSendGuards(void);
static void coopReapGuards(void);
static void coopClientGunfire(void);

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
        if (p->type == COOPPKT_WORLD && n >= (s32) sizeof(CoopWorldPkt))
        {
            if (coopInMission() && sRemoteValid && sRemote.stage == bossGetStageNum())
            {
                coopApplyWorld((CoopWorldPkt *) buf);
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
    prop->chr->chrnum = COOP_PUPPET_CHRNUM; /* out of the way of the spawn numbers */
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
        sChrFireSeen[i][0] = sChrFireSeen[i][1] = 0;
        sChrFiring[i] = 0;
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
    if ((sTick % 30) == 0 && sRemoteValid && sRemote.stage == bossGetStageNum())
    {
        coopSendWorld();
    }
    {   /* GE_COOP_TESTWORLD=1: host raises the alarm and a stage flag at 15 s (harness only) */
        static s32 testworld = -1;
        if (testworld == -1)
        {
            testworld = getenv("GE_COOP_TESTWORLD") != NULL;
        }
        if (testworld && coopNetIsHost() && sTick >= 900)
        {
            testworld = 0;
            alarmActivate();
            objectiveregisters1 |= 0x40000000;
            portConLogf("ge: coop: testworld: alarm + flag 0x40000000\n");
        }
    }

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
        coopClientGunfire();
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

    if (coopNetIsHost() && sRemoteValid && sRemote.stage == bossGetStageNum())
    {   /* GE_COOP_TESTSPAWN=<chrnum>: once, at 15 s, spawn a copy of that chr next to Bond (harness only) */
        static s32 testspawn = -2;
        if (testspawn == -2)
        {
            const char *e = getenv("GE_COOP_TESTSPAWN");
            testspawn = (e != NULL) ? atoi(e) : -1;
        }
        if (testspawn >= 0 && sTick >= 900 && coopInMission())
        {
            static s32 done;
            if (done++) { testspawn = -1; }
        }
        if (testspawn >= 0 && sTick >= 900 && coopInMission())
        {
            ChrRecord *src = chrFindByLiteralId(testspawn);
            PropRecord *bond = getCurrentPlayerProp();
            if (src != NULL && bond != NULL)
            {
                PropRecord *np = chrSpawnAtCoord(src->bodynum, src->headnum, &src->prop->pos, src->prop->stan, 0.0f, src->ailist, 0x10);
                if (np != NULL)
                {
                    chrGiveWeapon(np->chr, PROP_CHRKALASH, ITEM_AK47, 0);
                }
                portConLogf("ge: coop: testspawn -> chr %d (free %d)\n", np ? np->chr->chrnum : -1, chrGetNumFree());
            }
        }
    }
    if (sRemoteValid && sRemote.stage == bossGetStageNum())
    {   /* GE_COOP_TESTOBJ=1: every 5 s, a heavy "shot" on the nearest breakable setup object (harness only) */
        static s32 testobj = -1;
        if (testobj == -1)
        {
            testobj = getenv("GE_COOP_TESTOBJ") != NULL;
        }
        if (testobj && (sTick % 300) == 150 && coopInMission())
        {
            PropRecord *bond = getCurrentPlayerProp();
            PropRecord *it, *best = NULL;
            f32 bestd = 1e30f;

            for (it = g_ActivePropsHead; it != NULL; it = it->next)
            {
                if (it->type == PROP_TYPE_OBJ && it->obj != NULL && coopSetupOfs(it->obj) >= 0
                    && objIsMortal(it->obj) && !objGetDestroyedLevel(it->obj))
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
                portConLogf("ge: coop: testobj %d model %d (dist %.0f)\n", coopSetupOfs(best->obj), best->obj->obj, sqrtf(bestd));
                objApplyDamage(best->obj, 3.0f, &best->pos, ITEM_AK47, 0);
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
        if (i < COOP_MAX_CHRS)
        {   /* firecount ticks on every fire tick, shot or not */
            if (chr->firecount[GUNRIGHT] != sChrFireSeen[i][GUNRIGHT])
            {
                e->flags |= GSF_FIRE_R;
            }
            if (chr->firecount[GUNLEFT] != sChrFireSeen[i][GUNLEFT])
            {
                e->flags |= GSF_FIRE_L;
            }
            sChrFireSeen[i][GUNRIGHT] = chr->firecount[GUNRIGHT];
            sChrFireSeen[i][GUNLEFT] = chr->firecount[GUNLEFT];
        }
        e->body = (u8) chr->bodynum;
        e->head = (u8) chr->headnum;
        {
            PropRecord *gun = chrGetEquippedWeaponProp(chr, GUNRIGHT);
            if (gun != NULL && gun->weapon != NULL)
            {
                e->gunitem = gun->weapon->weaponnum;
                e->gunmodel = gun->weapon->obj;
            }
        }
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

/* Client: a chr the host has and we don't (spawned mid-mission, a clone,
 * a reinforcement). Make a local stand-in with the same chrnum; the host's
 * snapshots drive it from here on. */
static s32 sMirrorSpawnsThisTick;
static u32 sMirrorSpawnTick;

static ChrRecord *coopMirrorSpawn(CoopGuardEnt *e)
{
    coord3d pos;
    StandTile *stan;
    Model *header;
    PropRecord *prop;
    ChrRecord *chr;

    if (e->flags & GSF_HIDDEN)
    {
        return NULL; /* script chrs that never show; nothing to see */
    }
    if (sMirrorSpawnTick != sTick)
    {
        sMirrorSpawnTick = sTick;
        sMirrorSpawnsThisTick = 0;
    }
    if (sMirrorSpawnsThisTick >= 4 || chrGetNumFree() < 3)
    {
        return NULL; /* try again on the next snapshot */
    }
    pos.f[0] = e->pos[0];
    pos.f[1] = e->pos[1];
    pos.f[2] = e->pos[2];
    stan = stanFindTileBelowPos(&pos, NULL, NULL);
    if (stan == NULL)
    {
        return NULL;
    }
    header = retrieve_header_for_body_and_head(e->body, e->head, 0);
    if (header == NULL)
    {
        return NULL;
    }
    prop = chrAllocate(header, &pos, e->yaw, stan, (s32) ailistFindById(GAILIST_DEAD_AI));
    if (prop == NULL)
    {
        return NULL;
    }
    sMirrorSpawnsThisTick++;
    chrpropActivateThisFrame(prop);
    chrpropEnable(prop);
    chr = prop->chr;
    chr->chrnum = e->chrnum;
    chr->bodynum = e->body;
    chr->headnum = e->head;
    if (e->gunitem > 0)
    {
        chrGiveWeapon(chr, e->gunmodel, e->gunitem, 0);
    }
    portConLogf("ge: coop: mirrored host chr %d (body %d, gun %d) at %.0f %.0f %.0f\n",
        e->chrnum, e->body, e->gunitem, pos.f[0], pos.f[1], pos.f[2]);
    return chr;
}

static void coopApplyGuard(CoopGuardEnt *e)
{
    ChrRecord *chr = chrFindByLiteralId(e->chrnum);
    PropRecord *prop;
    coord3d pos;
    StandTile *stan;
    s32 slot;

    if (chr == NULL)
    {
        chr = coopMirrorSpawn(e);
    }
    if (chr == NULL || chr->prop == NULL || chr->prop == sPuppet)
    {
        return;
    }
    prop = chr->prop;
    slot = chr - g_ChrSlots;
    if (slot >= 0 && slot < COOP_MAX_CHRS)
    {
        sChrLastSync[slot] = sTick;
        sChrFiring[slot] = e->flags & (GSF_FIRE_R | GSF_FIRE_L);
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

/* Client: muzzle flash and gun sound for guards the host says are firing.
 * Damage is the host's business; this is only what the player sees and hears. */
static void coopClientGunfire(void)
{
    s32 i, h;

    for (i = 0; i < g_NumChrSlots && i < COOP_MAX_CHRS; i++)
    {
        ChrRecord *chr = &g_ChrSlots[i];

        if (chr->model == NULL || chr->prop == NULL || chr->prop == sPuppet)
        {
            continue;
        }
        chr->hidden &= ~CHRHIDDEN_FIRE_TRACER;
        for (h = GUNRIGHT; h <= GUNLEFT; h++)
        {
            s32 firing = (sChrFiring[i] & (h == GUNRIGHT ? GSF_FIRE_R : GSF_FIRE_L)) != 0;

            if (chrGetEquippedWeaponProp(chr, h) == NULL)
            {
                continue;
            }
            chrSetFiring(chr, h, firing);
            if (firing)
            {
                if ((sTick % 60) == 0)
                {
                    portConLogf("ge: coop: chr %d firing (mirrored)\n", chr->chrnum);
                }
                sub_GAME_7F02BFE4(chr, h, 1);
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
        case COOPEV_OBJDMG:
            coopApplyObjDamage(ev);
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
    ev.u.door.doorofs = coopSetupOfs(door);
    if (ev.u.door.doorofs < 0)
    {
        return;
    }
    ev.u.door.state = state;
    coopQueueEvent(&ev);
}

static void coopApplyDoor(s32 doorofs, s32 state)
{
    DoorRecord *door;

    if (coopSetupOfs((u8 *) g_CurrentSetup.propDefs + doorofs) < 0)
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


/* Byte offset of a record in the stage's setup prop defs, or -1 for things
 * made at run time (thrown grenades, dropped guns). Both sides load the same
 * setup file, so the offset names the same object on each. */

static s32 coopSetupOfs(void *rec)
{
    u8 *base = (u8 *) g_CurrentSetup.propDefs;
    s32 ofs;

    if (base == NULL)
    {
        return -1;
    }
    if ((void *) base != sDefsBase)
    {
        PropDefHeaderRecord *it = (PropDefHeaderRecord *) base;

        while (it->type != PROPDEF_END)
        {
            it = (PropDefHeaderRecord *) ((u32 *) it + sizepropdef(it));
        }
        sDefsBase = base;
        sDefsBytes = (s32) ((u8 *) it - base);
    }
    ofs = (s32) ((u8 *) rec - base);
    return (ofs >= 0 && ofs < sDefsBytes) ? ofs : -1;
}

/* ---- objects ---------------------------------------------------------------
 * The host owns object damage. The joiner forwards its own shots on setup
 * objects to the host and drops the rest (its explosions are replays of the
 * host's); the host applies everything and sends each result back, which the
 * joiner replays with the same numbers so destruction and its explosions
 * happen on both sides. */

static s32 sApplyingRemoteObj;

s32 coopObjDamage(ObjectRecord *obj, f32 damage, coord3d *pos, s32 item, s32 player)
{
    CoopEvent ev;
    s32 ofs;

    if (sEnabled <= 0 || sApplyingRemoteObj || !coopNetHasPeer() || !sRemoteValid
        || sRemote.stage != bossGetStageNum())
    {
        return 0;
    }
    ofs = coopSetupOfs(obj);
    if (ofs < 0)
    {
        return 0;
    }
    if (!coopNetIsHost())
    {
        if (item == ITEM_UNARMED || item == 0x1D) /* 0x1D: explosion */
        {
            return 1;
        }
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_OBJDMG;
    ev.u.obj.objofs = ofs;
    ev.u.obj.damage = damage;
    ev.u.obj.pos[0] = pos->f[0];
    ev.u.obj.pos[1] = pos->f[1];
    ev.u.obj.pos[2] = pos->f[2];
    ev.u.obj.item = (s16) item;
    ev.u.obj.player = (s16) player;
    coopQueueEvent(&ev);
    return !coopNetIsHost(); /* the joiner waits for the host's verdict */
}

static void coopApplyObjDamage(CoopEvent *ev)
{
    ObjectRecord *obj;
    coord3d pos;

    if (g_CurrentSetup.propDefs == NULL || coopSetupOfs((u8 *) g_CurrentSetup.propDefs + ev->u.obj.objofs) < 0)
    {
        return;
    }
    obj = (ObjectRecord *) ((u8 *) g_CurrentSetup.propDefs + ev->u.obj.objofs);
    if (obj->prop == NULL)
    {
        return; /* already gone here */
    }
    pos.f[0] = ev->u.obj.pos[0];
    pos.f[1] = ev->u.obj.pos[1];
    pos.f[2] = ev->u.obj.pos[2];
    if (coopNetIsHost())
    {
        /* the joiner's shot: apply as ours; the hook sends the result back */
        objApplyDamage(obj, ev->u.obj.damage, &pos, ev->u.obj.item, ev->u.obj.player);
    }
    else
    {
        sApplyingRemoteObj = 1;
        objApplyDamage(obj, ev->u.obj.damage, &pos, ev->u.obj.item, ev->u.obj.player);
        sApplyingRemoteObj = 0;
    }
    portConLogf("ge: coop: peer obj %d damage %.2f (item %d)\n", ev->u.obj.objofs, ev->u.obj.damage, ev->u.obj.item);
}

/* Explosions hurt chrs on the host only; the joiner's copies of the host's
 * explosions would count twice, and neither side's blast may hurt the
 * puppet (each player's own game hurts its own Bond). */
s32 coopSkipExplosionDamage(ChrRecord *chr)
{
    if (sEnabled <= 0 || !coopNetHasPeer())
    {
        return 0;
    }
    if (chr->prop != NULL && chr->prop == sPuppet)
    {
        return 1;
    }
    return coopIsMirroringClient();
}


/* ---- objectives and alarm ------------------------------------------------
 * Level scripts run on the host only, so its stage flags are the truth. The
 * criteria either player can meet (entering a room, a deposit, a photo,
 * copying the key) are OR'd together, so the team shares one objective list. */

static void coopSendWorld(void)
{
    CoopWorldPkt p;
    struct criteria_roomentered *r;
    struct criteria_deposit *d;
    struct criteria_picture *ph;
    s32 i;

    memset(&p, 0, sizeof(p));
    p.magic = COOP_MAGIC;
    p.version = COOP_VERSION;
    p.type = COOPPKT_WORLD;
    p.stageflags = (u32) objectiveregisters1;
    for (r = ptr_last_enter_room_subobject_entry_type20, i = 0; r != NULL && i < 32; r = r->next, i++)
    {
        if (r->status)
        {
            p.roombits |= 1u << i;
        }
    }
    for (d = ptr_last_deposit_in_room_subobject_entry_type21, i = 0; d != NULL && i < 32; d = d->next, i++)
    {
        if (d->flag)
        {
            p.depositbits |= 1u << i;
        }
    }
    for (ph = ptr_last_photo_obj_in_room_subobject_entry_type1E, i = 0; ph != NULL && i < 32; ph = ph->next, i++)
    {
        if (ph->flag)
        {
            p.photobits |= 1u << i;
        }
    }
    p.keycopied = g_CurrentPlayer->copiedgoldeneye != 0;
    p.alarm = alarmIsActive() != 0;
    coopNetSend(&p, sizeof(p));
}

static void coopApplyWorld(CoopWorldPkt *p)
{
    struct criteria_roomentered *r;
    struct criteria_deposit *d;
    struct criteria_picture *ph;
    s32 i;

    if (!coopNetIsHost())
    {
        if ((u32) objectiveregisters1 != p->stageflags)
        {
            portConLogf("ge: coop: stage flags %08x -> %08x\n", (u32) objectiveregisters1, p->stageflags);
            objectiveregisters1 = (s32) p->stageflags;
        }
        if (p->alarm && !alarmIsActive())
        {
            portConLogf("ge: coop: alarm on (host)\n");
            alarmActivate();
        }
        else if (!p->alarm && alarmIsActive())
        {
            portConLogf("ge: coop: alarm off (host)\n");
            alarmDeactivate();
        }
    }
    for (r = ptr_last_enter_room_subobject_entry_type20, i = 0; r != NULL && i < 32; r = r->next, i++)
    {
        if ((p->roombits & (1u << i)) && !r->status)
        {
            portConLogf("ge: coop: peer entered objective room %d\n", i);
            r->status = 1;
        }
    }
    for (d = ptr_last_deposit_in_room_subobject_entry_type21, i = 0; d != NULL && i < 32; d = d->next, i++)
    {
        if ((p->depositbits & (1u << i)) && !d->flag)
        {
            d->flag = 1;
        }
    }
    for (ph = ptr_last_photo_obj_in_room_subobject_entry_type1E, i = 0; ph != NULL && i < 32; ph = ph->next, i++)
    {
        if ((p->photobits & (1u << i)) && !ph->flag)
        {
            ph->flag = 1;
        }
    }
    if (p->keycopied && !g_CurrentPlayer->copiedgoldeneye)
    {
        g_CurrentPlayer->copiedgoldeneye = 1;
    }
}
