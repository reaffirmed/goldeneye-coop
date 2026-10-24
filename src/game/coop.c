/**
 * Story co-op: the other player's Bond as a puppet character.
 *
 * Each field tick this sends the local Bond's position and facing to the peer,
 * and moves a puppet chr to the peer's last reported state. The puppet runs the
 * dead AI list and an action type the tick dispatcher ignores, so only this file
 * moves it. Guard targeting and world sync build on this later.
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

enum { COOPPKT_STATE = 1 };

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

static s32 sEnabled = -1;
static u32 sTick;

static CoopStatePkt sRemote;
static s32 sRemoteValid;
static u32 sRemoteSeenTick;

static PropRecord *sPuppet;
static coord3d sPuppetPrev;
static s32 sPuppetAnim = -1;
static s32 sPuppetGhost;

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
