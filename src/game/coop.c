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

extern void portConLogf(const char *fmt, ...);

#define COOP_MAGIC   0x50434547u /* "GECP" */
#define COOP_VERSION 1

enum { COOPPKT_STATE = 1, COOPPKT_HIT = 2 };

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

typedef struct
{
    u32 magic;
    u16 version;
    u16 type;
    u32 seq;
    f32 damage;
    f32 angle;
} CoopHitPkt;

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
static u32 sHitSeq;

enum { PANIM_IDLE, PANIM_WALK, PANIM_RUN };

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
    u8 buf[512];
    s32 n;

    while ((n = coopNetRecv(buf, sizeof(buf))) > 0)
    {
        CoopStatePkt *p = (CoopStatePkt *) buf;

        if (n < (s32) sizeof(u32) * 2 || p->magic != COOP_MAGIC || p->version != COOP_VERSION)
        {
            continue;
        }
        if (p->type == COOPPKT_HIT && n >= (s32) sizeof(CoopHitPkt))
        {
            CoopHitPkt *h = (CoopHitPkt *) buf;

            if (coopInMission() && !g_CurrentPlayer->bonddead)
            {
                portConLogf("ge: coop: hit from host guard #%u: %.3f\n", h->seq, h->damage);
                bondviewCallRecordDamageKills(h->damage, h->angle, -1, 1);
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

    if (!coopInMission())
    {
        return;
    }

    coopSendState();

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

void coopChrTickBegin(ChrRecord *chr)
{
    sTickChr = NULL;
    g_CoopPropOverride = NULL;

    if (!sEnabled || !coopNetIsHost() || sPuppet == NULL || chr->prop == sPuppet)
    {
        return;
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
    CoopHitPkt h;

    if (g_CoopPropOverride == NULL)
    {
        return 0;
    }
    memset(&h, 0, sizeof(h));
    h.magic = COOP_MAGIC;
    h.version = COOP_VERSION;
    h.type = COOPPKT_HIT;
    h.seq = ++sHitSeq;
    h.damage = damage;
    h.angle = angle;
    coopNetSend(&h, sizeof(h));
    portConLogf("ge: coop: guard %d hit the peer #%u: %.3f\n", sTickChr ? sTickChr->chrnum : -1, h.seq, damage);
    return 1;
}
