/**
 * Accessibility: sound captions, colour-blind player colours, text size and
 * screen shake (the Accessibility sheet; the settings live in video.c).
 *
 * GoldenEye has no spoken dialogue to subtitle: briefings and mission
 * messages are already text. What a player who can't hear misses is the
 * sound of the world, so the captions say that instead, with where it came
 * from: "GUNFIRE - BEHIND", "EXPLOSION - LEFT", "DOOR OPENS - RIGHT", and
 * "ALARM" for as long as the alarm sounds. They sit above the mission
 * message line, at most COOPCAP_LINES, solo and co-op.
 *
 * Colours: every player colour (name tags, radar, pings, chat, toasts) comes
 * from coopPlayerColour, which picks the colour-blind set when it is on.
 * Text size scales the co-op text (chat, toasts, name tags, pings) and the
 * captions, through gfx_pc's HUD scale brackets.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include "math_atan2f.h"
#include "textrelated.h"
#include "fr.h"
#include "player.h"
#include "othermodemicrocode.h"
#include "bondview.h"
#include "options.h"
#include "coop.h"

extern void portConLogf(const char *fmt, ...);

extern s32 portAccessCaptions(void);     /* video.c */
extern s32 portAccessColorblind(void);
extern s32 portAccessScreenShake(void);
extern s32 portAccessTextSize(void);
bool alarmIsActive(void);

/* ---- colours -------------------------------------------------------------- */

/* by player number, alpha 0xFF: standard (yellow, cyan, pink, green), then
 * sets for red-green (Okabe-Ito yellow, sky blue, white, vermilion) and
 * blue-yellow colour blindness (white, red, teal, lavender) */
u32 coopPlayerColour(s32 player)
{
    static const u32 sets[3][COOP_MAX_PLAYERS] = {
        { 0xFFFF80FF, 0x80E0FFFF, 0xFF90E0FF, 0x90FF90FF },
        { 0xF0E442FF, 0x56B4E9FF, 0xF0F0F0FF, 0xE8702AFF },
        { 0xF0F0F0FF, 0xFF5A5AFF, 0x00C8C8FF, 0xC8A0FFFF },
    };
    s32 set = portAccessColorblind();

    if (set < 0 || set > 2)
    {
        set = 0;
    }
    return sets[set][(player >= 0 && player < COOP_MAX_PLAYERS) ? player : 0];
}

/* ---- text size ------------------------------------------------------------ */

f32 coopTextScale(void)
{
    s32 pct = portAccessTextSize();

    return (pct >= 100 && pct <= 200) ? pct / 100.0f : 1.0f;
}

static s32 sTextBracket;    /* coopTextScaleBegin opened a bracket */

/* scale what follows about (x, y) by Text Size, on top of HUD Scale's half
 * size; ends with coopTextScaleEnd. Replaces portHudScaleBegin/End. */
Gfx *coopTextScaleBegin(Gfx *gdl, f32 x, f32 y)
{
    f32 s = (portHudScaleOn() ? 0.5f : 1.0f) * coopTextScale();
    s32 x4 = (s32) (x * 4.0f + 0.5f);
    s32 y4 = (s32) (y * 4.0f + 0.5f);

    sTextBracket = 0;
    if (s > 0.99f && s < 1.01f)
    {
        return gdl;
    }
    x4 = (x4 < 0) ? 0 : (x4 > 0xFFE) ? 0xFFE : x4;
    y4 = (y4 < 0) ? 0 : (y4 > 0xFFE) ? 0xFFE : y4;
    gDPNoOpTag(gdl++, 0x5A000000u | (u32) (s * 256.0f + 0.5f));
    gDPNoOpTag(gdl++, 0x58000000u | ((u32) x4 << 12) | (u32) y4);
    sTextBracket = 1;
    return gdl;
}

Gfx *coopTextScaleEnd(Gfx *gdl)
{
    if (sTextBracket)
    {
        gDPNoOpTag(gdl++, 0x58FFFFFFu);
        sTextBracket = 0;
    }
    return gdl;
}

/* ---- screen shake --------------------------------------------------------- */

s32 coopScreenShakeOn(void)
{
    return portAccessScreenShake();
}

/* ---- captions ------------------------------------------------------------- */

#define COOPCAP_LINES   4
#define COOPCAP_SHOW_MS 2500
#define COOPCAP_FADE_MS 500
#define COOPCAP_MERGE_MS 1500   /* the same sound from the same side refreshes its line */

static const char *const sCapText[COOPCAP_COUNT] = { "GUNFIRE", "EXPLOSION", "DOOR OPENS", "ALARM" };
static const f32 sCapRange[COOPCAP_COUNT] = { 4000.0f, 8000.0f, 1500.0f, 0.0f };   /* about 40, 80 and 15 m */

typedef struct
{
    s8 kind;
    s8 side;        /* 0 ahead, 1 right, 2 behind, 3 left */
    u32 ms;         /* wall clock when last heard */
} CoopCaption;

static CoopCaption sCap[COOPCAP_LINES];
static s32 sCapCount;

static u32 coopCapNowMs(void)
{
    return (u32) (osGetTime() / 46875ull);
}

/* which side of the view pos is on (the radar's convention, coop_hud.c) */
static s32 coopCapSide(coord3d *pos, f32 *dist)
{
    coord3d *self = bondviewGetCurrentPlayersPosition();
    f32 dx, dz, rel;

    if (self == NULL)
    {
        *dist = 0.0f;
        return 0;
    }
    dx = pos->f[0] - self->f[0];
    dz = pos->f[2] - self->f[2];
    *dist = sqrtf(dx * dx + dz * dz + (pos->f[1] - self->f[1]) * (pos->f[1] - self->f[1]));
    rel = -((atan2f(dx, dz) * 180.0f) / M_PI_F + g_CurrentPlayer->vv_theta);
    while (rel > 180.0f)
    {
        rel -= 360.0f;
    }
    while (rel <= -180.0f)
    {
        rel += 360.0f;
    }
    if (rel > 45.0f && rel < 135.0f)
    {
        return 1;
    }
    if (rel < -45.0f && rel > -135.0f)
    {
        return 3;
    }
    return (rel >= 135.0f || rel <= -135.0f) ? 2 : 0;
}

/* a sound worth a caption happened at pos (solo or co-op) */
void coopCaption(s32 kind, coord3d *pos)
{
    u32 now = coopCapNowMs();
    f32 dist;
    s32 side;
    s32 i;

    if (!portAccessCaptions() || kind < 0 || kind >= COOPCAP_COUNT || g_CurrentPlayer == NULL
        || g_CurrentPlayer->prop == NULL || pos == NULL)
    {
        return;
    }
    side = coopCapSide(pos, &dist);
    if (sCapRange[kind] > 0.0f && dist > sCapRange[kind])
    {
        return;
    }
    if (kind == COOPCAP_DOOR && dist < 300.0f)
    {
        return;     /* a door you opened yourself */
    }
    for (i = 0; i < sCapCount; i++)
    {
        if (sCap[i].kind == kind && sCap[i].side == side && now - sCap[i].ms < COOPCAP_MERGE_MS)
        {
            sCap[i].ms = now;
            return;
        }
    }
    if (sCapCount == COOPCAP_LINES)
    {
        memmove(&sCap[0], &sCap[1], sizeof(sCap[0]) * (COOPCAP_LINES - 1));
        sCapCount--;
    }
    sCap[sCapCount].kind = (s8) kind;
    sCap[sCapCount].side = (s8) side;
    sCap[sCapCount].ms = now;
    sCapCount++;
}

/* chraction.c: a chr's gun went off (guards, and partners' puppets too) */
void coopCaptionChrGunfire(ChrRecord *chr)
{
    if (chr != NULL && chr->prop != NULL && !coopPropIsPartner(chr->prop))
    {
        coopCaption(COOPCAP_GUNFIRE, &chr->prop->pos);
    }
}

static Gfx *coopCapPrint(Gfx *gdl, s32 x, s32 y, char *text, u32 colour)
{
    return textRenderOutlined(gdl, &x, &y, text, ptrFontBankGothicChars, ptrFontBankGothic, colour,
                              (colour & 0xFF) * 5 / 8, (s32) viGetX(), viGetY(), 0, 0);
}

static void coopCapExpire(u32 now)
{
    while (sCapCount > 0 && now - sCap[0].ms >= COOPCAP_SHOW_MS)
    {
        memmove(&sCap[0], &sCap[1], sizeof(sCap[0]) * (COOPCAP_LINES - 1));
        sCapCount--;
    }
}

/* Modern captions (coop_captions.c) draw these in their own stack, oldest
 * first: "[Gunfire - behind]", and "[Alarm]" while it sounds */
s32 coopCaptionSoundLines(char lines[][48], s32 max)
{
    static const char *const text[COOPCAP_COUNT] = { "Gunfire", "Explosion", "Door opens", "Alarm" };
    static const char *const sides[4] = { "ahead", "right", "behind", "left" };
    s32 n = 0;
    s32 i;

    coopCapExpire(coopCapNowMs());
    if (!portAccessCaptions())
    {
        return 0;
    }
    for (i = 0; i < sCapCount && n < max; i++)
    {
        snprintf(lines[n++], 48, "[%s - %s]", text[(s32) sCap[i].kind], sides[(s32) sCap[i].side]);
    }
    if (alarmIsActive() && n < max)
    {
        snprintf(lines[n++], 48, "[%s]", text[COOPCAP_ALARM]);
    }
    return n;
}

/* above the mission message line, centred, newest lowest (Modern captions
 * draw them instead, coop_captions.c) */
Gfx *coopCaptionsRender(Gfx *gdl)
{
    static const char *const sides[4] = { "AHEAD", "RIGHT", "BEHIND", "LEFT" };
    u32 now = coopCapNowMs();
    s32 cx = viGetViewLeft() + viGetViewWidth() / 2;
    s32 bottom = viGetViewTop() + viGetViewHeight() - 0x48;
    s32 alarm;
    s32 rows = 0;
    s32 i;

    coopCapExpire(now);
    alarm = portAccessCaptions() && alarmIsActive();
    if (!portAccessCaptions() || (sCapCount == 0 && !alarm) || coopSubtitlesOn())
    {
        return gdl;
    }
    gdl = coopTextScaleBegin(gdl, (f32) cx, (f32) bottom + 0x0C);
    gdl = microcode_constructor(gdl);
    for (i = sCapCount - 1 + alarm; i >= 0; i--)
    {
        char line[48];
        s32 th = 0, tw = 0;
        s32 a = 0xFF;
        s32 y = bottom - rows * 0x0C;

        if (alarm && i == sCapCount)
        {
            snprintf(line, sizeof(line), "[ ALARM ]\n");
        }
        else
        {
            u32 age = now - sCap[i].ms;

            if (age > COOPCAP_SHOW_MS - COOPCAP_FADE_MS)
            {
                a = (s32) ((COOPCAP_SHOW_MS - age) * 0xFF / COOPCAP_FADE_MS);
            }
            snprintf(line, sizeof(line), "[ %s - %s ]\n", sCapText[(s32) sCap[i].kind], sides[(s32) sCap[i].side]);
        }
        textMeasure(&th, &tw, line, ptrFontBankGothicChars, ptrFontBankGothic, 0);
        gdl = microcode_constructor_related_to_menus(gdl, cx - tw / 2 - 3, y - 2, cx + tw / 2 + 3, y + 0x0A,
                                                     (u32) (a / 2));
        gdl = coopCapPrint(gdl, cx - tw / 2, y, line, 0xFFFFFF00u | (u32) a);
        rows++;
    }
    gdl = combiner_bayer_lod_perspective(gdl);
    gdl = coopTextScaleEnd(gdl);
    return gdl;
}

/* harness: GE_CAPTEST=<s> makes one of each caption around Bond <s> s (wall
 * clock) after the first mission HUD frame, and again every 0.9 s for 10 s
 * so a screenshot can catch them */
void coopCaptionHarness(void)
{
    static s32 at = -2;
    static s32 step;
    static u32 base;
    u32 now = coopCapNowMs();
    coord3d *self;
    coord3d pos;

    if (at == -2)
    {
        const char *e = getenv("GE_CAPTEST");

        at = (e != NULL) ? atoi(e) : -1;
        base = now;
    }
    if (at < 0 || step >= 33 || now - base < (u32) at * 1000u + (u32) (step / 3) * 900u + (u32) (step % 3) * 300u
        || (self = bondviewGetCurrentPlayersPosition()) == NULL)
    {
        return;
    }
    pos = *self;
    if (step % 3 == 0)
    {   /* behind */
        f32 t = g_CurrentPlayer->vv_theta * (M_PI_F / 180.0f);

        pos.f[0] += sinf(t) * 1500.0f;
        pos.f[2] -= cosf(t) * 1500.0f;
        coopCaption(COOPCAP_GUNFIRE, &pos);
    }
    else if (step % 3 == 1)
    {   /* left or right */
        f32 t = g_CurrentPlayer->vv_theta * (M_PI_F / 180.0f);

        pos.f[0] += cosf(t) * 2000.0f;
        pos.f[2] += sinf(t) * 2000.0f;
        coopCaption(COOPCAP_EXPLOSION, &pos);
    }
    else
    {   /* ahead */
        f32 t = g_CurrentPlayer->vv_theta * (M_PI_F / 180.0f);

        pos.f[0] -= sinf(t) * 800.0f;
        pos.f[2] += cosf(t) * 800.0f;
        coopCaption(COOPCAP_DOOR, &pos);
    }
    if (step < 3)
    {
        portConLogf("ge: captions: test caption %d\n", (int) step);
    }
    step++;
}
