/**
 * Story co-op: the Runway tank, seen by everyone.
 *
 * Any player can climb into the tank, but the game drives it from that
 * player's own Bond: the tank's position, its heading and the turret live
 * only in the driver's game, so the others saw it parked where it started.
 *
 *   - the driver sends COOPPKT_TANK every other tick while in the tank, and
 *     a few with driving 0 after getting out; the host passes a joiner's on
 *     to the other joiners
 *   - everyone else moves their copy of the tank to match (the same steps
 *     bondview2.c takes for the driver: matrix, rooms, collision) and hides
 *     the driver's puppet, which would stick out of the hatch
 *   - a tank a partner is driving can't be entered (bondview2.c)
 *
 * The tank's shells are the driver's own shots and explosions, which co-op
 * already shares.
 */
#include <ultra64.h>
#include <string.h>
#include <stdlib.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include <boss.h>
#include "player.h"
#include "bondview.h"
#include "matrixmath.h"
#include "loadobjectmodel.h"
#include "propobj.h"
#include "stan.h"
#include "chrai.h"
#include "coop.h"

extern void portConLogf(const char *fmt, ...);
extern void coopTeleportPlayerTo(coord3d *pos, StandTile *stan, f32 yaw);

#define COOPTANK_STALE 120     /* ticks without a packet: they are out */

typedef struct
{
    u32 magic;
    u16 version;
    u16 type;
    s32 stage;
    s32 tankofs;    /* the tank's setup offset */
    u8 player;
    u8 driving;
    u8 firing;
    u8 pad;
    f32 pos[3];
    f32 orient;
    f32 turret;
    f32 turretv;
} CoopTankPkt;

static u32 sNow;
static s32 sOfs[COOP_MAX_PLAYERS] = { -1, -1, -1, -1 };    /* the tank each player drives, -1 none */
static u32 sSeen[COOP_MAX_PLAYERS];
static s32 sMyOfs = -1;     /* the tank we drove, for the packets after we got out */
static s32 sOutFor;
static s32 sTestAt = -2;    /* GE_COOP_TESTTANK */

s32 coopTankDriving(s32 player)
{
    return player >= 0 && player < COOP_MAX_PLAYERS && player != coopExtSelf() && sOfs[player] >= 0
        && sNow - sSeen[player] < COOPTANK_STALE;
}

s32 coopTankTaken(PropRecord *prop)
{
    s32 p;
    s32 ofs;

    if (prop == NULL || prop->type != PROP_TYPE_OBJ || (ofs = coopExtSetupOfs(prop->obj)) < 0)
    {
        return 0;
    }
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        if (coopTankDriving(p) && sOfs[p] == ofs)
        {
            return 1;
        }
    }
    return 0;
}

static void coopTankSend(ObjectRecord *obj, s32 driving)
{
    TankRecord *tank = (TankRecord *) obj;
    CoopTankPkt p;

    memset(&p, 0, sizeof(p));
    p.type = COOPPKT_TANK;
    p.stage = bossGetStageNum();
    p.tankofs = coopExtSetupOfs(obj);
    p.player = (u8) coopExtSelf();
    p.driving = (u8) driving;
    p.firing = (u8) (driving && tank->is_firing_tank);
    p.pos[0] = obj->prop->pos.f[0];
    p.pos[1] = obj->prop->pos.f[1];
    p.pos[2] = obj->prop->pos.f[2];
    p.orient = tank->tank_orientation_angle;
    p.turret = tank->turret_orientation_angle;
    p.turretv = tank->turret_vertical_angle;
    coopExtSendPacket(&p, sizeof(p), 0xFF /* COOP_ALL */);
}

void coopTankPacket(const void *raw, s32 len, s32 from)
{
    CoopTankPkt p;
    ObjectRecord *obj;
    TankRecord *tank;
    PropRecord *prop;
    s32 player;
    s32 q;

    if (len < (s32) sizeof(p) || !coopExtMissionTicks())
    {
        return;
    }
    memcpy(&p, raw, sizeof(p));
    player = (from == 0) ? p.player : from;     /* the host passes joiners' on */
    if (player < 0 || player >= COOP_MAX_PLAYERS || player == coopExtSelf() || p.stage != bossGetStageNum()
        || p.tankofs < 0 || g_CurrentSetup.propDefs == NULL)
    {
        return;
    }
    if (coopExtIsHost())
    {
        p.player = (u8) player;
        for (q = 1; q < COOP_MAX_PLAYERS; q++)
        {
            if (q != player)
            {
                coopExtSendPacket(&p, sizeof(p), q);
            }
        }
    }
    obj = (ObjectRecord *) ((u8 *) g_CurrentSetup.propDefs + p.tankofs);
    prop = obj->prop;
    if (obj->type != PROPDEF_TANK || prop == NULL || obj->model == NULL)
    {
        return;
    }
    if (p.driving && sOfs[player] != p.tankofs)
    {
        portConLogf("ge: coop: player %d is driving tank %d\n", player + 1, p.tankofs);
    }
    else if (!p.driving && sOfs[player] >= 0)
    {
        portConLogf("ge: coop: player %d is out of tank %d\n", player + 1, p.tankofs);
    }
    sOfs[player] = p.driving ? p.tankofs : -1;
    sSeen[player] = sNow;
    if (g_PlayerIsInTank == 1 && g_PlayerTankProp == prop)
    {
        return;     /* we got in at the same moment: ours wins here */
    }
    tank = (TankRecord *) obj;
    tank->is_firing_tank = p.firing;
    tank->tank_orientation_angle = p.orient;
    tank->turret_orientation_angle = p.turret;
    tank->turret_vertical_angle = p.turretv;
    matrix_4x4_set_rotation_around_y(M_TAU_F - p.orient, &obj->mtx);
    matrix_scalar_multiply(obj->model->scale, (f32 *) &obj->mtx);
    if (prop->stan != NULL)
    {
        walkTilesBetweenPoints_NoCallback(&prop->stan, prop->pos.f[0], prop->pos.f[2], p.pos[0], p.pos[2]);
    }
    obj->runtime_pos.f[0] = prop->pos.f[0] = p.pos[0];
    obj->runtime_pos.f[1] = prop->pos.f[1] = p.pos[1];
    obj->runtime_pos.f[2] = prop->pos.f[2] = p.pos[2];
    setupUpdateObjectRoomPosition(obj);
    chrobjCollisionRelated(obj);
}

/* GE_COOP_TESTTANK=<s>: <s> seconds into the mission stand on the tank, get
 * in, and turn it for 10 s (harness only) */
s32 coopTankTestTap(void)
{
    const s32 t = (s32) coopExtMissionTicks();

    return sTestAt >= 0 && (t == sTestAt + 30 || t == sTestAt + 30 + 660);
}

static void coopTankTestTick(void)
{
    const s32 t = (s32) coopExtMissionTicks();
    PropRecord *it;
    coord3d to;
    StandTile *stan;

    if (sTestAt == -2)
    {
        const char *e = getenv("GE_COOP_TESTTANK");

        sTestAt = (e != NULL) ? atoi(e) * 60 : -1;
    }
    if (sTestAt < 0)
    {
        return;
    }
    if (t == sTestAt)
    {
        for (it = g_ActivePropsHead; it != NULL; it = it->next)
        {
            if (it->type == PROP_TYPE_OBJ && it->obj != NULL && it->obj->type == PROPDEF_TANK)
            {
                break;
            }
        }
        if (it == NULL)
        {
            portConLogf("ge: coop: testtank: no tank\n");
            sTestAt = -1;
            return;
        }
        to = it->pos;
        to.f[1] += 100.0f;
        stan = it->stan != NULL ? it->stan : stanFindTileBelowPos(&to, NULL, NULL);
        coopTeleportPlayerTo(&to, stan, 0.0f);
        g_WorldTankProp = it;   /* as walking into it does */
        portConLogf("ge: coop: testtank: on the tank at %.0f %.0f %.0f\n", to.f[0], to.f[1], to.f[2]);
    }
    if (g_PlayerIsInTank == 1 && t > sTestAt + 30 && t < sTestAt + 30 + 600)
    {
        g_TankOrientationAngle += 0.01f;
        if (g_TankOrientationAngle >= M_TAU_F)
        {
            g_TankOrientationAngle -= M_TAU_F;
        }
    }
}

void coopTankTick(s32 inmission)
{
    sNow++;
    if (!inmission || coopExtMissionTicks() <= 1)
    {
        memset(sOfs, 0xFF, sizeof(sOfs));
        sMyOfs = -1;
        sOutFor = 0;
        return;
    }
    coopTankTestTick();
    if (!coopLobbyHasPartner())
    {
        return;
    }
    if (g_PlayerIsInTank == 1 && g_PlayerTankProp != NULL && g_PlayerTankProp->type == PROP_TYPE_OBJ)
    {
        if (sMyOfs < 0)
        {
            portConLogf("ge: coop: we are driving tank %d\n", coopExtSetupOfs(g_PlayerTankProp->obj));
        }
        sMyOfs = coopExtSetupOfs(g_PlayerTankProp->obj);
        sOutFor = 0;
        if ((sNow & 1) == 0)
        {
            coopTankSend(g_PlayerTankProp->obj, 1);
        }
    }
    else if (sMyOfs >= 0)
    {   /* out: a few to say so, unreliable as they are */
        if (sOutFor++ % 10 == 0)
        {
            coopTankSend((ObjectRecord *) ((u8 *) g_CurrentSetup.propDefs + sMyOfs), 0);
        }
        if (sOutFor > 60)
        {
            sMyOfs = -1;
        }
    }
}
