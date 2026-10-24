/**
 * Story co-op: watch a partner while we are down.
 *
 * Once our death has faded to black and co-op has taken it (coopDeadTick:
 * waiting to respawn, or for a revive), the screen fades back in through a
 * partner's eyes: their position and look from their state, our room for
 * the level's visibility swapped for theirs, their own puppet hidden so the
 * camera is not inside it. Fire moves to the next partner who is standing.
 * When we are back (or the mission ends) the view is ours again; the respawn
 * fades in from black as before.
 *
 * bondview2.c asks here before it uses our camera or our room:
 *   - bondviewRenderDebugBondView: coopSpectateCamera
 *   - bondviewGetCurrentPlayersRoom / Position / Position3: coopSpectateRoom,
 *     coopSpectatePos
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include "player.h"
#include "joy.h"
#include <bondgame.h>
#include "coop.h"

extern void portConLogf(const char *fmt, ...);
extern f32 sinf(f32);
extern f32 cosf(f32);
extern void currentPlayerAdjustFade(f32 maxfadetime, s32 r, s32 g, s32 b, f32 frac);
extern s32 getPlayerCount(void);
extern f32 bondviewYPositionRelated(StandTile *stan, f32 x, f32 z);

#define COOPSPEC_FADE   30.0f   /* ticks back in from black */

static s32 sTarget = -1;        /* the partner we watch, -1: none */
static s32 sLive;              /* we came back in on a partner */
static coord3d sPos;            /* their eyes */
static coord3d sPos3;           /* their feet, for the portal walk */
static f32 sYaw, sPitch;
static u8 sRoom;

static s32 coopSpectatePick(s32 from)
{
    CoopExtView v;
    s32 i, p;

    for (i = 1; i <= COOP_MAX_PLAYERS; i++)
    {
        p = (from + i) % COOP_MAX_PLAYERS;
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
    sPos = v.pos;
    sPos3 = sPos;
    sPos3.f[1] = bondviewYPositionRelated(v.stan, sPos.f[0], sPos.f[2]);   /* the floor under them */
    sYaw = v.yaw;
    sPitch = v.pitch;
    sRoom = v.stan->room;
    return 1;
}

/* coopDeadTick: once a tick while we are down and co-op has the death */
void coopSpectateTick(void)
{
    s32 next;

    if (getPlayerCount() != 1)
    {
        return;
    }
    if (!sLive)
    {   /* the death has just faded to black: come back in on a partner */
        sTarget = coopSpectatePick(COOP_MAX_PLAYERS - 1);
        if (sTarget < 0)
        {
            return;
        }
        sLive = 1;
        currentPlayerAdjustFade(COOPSPEC_FADE, 150, 0, 0, 0.18f);   /* a red wash: we are watching */
        portConLogf("ge: coop: spectating player %d\n", sTarget + 1);
    }
    if (joyGetButtonsPressedThisFrame(0, Z_TRIG))
    {
        next = coopSpectatePick(sTarget);
        if (next >= 0 && next != sTarget)
        {
            sTarget = next;
            portConLogf("ge: coop: spectating player %d\n", sTarget + 1);
        }
    }
    if (!coopSpectateFollow())
    {   /* they went down too, or left: the next one standing */
        next = coopSpectatePick(sTarget < 0 ? 0 : sTarget);
        if (next >= 0)
        {
            sTarget = next;
            coopSpectateFollow();
        }
    }
}

/* coopDeadTick: we are back, or nobody is left standing */
void coopSpectateStop(void)
{
    sLive = 0;
    sTarget = -1;
}

/* the partner we watch, -1 when the view is our own */
s32 coopSpectateTarget(void)
{
    if (!sLive || sTarget < 0 || g_CurrentPlayer == NULL || !g_CurrentPlayer->bonddead)
    {
        sLive = 0;
        sTarget = -1;
        return -1;
    }
    return sTarget;
}

s32 coopSpectateCamera(coord3d *pos, coord3d *look, coord3d *up)
{
    f32 cp;

    if (coopSpectateTarget() < 0)
    {
        return 0;
    }
    cp = cosf(sPitch);
    /* the state's yaw is the model's: the view looks along +z turned by it */
    look->f[0] = sinf(sYaw) * cp;
    look->f[1] = sinf(sPitch);
    look->f[2] = cosf(sYaw) * cp;
    up->f[0] = 0.0f;
    up->f[1] = 1.0f;
    up->f[2] = 0.0f;
    *pos = sPos;
    return 1;
}

s32 coopSpectateRoom(u8 *room)
{
    if (coopSpectateTarget() < 0)
    {
        return 0;
    }
    *room = sRoom;
    return 1;
}

coord3d *coopSpectatePos(s32 three)
{
    if (coopSpectateTarget() < 0)
    {
        return NULL;
    }
    return three ? &sPos3 : &sPos;
}
