/**
 * Story co-op: a partner right in front of you fades out.
 *
 * Two Bonds pass through each other (the puppet is not solid up close), and a
 * partner that close fills the view with the inside of their jacket. So the
 * puppet's model fades with the distance between the two of you: whole from
 * COOPFADE_FAR out, gone at COOPFADE_NEAR, smoothly in between, and back as
 * you part. The amount also eases over a few frames so a teleport (a respawn
 * beside you) doesn't pop.
 *
 * Story only: versus players are never faded (they are targets, coop_versus.c
 * draws them as Rare does), nor in cutscenes or while spectating, where the
 * camera isn't where our Bond stands. The partner_fade option turns it off.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include "bondview.h"
#include "math_atan2f.h"
#include "player.h"
#include "coop.h"

extern void portConLogf(const char *fmt, ...);
extern s32 portPartnerFade(void);   /* video.c: [display] partner_fade */

#define COOPFADE_FAR   260.0f   /* about 2.6 m: whole */
#define COOPFADE_NEAR  100.0f   /* about 1 m, about where two Bonds touch: gone */
#define COOPFADE_FLOOR 220.0f   /* a partner this far above or below is on another floor: whole */
#define COOPFADE_EASE_MS 90.0f  /* time constant of the easing */

typedef struct
{
    PropRecord *prop;
    f32 amount;     /* 0 gone .. 1 whole, eased */
    u32 ms;         /* wall clock of the last update */
} CoopFade;

static CoopFade sFade[COOP_MAX_PLAYERS];

static u32 coopFadeNowMs(void)
{
    return (u32) (osGetTime() / 46875ull);
}

/* how much of a partner the distance alone asks for, 0..1 */
static f32 coopFadeTarget(PropRecord *prop)
{
    PropRecord *self;
    f32 dx, dy, dz, d, t;
    s32 who;

    if (!portPartnerFade() || coopVersus() || getPlayerCount() != 1 || g_CurrentPlayer == NULL
        || (self = g_CurrentPlayer->prop) == NULL || coopCutsceneActive() || coopSpectateHud(&who) != 0)
    {
        return 1.0f;
    }
    dx = prop->pos.f[0] - self->pos.f[0];
    dy = prop->pos.f[1] - self->pos.f[1];
    dz = prop->pos.f[2] - self->pos.f[2];
    if (dy > COOPFADE_FLOOR || dy < -COOPFADE_FLOOR)
    {
        return 1.0f;
    }
    d = sqrtf(dx * dx + dz * dz);
    if (d >= COOPFADE_FAR)
    {
        return 1.0f;
    }
    if (d <= COOPFADE_NEAR)
    {
        return 0.0f;
    }
    t = (d - COOPFADE_NEAR) / (COOPFADE_FAR - COOPFADE_NEAR);
    return t * t * (3.0f - 2.0f * t);   /* smoothstep: no edge where it starts or ends */
}

/* chr.c chrRenderProp: scale a chr's fade alpha (0..255) for a partner up close;
 * anything not a partner comes back as it went in */
s32 coopFadePartnerAlpha(PropRecord *prop, s32 alpha)
{
    u32 now = coopFadeNowMs();
    CoopFade *f = NULL;
    f32 target;
    s32 i;

    if (prop == NULL || !coopPropIsPartner(prop))
    {
        return alpha;
    }
    target = coopFadeTarget(prop);
    for (i = 0; i < COOP_MAX_PLAYERS && f == NULL; i++)
    {
        if (sFade[i].prop == prop)
        {
            f = &sFade[i];
        }
    }
    for (i = 0; i < COOP_MAX_PLAYERS && f == NULL; i++)
    {
        if (sFade[i].prop == NULL || !coopPropIsPartner(sFade[i].prop))
        {   /* a new puppet (or one rebuilt): start where the distance says */
            f = &sFade[i];
            f->prop = prop;
            f->amount = target;
            f->ms = now;
        }
    }
    if (f == NULL)
    {
        return (s32) (target * (f32) alpha);
    }
    if (now - f->ms > 500u)
    {
        f->amount = target;     /* not drawn for a while: no easing from stale */
    }
    else
    {   /* the opaque and translucent passes of one frame see dt 0 */
        f32 k = (f32) (now - f->ms) / COOPFADE_EASE_MS;

        f->amount += (target - f->amount) * ((k > 1.0f) ? 1.0f : k);
    }
    f->ms = now;
    if (f->amount > 0.995f)
    {
        return alpha;
    }
    return (s32) (f->amount * (f32) alpha);
}

/* harness: GE_COOP_TESTWALK=<s>:<in|out|fwd>:<m>[,...]: <s> seconds (wall
 * clock) after the first co-op HUD frame, face the first partner and walk
 * forward until within <m> metres of them (in), or backward until <m> metres
 * away (out), or walk <m> metres the way we face (fwd); each step gives up
 * after 8 s. Drives GE_GOTO's stick (lv.c). For the
 * partner fade recordings. */
void coopFadeHarness(void)
{
    extern s32 g_PortGotoActive, g_PortGotoStickX, g_PortGotoStickY;
    static s32 n = -1, next, on;
    static s32 at[6];
    static char mode[6][8];
    static f32 metres[6];
    static u32 base;
    static coord3d from;
    u32 now = coopFadeNowMs();
    CoopHudPartner p;
    coord3d *me;
    f32 dx, dz, d, gone, want;
    s32 in, fwd, done;

    if (n < 0)
    {
        const char *e = getenv("GE_COOP_TESTWALK");

        n = 0;
        base = now;
        while (e != NULL && n < 6 && sscanf(e, "%d:%7[a-z]:%f", &at[n], mode[n], &metres[n]) == 3)
        {
            n++;
            e = strchr(e, ',');
            e = (e != NULL) ? e + 1 : NULL;
        }
    }
    if (next >= n || now - base < (u32) at[next] * 1000u || g_CurrentPlayer == NULL || g_CurrentPlayer->prop == NULL
        || !coopHudPartnerAt(0, &p))
    {
        return;
    }
    me = &g_CurrentPlayer->prop->pos;
    in = strcmp(mode[next], "in") == 0;
    fwd = strcmp(mode[next], "fwd") == 0;
    if (!on)
    {
        on = 1;
        from = *me;
    }
    dx = p.pos.f[0] - me->f[0];
    dz = p.pos.f[2] - me->f[2];
    d = sqrtf(dx * dx + dz * dz);
    gone = sqrtf((me->f[0] - from.f[0]) * (me->f[0] - from.f[0]) + (me->f[2] - from.f[2]) * (me->f[2] - from.f[2]));
    done = fwd ? gone >= metres[next] * 100.0f : in ? d <= metres[next] * 100.0f : d >= metres[next] * 100.0f;
    if (done || now - base > (u32) at[next] * 1000u + 8000u)
    {
        portConLogf("ge: coop: testwalk %s %.1f m done, partner %.0f cm away\n", mode[next], metres[next], d);
        g_PortGotoActive = 0;
        g_PortGotoStickX = 0;
        g_PortGotoStickY = 0;
        on = 0;
        next++;
        return;
    }
    if (!fwd && d > 20.0f)
    {   /* face them (lv.c portGotoTick's angle convention); the view is set, not
         * steered, so the walk is straight at them */
        want = 360.0f - (atan2f(dx, dz) * 360.0f / 6.2831855f);
        g_CurrentPlayer->vv_theta = (want >= 360.0f) ? want - 360.0f : want;
    }
    g_PortGotoStickX = 0;
    g_PortGotoStickY = (in || fwd) ? 30 : -30;   /* a walk */
    g_PortGotoActive = 1;
}
