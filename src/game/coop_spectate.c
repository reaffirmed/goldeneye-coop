/**
 * Story co-op: watch a partner while we are down.
 *
 * Once our death has faded to black and co-op has taken it (coopDeadTick:
 * waiting to respawn, or for a revive, or out of lives), the screen fades
 * back in through a partner's eyes: their position and look from their
 * state, our room for the level's visibility swapped for theirs, their own
 * puppet hidden so the camera is not inside it. Fire moves to the next
 * partner who is standing, Aim to the one before. Use frees the camera: it
 * starts at the partner's eyes and flies as photo mode's does (look turns
 * it, movement flies it the way it looks, over floor and up to 4 m above
 * it); Fire or Aim go back to a partner's eyes, Use to the one we left.
 * When we are back (or the mission ends) the view is ours again; the respawn
 * fades in from black as before.
 *
 * A drop-in (coop.c coopDropInTick: joined a mission under way) watches the
 * same way while it waits for a quiet moment, alive and without the wash.
 *
 * bondview2.c asks here before it uses our camera or our room:
 *   - bondviewRenderDebugBondView: coopSpectateCamera
 *   - bondviewGetCurrentPlayersRoom / Position / Position3: coopSpectateRoom,
 *     coopSpectatePos
 * The free camera reads the mouse itself each tick (bondview2.c coopSpectateMouse).
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include "player.h"
#include "joy.h"
#include "stan.h"
#include <bondgame.h>
#include "coop.h"
#include "bondview.h"

extern void portConLogf(const char *fmt, ...);
extern f32 sinf(f32);
extern f32 cosf(f32);
extern void currentPlayerAdjustFade(f32 maxfadetime, s32 r, s32 g, s32 b, f32 frac);
extern s32 getPlayerCount(void);
extern f32 bondviewYPositionRelated(StandTile *stan, f32 x, f32 z);
extern s32 portJoyPortUseMenuPressed(s32 pad);
extern char *getenv(const char *);
extern int atoi(const char *);

#define COOPSPEC_FADE   30.0f   /* ticks back in from black */
#define FREE_SPEED      6.0f    /* units a tick at full stick */
#define FREE_PITCH      1.48f   /* about 85 degrees up or down */
#define FREE_DEADZONE   10      /* stick units of 70 that don't move it */
#define DEG             0.017453292f

static s32 sTarget = -1;        /* the partner we watch, -1: none */
static s32 sLive;              /* we came back in on a partner */
static s32 sDropIn;            /* watching while we drop in, not dead */
static coord3d sPos;            /* their eyes */
static coord3d sPos3;           /* their feet, for the portal walk */
static f32 sYaw, sPitch;
static u8 sRoom;

/* the free camera: the same yaw convention as a partner's view */
static s32 sFree;
static coord3d sFPos, sFPos3;
static StandTile *sFTile;
static f32 sFYaw, sFPitch;
static f32 sLookDx, sLookDy, sLookYaw, sLookPitch;     /* this tick's look (bondview2.c coopSpectateMouse) */
static s32 sLookDirect;

static s32 coopSpectatePick(s32 from, s32 step)
{
    CoopExtView v;
    s32 i, p;

    for (i = 1; i <= COOP_MAX_PLAYERS; i++)
    {
        p = (from + i * step + COOP_MAX_PLAYERS * 2) % COOP_MAX_PLAYERS;
        if (p != coopExtSelf() && coopExtRemoteView(p, &v) && !v.dead && v.stan != NULL)
        {
            return p;
        }
    }
    return -1;
}

/* our view this tick: where the partner is now */
static s32 coopSpectateFollow(void)
{
    CoopExtView v;

    if (sTarget < 0 || !coopExtRemoteView(sTarget, &v) || v.dead || v.stan == NULL)
    {
        return 0;
    }
    /* where their body is drawn, not their last state: its room is the one
     * the level is drawn from, and with packets lost the two parted (the
     * view went into the rock, their body walked ahead of it) */
    sPos = (v.prop != NULL) ? v.prop->pos : v.pos;
    sPos3 = sPos;
    sPos3.f[1] = bondviewYPositionRelated(v.stan, sPos.f[0], sPos.f[2]);   /* the floor under them */
    sYaw = v.yaw;
    sPitch = v.pitch;
    sRoom = v.stan->room;
    return 1;
}

static void coopSpectateFreeStart(void)
{
    CoopExtView v;

    if (sTarget < 0 || !coopExtRemoteView(sTarget, &v) || v.stan == NULL)
    {
        return;
    }
    sFree = 1;
    sFPos = sPos;
    sFPos3 = sPos3;
    sFTile = v.stan;
    sFYaw = sYaw;
    sFPitch = sPitch;
    portConLogf("ge: coop: spectate: free camera at %.0f %.0f %.0f\n", sFPos.f[0], sFPos.f[1], sFPos.f[2]);
}

/* A free camera's step, through walls, floors and ceilings (photo mode's too).
 * Nothing stops it; the floor tile under it is only followed for the room the
 * level is drawn from: along the floor while the floor goes on, else whatever
 * floor is below the new spot, else (over the void, out past the walls) the
 * last one, until it comes back over floor. *pos3 is that floor under it, for
 * the portal walk. The camera used to slide only over floor it could walk to
 * and stay 15 cm to 4 m above it, so it caught on every wall and ledge. */
void coopFreeCamStep(coord3d *pos, coord3d *pos3, StandTile **tile, const coord3d *want)
{
    StandTile *t = *tile;

    if (t == NULL || !walkTilesBetweenPoints_NoCallback(&t, pos->f[0], pos->f[2], want->f[0], want->f[2]) || t == NULL
        || want->f[1] < bondviewYPositionRelated(t, want->f[0], want->f[2]))
    {   /* off this floor, or gone down through it */
        coord3d probe = *want;

        t = stanFindTileBelowPos(&probe, NULL, NULL);
    }
    if (t != NULL)
    {
        *tile = t;
    }
    *pos = *want;
    *pos3 = *want;
    if (*tile != NULL)
    {
        pos3->f[1] = bondviewYPositionRelated(*tile, want->f[0], want->f[2]);
    }
}

/* the free camera flies the way it looks, through anything (coopFreeCamStep) */
static void coopSpectateFreeMove(f32 fwd, f32 side)
{
    const f32 cy = cosf(sFYaw), sy = sinf(sFYaw);
    const f32 cp = cosf(sFPitch), sp = sinf(sFPitch);
    coord3d want = sFPos;

    /* the view's right is (-cos, sin) of the yaw: the world's x runs to the
     * left of a camera looking along +z */
    want.f[0] += (sy * cp * fwd - cy * side) * FREE_SPEED;
    want.f[1] += sp * fwd * FREE_SPEED;
    want.f[2] += (cy * cp * fwd + sy * side) * FREE_SPEED;
    coopFreeCamStep(&sFPos, &sFPos3, &sFTile, &want);
}

/* GE_COOP_TESTFREECAM=<s>: <s> seconds into watching, free the camera, back
 * off for 1.5 s, then turn a little; GE_COOP_TESTSPECNEXT=<s>: Fire at <s>
 * seconds, Aim 4 s later (harness only) */
static void coopSpectateTest(s32 *use, s32 *step, f32 *turn, f32 *fwd, f32 *side)
{
    static s32 at = -2, nextat;
    static s32 n;

    if (at == -2)
    {
        const char *e = getenv("GE_COOP_TESTFREECAM");
        const char *f = getenv("GE_COOP_TESTSPECNEXT");

        at = (e != NULL) ? atoi(e) * 60 : -1;
        nextat = (f != NULL) ? atoi(f) * 60 : -1;
    }
    n++;
    if (n == nextat)
    {
        *step = 1;
    }
    else if (nextat > 0 && n == nextat + 240)
    {
        *step = -1;
    }
    if (at < 0)
    {
        return;
    }
    if (n == at)
    {
        *use = 1;
    }
    else if (n > at && n <= at + 90)
    {
        *fwd = -1.0f;       /* back off: the partner in front */
    }
    else if (n > at + 150 && n <= at + 210)
    {
        *turn = 0.75f;      /* degrees a tick */
    }
    else if (n > at + 240 && n <= at + 300)
    {
        *side = 1.0f;       /* strafe right */
    }
}

/* a stick axis, with a dead zone: a pad that rests a little off centre
 * flew the camera off on its own into walls (the tester's 1:02:00) */
static f32 coopSpectateStick(s32 raw)
{
    if (raw > -FREE_DEADZONE && raw < FREE_DEADZONE)
    {
        return 0.0f;
    }
    return (raw > 0 ? raw - FREE_DEADZONE : raw + FREE_DEADZONE) / (70.0f - FREE_DEADZONE);
}

static void coopSpectateFreeTick(f32 tturn, f32 tfwd, f32 tside)
{
    f32 fwd, side;

    coopSpectateMouse(&sLookDx, &sLookDy, &sLookDirect, &sLookYaw, &sLookPitch);
    if (sLookDirect)
    {
        sFYaw -= sLookYaw * DEG;
        sFPitch += sLookPitch * DEG;
    }
    else
    {
        sFYaw -= sLookDx * 2.0f * DEG;
        sFPitch += sLookDy * 2.0f * DEG;
    }
    sFYaw -= tturn * DEG;
    sLookDx = sLookDy = sLookYaw = sLookPitch = 0.0f;
    if (sFPitch > FREE_PITCH)
    {
        sFPitch = FREE_PITCH;
    }
    if (sFPitch < -FREE_PITCH)
    {
        sFPitch = -FREE_PITCH;
    }

    /* movement: the stick, or the keys on the C buttons (WASD by default) */
    fwd = coopSpectateStick(joyGetStickY(0));
    side = coopSpectateStick(joyGetStickX(0));
    if (joyGetButtons(0, U_CBUTTONS)) fwd = 1.0f;
    if (joyGetButtons(0, D_CBUTTONS)) fwd = -1.0f;
    if (joyGetButtons(0, L_CBUTTONS)) side = -1.0f;
    if (joyGetButtons(0, R_CBUTTONS)) side = 1.0f;
    fwd += tfwd;
    side += tside;
    fwd = (fwd > 1.0f) ? 1.0f : (fwd < -1.0f) ? -1.0f : fwd;
    side = (side > 1.0f) ? 1.0f : (side < -1.0f) ? -1.0f : side;
    if (fwd > 0.05f || fwd < -0.05f || side > 0.05f || side < -0.05f)
    {
        coopSpectateFreeMove(fwd, side);
    }
}

/* coopDeadTick: once a tick while we are down and co-op has the death */
void coopSpectateTick(void)
{
    s32 next, step = 0, use = 0;
    f32 tturn = 0.0f, tfwd = 0.0f, tside = 0.0f;

    if (getPlayerCount() != 1)
    {
        return;
    }
    if (!sLive)
    {   /* the death has just faded to black: come back in on a partner */
        sTarget = coopSpectatePick(COOP_MAX_PLAYERS - 1, 1);
        if (sTarget < 0)
        {
            return;
        }
        sLive = 1;
        sFree = 0;
        if (!sDropIn)
        {
            currentPlayerAdjustFade(COOPSPEC_FADE, 150, 0, 0, 0.18f);   /* a red wash: we are watching */
        }
        portConLogf("ge: coop: spectating player %d\n", sTarget + 1);
        coopRestoreMusic();     /* the death stopped the level's music; we are still in the mission */
    }
    if (joyGetButtonsPressedThisFrame(0, Z_TRIG))
    {
        step = 1;
    }
    else if (joyGetButtonsPressedThisFrame(0, R_TRIG))
    {
        step = -1;
    }
    use = portJoyPortUseMenuPressed(0) || joyGetButtonsPressedThisFrame(0, B_BUTTON);
    coopSpectateTest(&use, &step, &tturn, &tfwd, &tside);
    if (use)
    {
        if (sFree)
        {
            sFree = 0;
            portConLogf("ge: coop: spectate: back to player %d\n", sTarget + 1);
        }
        else
        {
            coopSpectateFreeStart();
        }
    }
    else if (step != 0)
    {
        next = (sFree && step > 0) ? sTarget : coopSpectatePick(sTarget, step);   /* out of the free camera: the one we left first */
        sFree = 0;
        if (next >= 0 && next != sTarget)
        {
            sTarget = next;
            portConLogf("ge: coop: spectating player %d\n", sTarget + 1);
        }
    }
    if (!coopSpectateFollow())
    {   /* they went down too, or left: the next one standing */
        next = coopSpectatePick(sTarget < 0 ? 0 : sTarget, 1);
        if (next >= 0)
        {
            sTarget = next;
            coopSpectateFollow();
        }
    }
    if (sFree)
    {
        coopSpectateFreeTick(tturn, tfwd, tside);
    }
}

/* coopDeadTick: we are back, or nobody is left standing */
void coopSpectateStop(void)
{
    sLive = 0;
    sTarget = -1;
    sFree = 0;
}

/* coopDropInTick: start (1) or end (0) watching while we drop in */
void coopSpectateDropIn(s32 on)
{
    sDropIn = on;
    coopSpectateStop();
}

/* the view is a partner's or the free camera's */
static s32 coopSpectateOn(void)
{
    if (!sLive || sTarget < 0 || g_CurrentPlayer == NULL || (!g_CurrentPlayer->bonddead && !sDropIn)
        || g_CameraMode == CAMERAMODE_POSEND || g_CameraMode == CAMERAMODE_INTRO)
    {   /* (an ending or intro has its own camera: our room swapped for the
         * partner's sent it through the hull into the void, Frigate's boat) */
        sLive = 0;
        sTarget = -1;
        sFree = 0;
        return 0;
    }
    return 1;
}

/* the partner whose eyes we look out of, -1 when the view is our own or the
 * free camera's (their puppet and tag are then drawn) */
s32 coopSpectateTarget(void)
{
    return (coopSpectateOn() && !sFree) ? sTarget : -1;
}

/* coop_hud.c: 1 watching *who, 2 the free camera (*who the one we left), 0 neither */
s32 coopSpectateHud(s32 *who)
{
    if (!coopSpectateOn())
    {
        return 0;
    }
    *who = sTarget;
    return sFree ? 2 : 1;
}

s32 coopSpectateCamera(coord3d *pos, coord3d *look, coord3d *up)
{
    f32 cp, yaw, pitch;

    if (!coopSpectateOn())
    {
        return 0;
    }
    yaw = sFree ? sFYaw : sYaw;
    pitch = sFree ? sFPitch : sPitch;
    cp = cosf(pitch);
    /* the state's yaw is the model's: the view looks along +z turned by it */
    look->f[0] = sinf(yaw) * cp;
    look->f[1] = sinf(pitch);
    look->f[2] = cosf(yaw) * cp;
    up->f[0] = 0.0f;
    up->f[1] = 1.0f;
    up->f[2] = 0.0f;
    *pos = sFree ? sFPos : sPos;
    return 1;
}

s32 coopSpectateRoom(u8 *room)
{
    if (!coopSpectateOn())
    {
        return 0;
    }
    *room = (sFree && sFTile != NULL) ? sFTile->room : sRoom;
    return 1;
}

coord3d *coopSpectatePos(s32 three)
{
    if (!coopSpectateOn())
    {
        return NULL;
    }
    if (sFree)
    {
        return three ? &sFPos3 : &sFPos;
    }
    return three ? &sPos3 : &sPos;
}
