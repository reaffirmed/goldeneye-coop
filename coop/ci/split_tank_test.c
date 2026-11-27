/**
 * split_tank.c with two players on one screen (no game, no ROM):
 *
 *   gcc -m32 -O1 -w -fms-extensions -I. -Iinclude -Iinclude/PR -Iinclude/ultra64 -Iport/include \
 *       -Isrc -Isrc/game -DNONMATCHING -DAVOID_UB -D_LANGUAGE_C -o split_tank_test coop/ci/split_tank_test.c
 *   ./split_tank_test
 *
 * Split-screen story runs every player's tick through the tank code, which
 * keeps the solo player's tank state in globals. Before split_tank.c, one
 * player climbing into Runway's tank put every view in it: the other player
 * was pulled into the hatch, steered it, and could press B to get the driver
 * out (testers' report). Here player 1 climbs in and turns the tank while
 * player 2 walks about, then player 2 tries the same tank. Exit 1 if player
 * 2 is ever "in the tank", if player 2's turn of their own state reaches
 * player 1's, if player 1 loses their seat or heading, if player 2 may climb
 * into the tank player 1 drives, or if a new stage doesn't start everyone out.
 *
 * Built with -DSPLIT_TANK_OLD it plays the same steps with no swap, as on
 * v0.9.1: most checks fail there.
 */
#include <stdio.h>
#include <string.h>
#ifdef SPLIT_TANK_OLD
#define splitTankSwap splitTankSwap_unused
#endif
#include "../../src/game/split_tank.c"
#ifdef SPLIT_TANK_OLD
#undef splitTankSwap
void splitTankSwap(s32 to) { (void) to; }
#endif

/* the globals split_tank.c swaps (bondview.c, bondview2.c) */
s32 g_PlayerIsInTank;
struct PropRecord *g_WorldTankProp;
struct PropRecord *g_PlayerTankProp;
f32 g_PlayerTankYOffset;
ALSoundState *g_TankSfxState[2];
f32 g_TankTurnSpeed;
f32 g_TankOrientationAngle;
f32 tank_turret_unused_angle;
f32 g_TankTurretVerticalAngle;
f32 g_TankTurretVerticalAngleRelated;
f32 g_TankTurretOrientationAngleRad;
f32 g_TankTurretOrientationAngleDeg;
f32 tank_turret_turn_speed;
s32 g_BondCanEnterTank;
f32 g_TankTurretAngle;
f32 g_TankTurretTurn;
s32 g_ExplodeTankOnDeathFlag;
s32 g_TankDamagePenaltyTicks;
struct coord3d g_TankModelPositionOffset;
s32 g_TankEngineSfxVolume;
s32 g_EnterTankAudioState;
f32 g_TankEnteringSitHeight;
f32 g_TankEnteringSitHeightRemain;
f32 g_TankEnterBondHorizAngleDeg;
f32 g_TankEnterBondVertAngleDeg;
struct coord3d g_EnterTankCoord;

/* the game around it */
static s32 sCur;
static s32 sSplit = 1;
s32 get_cur_playernum(void) { return sCur; }
s32 getPlayerCount(void) { return 2; }
s32 splitStoryActive(void) { return sSplit; }

/* player.c set_cur_player: the swap, then the player */
static void setPlayer(s32 p)
{
    splitTankSwap(p);
    sCur = p;
}

static struct PropRecord sTank;     /* only its address is used */
static s32 sFails, sChecks;

static void check(s32 ok, const char *what)
{
    sChecks++;
    if (!ok)
    {
        sFails++;
    }
    printf("%s  %s\n", ok ? "pass" : "FAIL", what);
}

/* bondviewLoadSetupIntroSection: the stage's fresh tank state */
static void stageLoad(void)
{
    g_PlayerIsInTank = 0;
    g_WorldTankProp = NULL;
    g_PlayerTankProp = NULL;
    g_TankOrientationAngle = 0.0f;
    g_TankTurretAngle = 0.0f;
    g_BondCanEnterTank = 0;
    splitTankReset();
}

/* bondview2.c: standing on the tank, B (as the B press checks it) */
static s32 climbIn(void)
{
    if (g_PlayerTankProp == &sTank && g_BondCanEnterTank && !splitTankTakenByOther(g_PlayerTankProp) && g_PlayerIsInTank != 1)
    {
        g_PlayerIsInTank = 1;
        g_TankOrientationAngle = 1.0f;
        return 1;
    }
    return 0;
}

int main(void)
{
    s32 tick;
    s32 p2EverIn = 0;

    setPlayer(0);
    stageLoad();

    /* player 1 walks onto the tank and climbs in */
    setPlayer(0);
    g_WorldTankProp = g_PlayerTankProp = &sTank;
    g_BondCanEnterTank = 1;
    check(climbIn(), "player 1 climbs in");

    /* a few ticks: player 1 turns the tank, player 2 walks about away from it */
    for (tick = 0; tick < 10; tick++)
    {
        setPlayer(0);
        if (g_PlayerIsInTank == 1)
        {
            g_TankOrientationAngle += 0.1f;
            g_TankTurretAngle += 0.05f;
        }
        setPlayer(1);
        p2EverIn |= (g_PlayerIsInTank == 1);
        if (g_PlayerIsInTank != 1)
        {   /* off the tank's footprint: bondviewCalcUpdatePlayerCollision lets go of it */
            g_WorldTankProp = NULL;
            g_PlayerTankProp = NULL;
            g_BondCanEnterTank = 0;
        }
        else
        {   /* "in the tank": player 2's stick steers it */
            g_TankOrientationAngle -= 0.3f;
        }
    }
    check(!p2EverIn, "player 2 is never in the tank player 1 climbed into");
    setPlayer(0);
    check(g_PlayerIsInTank == 1 && g_PlayerTankProp == &sTank, "player 1 is still in the tank after player 2's ticks");
    check(g_TankOrientationAngle > 1.95f && g_TankOrientationAngle < 2.05f, "the heading is player 1's turning alone");
    check(g_TankTurretAngle > 0.45f && g_TankTurretAngle < 0.55f, "the turret is player 1's alone");
    check(splitTankOf(0) == &sTank && splitTankOf(1) == NULL, "splitTankOf: player 1 drives, player 2 doesn't");

    /* player 2 walks onto the tank player 1 drives and presses B */
    setPlayer(1);
    g_WorldTankProp = g_PlayerTankProp = &sTank;
    g_BondCanEnterTank = 1;
    check(!climbIn(), "player 2 can't climb into the tank player 1 drives");
    check(splitTankTakenByOther(&sTank), "the tank is taken, seen from player 2");
    setPlayer(0);
    check(!splitTankTakenByOther(&sTank), "seen from player 1, nobody else has it");

    /* player 1 gets out; player 2 may climb in now */
    g_PlayerIsInTank = 0;
    setPlayer(1);
    check(climbIn(), "player 2 climbs in once player 1 is out");
    setPlayer(0);
    check(g_PlayerIsInTank == 0, "player 1 stays out while player 2 drives");

    /* a new stage: everyone starts out of any tank */
    setPlayer(1);
    stageLoad();
    setPlayer(0);
    check(g_PlayerIsInTank == 0 && splitTankOf(1) == NULL, "a new stage starts everyone out of the tank");

    /* solo: no swap, the globals are the one player's */
    sSplit = 0;
    stageLoad();
    setPlayer(0);
    g_PlayerIsInTank = 1;
    setPlayer(0);
    check(g_PlayerIsInTank == 1 && splitTankOf(0) == NULL, "solo: the globals are left alone");

    printf("%d of %d checks failed\n", sFails, sChecks);
    return sFails ? 1 : 0;
}
