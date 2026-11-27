/**
 * Split-screen story: each player's own tank state.
 *
 * The tank code (bondview2.c) keeps the solo player's tank state in globals:
 * whether Bond is in the tank, which tank, its heading and speed, the
 * turret, the climb-in animation, the engine sound. Split screen runs every
 * player's tick through them in turn, so when one player climbed in, every
 * view was "in the tank": the other players were pulled into the hatch, their
 * sticks steered and turned the turret, their cameras spun with the driver's,
 * and the next player's B press got the driver out (Runway, testers' report).
 *
 * Each player on the screen keeps their own copy, swapped in by
 * set_cur_player (player.c). The tank itself is a world object and stays
 * shared, as does g_TankShellSpeed. Solo, online with one player per PC and
 * multiplayer never swap: splitStoryActive() is false there.
 *
 * coop/ci/split_tank_test.c checks it without the game.
 */
#include <ultra64.h>
#include <bondtypes.h>
#include <snd.h>
#include "player.h"

#ifndef TARGET_N64

/* bondview.c */
extern s32 g_PlayerIsInTank;
extern struct PropRecord *g_WorldTankProp;
extern struct PropRecord *g_PlayerTankProp;
extern f32 g_PlayerTankYOffset;
extern ALSoundState *g_TankSfxState[2];
extern f32 g_TankTurnSpeed;
extern f32 g_TankOrientationAngle;
extern f32 tank_turret_unused_angle;
extern f32 g_TankTurretVerticalAngle;
extern f32 g_TankTurretVerticalAngleRelated;
extern f32 g_TankTurretOrientationAngleRad;
extern f32 g_TankTurretOrientationAngleDeg;
extern f32 tank_turret_turn_speed;
extern s32 g_BondCanEnterTank;
extern f32 g_TankTurretAngle;
extern f32 g_TankTurretTurn;
extern s32 g_ExplodeTankOnDeathFlag;
extern s32 g_TankDamagePenaltyTicks;
/* bondview2.c */
extern struct coord3d g_TankModelPositionOffset;
extern s32 g_TankEngineSfxVolume;
extern s32 g_EnterTankAudioState;
extern f32 g_TankEnteringSitHeight;
extern f32 g_TankEnteringSitHeightRemain;
extern f32 g_TankEnterBondHorizAngleDeg;
extern f32 g_TankEnterBondVertAngleDeg;
extern struct coord3d g_EnterTankCoord;

#define SPLITTANK_PLAYERS 4

typedef struct
{
    s32 isInTank;
    struct PropRecord *worldTank;
    struct PropRecord *playerTank;
    f32 yOffset;
    ALSoundState *sfx[2];
    f32 turnSpeed;
    f32 orient;
    f32 unusedAngle;
    f32 turretVert;
    f32 turretVertRel;
    f32 turretRad;
    f32 turretDeg;
    f32 turretTurnSpeed;
    s32 canEnter;
    f32 turretAngle;
    f32 turretTurn;
    s32 explodeOnDeath;
    s32 damagePenalty;
    struct coord3d modelOffset;
    s32 engineVolume;
    s32 enterAudio;
    f32 sitHeight;
    f32 sitRemain;
    f32 enterHoriz;
    f32 enterVert;
    struct coord3d enterCoord;
} SplitTankState;

static SplitTankState sSplitTank[SPLITTANK_PLAYERS];
static s32 sSplitTankOwner = -1;   /* whose state the globals hold; -1: the globals are everyone's */

static void splitTankSave(SplitTankState *t)
{
    t->isInTank = g_PlayerIsInTank;
    t->worldTank = g_WorldTankProp;
    t->playerTank = g_PlayerTankProp;
    t->yOffset = g_PlayerTankYOffset;
    t->sfx[0] = g_TankSfxState[0];
    t->sfx[1] = g_TankSfxState[1];
    t->turnSpeed = g_TankTurnSpeed;
    t->orient = g_TankOrientationAngle;
    t->unusedAngle = tank_turret_unused_angle;
    t->turretVert = g_TankTurretVerticalAngle;
    t->turretVertRel = g_TankTurretVerticalAngleRelated;
    t->turretRad = g_TankTurretOrientationAngleRad;
    t->turretDeg = g_TankTurretOrientationAngleDeg;
    t->turretTurnSpeed = tank_turret_turn_speed;
    t->canEnter = g_BondCanEnterTank;
    t->turretAngle = g_TankTurretAngle;
    t->turretTurn = g_TankTurretTurn;
    t->explodeOnDeath = g_ExplodeTankOnDeathFlag;
    t->damagePenalty = g_TankDamagePenaltyTicks;
    t->modelOffset = g_TankModelPositionOffset;
    t->engineVolume = g_TankEngineSfxVolume;
    t->enterAudio = g_EnterTankAudioState;
    t->sitHeight = g_TankEnteringSitHeight;
    t->sitRemain = g_TankEnteringSitHeightRemain;
    t->enterHoriz = g_TankEnterBondHorizAngleDeg;
    t->enterVert = g_TankEnterBondVertAngleDeg;
    t->enterCoord = g_EnterTankCoord;
}

static void splitTankLoad(const SplitTankState *t)
{
    g_PlayerIsInTank = t->isInTank;
    g_WorldTankProp = t->worldTank;
    g_PlayerTankProp = t->playerTank;
    g_PlayerTankYOffset = t->yOffset;
    g_TankSfxState[0] = t->sfx[0];
    g_TankSfxState[1] = t->sfx[1];
    g_TankTurnSpeed = t->turnSpeed;
    g_TankOrientationAngle = t->orient;
    tank_turret_unused_angle = t->unusedAngle;
    g_TankTurretVerticalAngle = t->turretVert;
    g_TankTurretVerticalAngleRelated = t->turretVertRel;
    g_TankTurretOrientationAngleRad = t->turretRad;
    g_TankTurretOrientationAngleDeg = t->turretDeg;
    tank_turret_turn_speed = t->turretTurnSpeed;
    g_BondCanEnterTank = t->canEnter;
    g_TankTurretAngle = t->turretAngle;
    g_TankTurretTurn = t->turretTurn;
    g_ExplodeTankOnDeathFlag = t->explodeOnDeath;
    g_TankDamagePenaltyTicks = t->damagePenalty;
    g_TankModelPositionOffset = t->modelOffset;
    g_TankEngineSfxVolume = t->engineVolume;
    g_EnterTankAudioState = t->enterAudio;
    g_TankEnteringSitHeight = t->sitHeight;
    g_TankEnteringSitHeightRemain = t->sitRemain;
    g_TankEnterBondHorizAngleDeg = t->enterHoriz;
    g_TankEnterBondVertAngleDeg = t->enterVert;
    g_EnterTankCoord = t->enterCoord;
}

/* stage load (bondview_r.c, after it resets the tank globals): the next swap
 * hands that fresh state to every player */
void splitTankReset(void)
{
    sSplitTankOwner = -1;
}

/* player.c set_cur_player: the tank globals become player `to`'s */
void splitTankSwap(s32 to)
{
    s32 i;

    if (!splitStoryActive() || to < 0 || to >= SPLITTANK_PLAYERS || to == sSplitTankOwner)
    {
        return;
    }
    if (sSplitTankOwner < 0)
    {
        for (i = 0; i < SPLITTANK_PLAYERS; i++)
        {
            splitTankSave(&sSplitTank[i]);
        }
    }
    else
    {
        splitTankSave(&sSplitTank[sSplitTankOwner]);
    }
    splitTankLoad(&sSplitTank[to]);
    sSplitTankOwner = to;
}

/* the tank local player p drives in split-screen story, NULL none */
struct PropRecord *splitTankOf(s32 p)
{
    if (!splitStoryActive() || p < 0 || p >= getPlayerCount() || p >= SPLITTANK_PLAYERS)
    {
        return NULL;
    }
    if (p == sSplitTankOwner || sSplitTankOwner < 0)
    {
        return (g_PlayerIsInTank == 1) ? g_PlayerTankProp : NULL;
    }
    return (sSplitTank[p].isInTank == 1) ? sSplitTank[p].playerTank : NULL;
}

/* another player on this screen is driving this tank (bondview2.c: then we
 * can't climb in) */
s32 splitTankTakenByOther(struct PropRecord *prop)
{
    s32 p;

    for (p = 0; prop != NULL && p < getPlayerCount(); p++)
    {
        if (p != get_cur_playernum() && splitTankOf(p) == prop)
        {
            return 1;
        }
    }
    return 0;
}

#endif
