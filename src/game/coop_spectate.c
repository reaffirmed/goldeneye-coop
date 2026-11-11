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
 * Through a partner's eyes the view is theirs as they see it: their gun in
 * first person, drawn by our own hands, which are idle while we are down.
 * Each tick (gunfire.c gunTickGameplay asks coopSpectateHandsTick) the hands
 * are asked for the guns in the partner's STATE (CoopLook.weapon: switches
 * play the draw), their trigger is held while the partner's shots come in
 * (CoopLook.shotseq: an automatic holds it, any other gun is pressed once a
 * shot; a throw or a stab from their muzzle flash), a reload cue
 * (CoopLook.cue) plays the reload, and each tracer runs to where their shot
 * stopped. Their gun points where they aim and their sight is up while they
 * aim (coopSpectateHandsAim, from CoopLook.aimpitch / aimyaw and
 * CoopStatePkt.viewaim: the view's own pitch, so the camera no longer tips
 * with the crosshair). It is only the look of it: no bullet, throw, noise,
 * sound or ammo of ours goes with it (coopSpectateHandsOn at each place),
 * and our STATE says our hands are empty. The free camera has no hands, and
 * neither does a drop-in's watch (it stands with its own gun in them).
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
#include "bg.h"
#include <bondgame.h>
#include "coop.h"
#include "bondview.h"
#include "gun.h"

extern void portConLogf(const char *fmt, ...);
extern f32 sinf(f32);
extern f32 cosf(f32);
extern void currentPlayerAdjustFade(f32 maxfadetime, s32 r, s32 g, s32 b, f32 frac);
extern s32 getPlayerCount(void);
extern f32 bondviewYPositionRelated(StandTile *stan, f32 x, f32 z);
extern s32 portJoyPortUseMenuPressed(s32 pad);
extern char *getenv(const char *);
extern int atoi(const char *);
extern s32 portReturnStartThisTick(void); /* video.c: START came from Return alone; weak 0 headless */
extern f32 g_GlobalTimerDelta;   /* lv.c: 60 Hz frames this tick */
extern void sub_GAME_7F067AB4(coord3d *aim);    /* gunfire.c: the gun's aim point, in the camera's frame */
extern s32 get_ammo_type_for_weapon(s32 weapon);

#define COOPSPEC_FADE   30.0f   /* ticks back in from black */
#define FREE_SPEED      6.0f    /* units a tick at full stick */
#define FREE_PITCH      1.48f   /* about 85 degrees up or down */
#define FREE_DEADZONE   10      /* stick units of 70 that don't move it */
#define DEG             0.017453292f
#define COOPSPEC_ENDASK 240     /* ticks to press Start a second time */

static s32 sTarget = -1;        /* the partner we watch, -1: none */
static s32 sLive;              /* we came back in on a partner */
static s32 sDropIn;            /* watching while we drop in, not dead */
static coord3d sPos;            /* their eyes */
static coord3d sPos3;           /* their feet, for the portal walk */
static f32 sYaw, sPitch;
static u8 sRoom;
static s32 sEndAsk;            /* ticks left to press Start again and end the mission */

/* the free camera: the same yaw convention as a partner's view */
static s32 sFree;
static coord3d sFPos, sFPos3;
static s32 sFRoom;
static f32 sFYaw, sFPitch;
static f32 sLookDx, sLookDy, sLookYaw, sLookPitch;     /* this tick's look (bondview2.c coopSpectateMouse) */
static s32 sLookDirect;

/* the watched partner's gun in our hands */
#define HANDS_HOLD      4.0f    /* frames (60 Hz) an automatic's trigger stays down after its last shot came in */
#define HANDS_PRESS     2.0f    /* frames a semi-automatic's press lasts, and the gap after it */
#define HANDS_PULSES    3       /* a semi-automatic's presses still to show, at most */
#define HANDS_AIM_MAX   0.7f    /* radians: the crosshair's offset at most (a stale or bad one) */
static s32 sHands;              /* our hands play sHandsFor's gun */
static s32 sHandsFor = -1;
static u8 sHandsSeq;            /* their shotseq we have shown */
static u8 sHandsCue;            /* and their cue */
static s32 sHandsWant[2];       /* the guns we asked our hands for */
static f32 sHandsHold[2];       /* frames left with the trigger down */
static s32 sHandsPulse[2];      /* presses still to show */
static f32 sHandsPress[2];      /* frames left of this press (> 0), or of the gap after it (< 0) */
static u8 sHandsFlash;          /* their muzzle flash bits last tick */
static s32 sHandsAiming;        /* they hold aim */
static s32 sHandsShots;         /* shown since we came in on them, for the log */

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
    sPitch = v.viewpitch;   /* the view's own pitch: aiming moves their sight, not their head */
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
    coopFreeCamStart(&sFPos, &sFPos3, &sFRoom, v.stan->room);
    sFYaw = sYaw;
    sFPitch = sPitch;
    portConLogf("ge: coop: spectate: free camera at %.0f %.0f %.0f\n", sFPos.f[0], sFPos.f[1], sFPos.f[2]);
}

/* A free camera's step, through walls, floors and ceilings (photo mode's too).
 * Nothing stops it. *room is the room it is in, which the level is drawn
 * from: over a floor, the floor's room walked up through the portals to the
 * camera (as a player's own view walks from the feet to the eyes); over the
 * void (out past the walls, under the level), the room it was in walked along
 * the step through any portal it crossed. *pos3 is the camera itself, so the
 * draw's own walk from the feet adds nothing.
 * It used to follow the floor tile it had and report that tile's room: flown
 * up a storey, the room stayed the one below, and out past the walls it stayed
 * the last room it left however far it flew. Seen from the wrong room the
 * portals showed nothing, so after a while the level stopped drawing until the
 * camera went back to a player (the testers' report). */
void coopFreeCamStep(coord3d *pos, coord3d *pos3, s32 *room, const coord3d *want)
{
    coord3d probe = *want;
    coord3d from = *pos;
    StandTile *t = stanFindTileBelowPos(&probe, NULL, NULL);

    *pos = *want;
    *pos3 = *want;
    if (t != NULL)
    {
        coord3d floor = *want;

        floor.f[1] = bondviewYPositionRelated(t, want->f[0], want->f[2]);
        *room = bgRoomAlongSegment(t->room, &floor, pos);
    }
    else
    {
        *room = bgRoomAlongSegment(*room, &from, pos);
    }
}

/* where a free camera starts: at a view whose feet are on a floor in room */
void coopFreeCamStart(coord3d *pos, coord3d *pos3, s32 *room, s32 feetroom)
{
    *room = bgRoomAlongSegment(feetroom, pos3, pos);
    *pos3 = *pos;
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
    coopFreeCamStep(&sFPos, &sFPos3, &sFRoom, &want);
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
    if (sEndAsk > 0)
    {
        sEndAsk--;
    }
    {   /* GE_COOP_TESTDOWNEND=<s>: press Start <s> seconds into watching, and again 1 s later (harness only) */
        static s32 endat = -2, n;

        if (endat == -2)
        {
            const char *e = getenv("GE_COOP_TESTDOWNEND");
            endat = (e != NULL) ? atoi(e) * 60 : -1;
        }
        n++;
        if (endat > 0 && (n == endat || n == endat + 60))
        {
            portJoyInjectPressed(0, START_BUTTON);
        }
    }
    if (!sDropIn && joyGetButtonsPressedThisFrame(0, START_BUTTON) && !portReturnStartThisTick())
    {   /* down, the watch can't open: Start, then Start again, is its Abort */
        if (sEndAsk > 0)
        {
            sEndAsk = 0;
            portConLogf("ge: coop: spectate: ending the mission\n");
            coopDownEndMission();
            return;
        }
        sEndAsk = COOPSPEC_ENDASK;
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

static void coopSpectateHandsEnd(void);

/* coopDeadTick: we are back, or nobody is left standing */
void coopSpectateStop(void)
{
    sEndAsk = 0;
    sLive = 0;
    sTarget = -1;
    sFree = 0;
    coopSpectateHandsEnd();
}

/* coop.c coopStageStart: a new mission sets up our hands itself */
void coopSpectateStageStart(void)
{
    sHands = 0;
    sHandsFor = -1;
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

/* coop_hud.c: 2 Start was pressed once and a second press ends the mission,
 * 1 Start can end it, 0 not (dropping in) */
s32 coopSpectateEndAsked(void)
{
    if (sDropIn || !coopSpectateOn())
    {
        return 0;
    }
    return (sEndAsk > 0) ? 2 : 1;
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
    *room = sFree ? (u8) sFRoom : sRoom;
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

/* ---- the partner's gun in our hands -------------------------------------- */

/* the view's pitch and the aim button in CoopStatePkt.viewaim: bits 0-14 the
 * pitch in 1/10000 radian from -1.6383, bit 15 aiming; 0 (an older build's
 * padding) is none */
u16 coopViewAimPack(f32 pitch, s32 aiming)
{
    s32 q;

    if (!(pitch == pitch))
    {
        pitch = 0.0f;
    }
    q = (s32) (pitch * 10000.0f + (pitch < 0.0f ? -0.5f : 0.5f)) + 16384;
    q = (q < 1) ? 1 : (q > 32767) ? 32767 : q;
    return (u16) (q | (aiming ? 0x8000 : 0));
}

s32 coopViewAimUnpack(u16 v, f32 *pitch, s32 *aiming)
{
    if ((v & 0x7FFF) == 0)
    {
        return 0;
    }
    *pitch = (f32) ((s32) (v & 0x7FFF) - 16384) / 10000.0f;
    *aiming = (v & 0x8000) != 0;
    return 1;
}

s32 coopSpectateHandsOn(void)
{
    return sHands;
}

/* the guns our hands can play: a weapon, not a gadget or a mission item
 * (using one of those does things: a door decoder opens doors) */
static s32 coopSpectateHandsItem(s32 item)
{
    if (item >= ITEM_FIST && item <= ITEM_REMOTEMINE && item != ITEM_WATCHLASER)
    {
        return item;
    }
    return ITEM_UNARMED;
}

/* a gun whose shots are bullets (a CoopShot each); the rest show from the flash */
static s32 coopSpectateHandsBullets(s32 item)
{
    return item >= ITEM_WPPK && item <= ITEM_LASER;
}

static s32 coopSpectateHandsAuto(s32 item)
{
    return (item >= ITEM_SKORPION && item <= ITEM_FNP90) || item == ITEM_AUTOSHOT || item == ITEM_LASER;
}

/* back to our own empty hands: the partner's guns put away, their rounds not ours */
static void coopSpectateHandsEnd(void)
{
    s32 h;

    if (!sHands)
    {
        return;
    }
    sHands = 0;
    sHandsFor = -1;
    if (g_CurrentPlayer == NULL)
    {
        return;
    }
    for (h = GUNRIGHT; h <= GUNLEFT; h++)
    {
        g_CurrentPlayer->hands[h].weapon_ammo_in_magazine = 0;
    }
    currentPlayerEquipWeaponWrapper(GUNLEFT, ITEM_UNARMED);
    if (g_CurrentPlayer->bonddead)
    {   /* (back up, the respawn has asked for our own gun already) */
        currentPlayerEquipWeaponWrapper(GUNRIGHT, ITEM_UNARMED);
    }
    if (g_CurrentPlayer->bonddead)
    {
        gunSetSightVisible(GUNSIGHTREASON_DAMAGE, FALSE);   /* down: hidden again, as the death left it */
    }
    portConLogf("ge: coop: spectate: hands back (%d shots shown)\n", sHandsShots);
}

static void coopSpectateHandsStart(s32 who, const CoopLook *look)
{
    sHands = 1;
    sHandsFor = who;
    sHandsSeq = look->shotseq;      /* their shots from now on */
    sHandsCue = look->cue & 0x1F;
    sHandsFlash = look->flash;
    sHandsShots = 0;
    sHandsWant[GUNRIGHT] = sHandsWant[GUNLEFT] = -1;
    sHandsHold[GUNRIGHT] = sHandsHold[GUNLEFT] = 0.0f;
    sHandsPress[GUNRIGHT] = sHandsPress[GUNLEFT] = 0.0f;
    sHandsPulse[GUNRIGHT] = sHandsPulse[GUNLEFT] = 0;
    portConLogf("ge: coop: spectate: player %d's guns in our hands (%d, %d)\n", who + 1, (int) look->weapon[GUNRIGHT],
                (int) look->weapon[GUNLEFT]);
}

/* a reload state, or one asked for: the magazine is left alone */
static s32 coopSpectateHandsReloading(struct hand *hand)
{
    return hand->weapon_current_animation == 9
        || (hand->weapon_action_state >= GUN_ANIM_STATE_RELOAD_START && hand->weapon_action_state <= GUN_ANIM_STATE_RELOAD_RAISE);
}

s32 coopSpectateHandsTick(s32 *trigger, PropRecord **partner)
{
    CoopExtView v;
    const CoopLook *look;
    s32 who = coopSpectateTarget();
    s32 h, i, n, reload;

    trigger[GUNRIGHT] = trigger[GUNLEFT] = 0;
    *partner = NULL;
    if (who < 0 || g_CurrentPlayer == NULL || !g_CurrentPlayer->bonddead || !coopExtRemoteView(who, &v) || v.look == NULL)
    {   /* (a drop-in watches standing up, with a gun of its own in its hands: those stay put) */
        coopSpectateHandsEnd();
        return 0;
    }
    look = v.look;
    if (!sHands || sHandsFor != who)
    {
        coopSpectateHandsStart(who, look);
    }
    *partner = v.prop;
    sHandsAiming = v.aiming;
    if (g_CurrentPlayer->damageshowtime < 0)
    {   /* down, the sight is hidden as in a hit: theirs shows */
        gunSetSightVisible(GUNSIGHTREASON_DAMAGE, TRUE);
    }

    /* their guns: a change plays the put away and the draw (and if anything
     * else put a gun in our hands meanwhile, theirs goes back) */
    for (h = GUNRIGHT; h <= GUNLEFT; h++)
    {
        const s32 want = coopSpectateHandsItem(look->weapon[h]);
        struct hand *hand = &g_CurrentPlayer->hands[h];

        if (want != sHandsWant[h]
            || (getCurrentPlayerWeaponId(h) != want && hand->weapon_next_weapon != want))
        {
            sHandsWant[h] = want;
            sHandsHold[h] = sHandsPress[h] = 0.0f;
            sHandsPulse[h] = 0;
            currentPlayerEquipWeaponWrapper(h, want);
        }
    }

    /* their new shots, oldest first: the trigger, and the tracer to where each stopped */
    n = (u8) (look->shotseq - sHandsSeq);
    if (n > COOP_SHOTS_KEPT)
    {
        n = COOP_SHOTS_KEPT;    /* more came than a STATE keeps: the newest */
    }
    for (i = n - 1; i >= 0; i--)
    {
        const CoopShot *shot = &look->shot[i];
        struct hand *hand;

        h = (shot->kind & COOPSHOT_LEFT) ? GUNLEFT : GUNRIGHT;
        hand = &g_CurrentPlayer->hands[h];
        hand->item_related.f[0] = shot->pos[0];
        hand->item_related.f[1] = shot->pos[1];
        hand->item_related.f[2] = shot->pos[2];
        if (coopSpectateHandsAuto(getCurrentPlayerWeaponId(h)))
        {
            sHandsHold[h] = HANDS_HOLD;
        }
        else if (sHandsPulse[h] < HANDS_PULSES)
        {
            sHandsPulse[h]++;
        }
        sHandsShots++;
    }
    sHandsSeq = look->shotseq;
    for (h = GUNRIGHT; h <= GUNLEFT; h++)
    {   /* a throw, a stab or a punch has no shot: its flash */
        if ((look->flash & (1 << h)) && !(sHandsFlash & (1 << h)) && !coopSpectateHandsBullets(getCurrentPlayerWeaponId(h))
            && getCurrentPlayerWeaponId(h) != ITEM_UNARMED && sHandsPulse[h] < HANDS_PULSES)
        {
            sHandsPulse[h]++;
        }
    }
    sHandsFlash = look->flash;

    /* a reload: both hands, as the reload key does */
    reload = 0;
    if ((look->cue & 0x1F) != sHandsCue)
    {
        sHandsCue = look->cue & 0x1F;
        reload = ((look->cue >> 5) & 3) == COOPCUE_RELOAD;
    }
    for (h = GUNRIGHT; h <= GUNLEFT; h++)
    {
        struct hand *hand = &g_CurrentPlayer->hands[h];
        const s32 item = getCurrentPlayerWeaponId(h);
        const s32 mag = (item != ITEM_UNARMED && get_ammo_type_for_weapon(item) != 0) ? get_ptr_item_statistics(item)->MagSize : 0;

        if (mag > 0 && reload)
        {
            hand->weapon_ammo_in_magazine = (mag > 1) ? mag - 1 : 0;    /* room for the rounds: the reload plays */
            sHandsHold[h] = sHandsPress[h] = 0.0f;
            sHandsPulse[h] = 0;
            attempt_reload_item_in_hand(h);
        }
        else if (mag > 0 && !coopSpectateHandsReloading(hand))
        {
            hand->weapon_ammo_in_magazine = mag;    /* never runs dry of its own: they reload when they do */
        }

        if (sHandsHold[h] > 0.0f)
        {
            sHandsHold[h] -= g_GlobalTimerDelta;
            trigger[h] = 1;
        }
        else if (sHandsPress[h] > 0.0f)
        {   /* a press: down for HANDS_PRESS, then up as long */
            sHandsPress[h] -= g_GlobalTimerDelta;
            trigger[h] = 1;
            if (sHandsPress[h] <= 0.0f)
            {
                sHandsPress[h] = -HANDS_PRESS;
            }
        }
        else if (sHandsPress[h] < 0.0f)
        {
            sHandsPress[h] += g_GlobalTimerDelta;
            if (sHandsPress[h] > 0.0f)
            {
                sHandsPress[h] = 0.0f;
            }
        }
        else if (sHandsPulse[h] > 0)
        {   /* one press a shot */
            sHandsPulse[h]--;
            sHandsPress[h] = HANDS_PRESS;
            trigger[h] = 1;
        }
    }
    return 1;
}

/* bondview2.c, each frame before the hands are drawn: their gun points where
 * they aim (field_FFC, field_A38..A40) and the sight sits there (crosshair_angle) */
void coopSpectateHandsAim(void)
{
    CoopExtView v;
    coord3d dir, aim;
    coord2d at;
    f32 ay, ap, w, h, left, top;

    if (!sHands || g_CurrentPlayer == NULL || !coopExtRemoteView(sHandsFor, &v) || v.look == NULL)
    {
        return;
    }
    ay = v.look->aimyaw;
    ap = v.look->aimpitch - v.viewpitch;
    {   /* GE_COOP_TESTHANDSAIM=<yaw>:<pitch>: degrees added to theirs, right and up (harness only) */
        static s32 test = -1;
        static f32 tyaw, tpitch;

        if (test < 0)
        {
            const char *e = getenv("GE_COOP_TESTHANDSAIM");

            test = (e != NULL && sscanf(e, "%f:%f", &tyaw, &tpitch) == 2);
        }
        if (test)
        {
            ay -= tyaw * DEG;
            ap += tpitch * DEG;
        }
    }
    if (!(ay == ay) || !(ap == ap))
    {
        return;
    }
    ay = (ay > HANDS_AIM_MAX) ? HANDS_AIM_MAX : (ay < -HANDS_AIM_MAX) ? -HANDS_AIM_MAX : ay;
    ap = (ap > HANDS_AIM_MAX) ? HANDS_AIM_MAX : (ap < -HANDS_AIM_MAX) ? -HANDS_AIM_MAX : ap;
    /* their camera's frame (bondview2.c: field_2A0C = atan2(-x, -z), field_2A08 = atan2(y, xz) + pitch) */
    dir.f[0] = -cosf(ap) * sinf(ay);
    dir.f[1] = sinf(ap);
    dir.f[2] = -cosf(ap) * cosf(ay);
    transform3Dto2DCoords(&dir, &at);
    at.x = bondviewAimPinX(at.x);
    w = getPlayer_c_screenwidth();
    h = getPlayer_c_screenheight();
    left = getPlayer_c_screenleft();
    top = getPlayer_c_screentop();
    at.x = (at.x < left + 3.0f) ? left + 3.0f : (at.x > left + w - 4.0f) ? left + w - 4.0f : at.x;
    at.y = (at.y < top + 3.0f) ? top + 3.0f : (at.y > top + h - 4.0f) ? top + h - 4.0f : at.y;
    g_CurrentPlayer->crosshair_angle.f[0] = at.x;
    g_CurrentPlayer->crosshair_angle.f[1] = at.y;
    g_CurrentPlayer->field_FFC.x = at.x;
    g_CurrentPlayer->field_FFC.y = at.y;
    aim.f[0] = dir.f[0] * 1000.0f;
    aim.f[1] = dir.f[1] * 1000.0f;
    aim.f[2] = dir.f[2] * 1000.0f;
    sub_GAME_7F067AB4(&aim);
}

/* bondview2.c: the sight is up while they aim */
s32 coopSpectateHandsAiming(s32 aiming)
{
    return sHands ? sHandsAiming : aiming;
}

/* GE_COOP_TESTAIMHOLD=<s>[:<len>]: from <s> seconds into the mission hold aim
 * for <len> seconds (6) (harness only: a partner watching sees the sight and
 * the zoom) */
void coopSpectateTestAimHold(s8 *stick_x, s8 *stick_y, u16 *buttons)
{
    static s32 at = -2, len;
    s32 t;

    (void) stick_x;
    (void) stick_y;
    if (at == -2)
    {
        const char *e = getenv("GE_COOP_TESTAIMHOLD");

        at = -1;
        len = 6;
        if (e != NULL && sscanf(e, "%d:%d", &at, &len) >= 1)
        {
            at *= 60;
        }
    }
    t = (s32) coopExtMissionTicks() - at;
    if (at < 0 || t < 0 || t >= len * 60)
    {
        return;
    }
    *buttons |= R_TRIG;
    if (t == 0)
    {
        portConLogf("ge: coop: testaimhold: aiming for %d s\n", len);
    }
}
