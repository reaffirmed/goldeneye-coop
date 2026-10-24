/**
 * Co-op and mission HUD (port only).
 *
 *   - the partner on a radar in the top-right corner, drawn like the
 *     multiplayer radar (radar.c), and their name and distance over them
 *   - the partner's health and armour under their name in the top-left
 *     corner, or how long until they are back when they are down
 *   - our own wait to respawn while we are down, or to be revived (the
 *     lobby's When Down rule), with the revive's progress
 *   - a downed partner who can be revived: red on the radar and a red
 *     REVIVE tag over where they fell
 *   - partners' pings (coop_ping.c): a diamond in the pinger's colour where
 *     they pinged, with their name and the distance, held to the edge of the
 *     screen when it is off it, and a square on the radar
 *   - a partner whose connection dropped: LOST on their tag and status, and
 *     a banner across the top while the host is not answering or after a
 *     new host takes over (coop.c, host migration)
 *   - the mission's objectives in the top-right corner, solo too
 *     ([display] objectives_hud, toggled in play by the objectives_hud key)
 *
 * Everything is drawn in the small BankGothic font, which has no lowercase.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include "math_atan2f.h"
#include "textrelated.h"
#include "front.h"
#include "fr.h"
#include "player.h"
#include "othermodemicrocode.h"
#include "image_bank.h"
#include "bondview.h"
#include "options.h"
#include "lv.h"
#include "objective_status.h"
#include "language.h"
#include "coop.h"

extern Gfx *portHudScaleBegin(Gfx *gdl, f32 x, f32 y);
extern Gfx *portHudScaleEnd(Gfx *gdl);
extern s32 portObjectivesHudShown(void);    /* video.c: [display] objectives_hud and its key */

#define COOP_HUD_COL_TEXT   0xFFFFFFFF
#define COOP_HUD_COL_DIM    0xB0B0B0C0
#define COOP_HUD_COL_DONE   0x60FF60D0
#define COOP_HUD_COL_FAIL   0xFF4040FF
#define COOP_HUD_COL_SHADOW 0x000000A0
#define COOP_HUD_COL_TAG    0xFFFF80FF  /* the first partner's yellow, as their radar dot */

/* each player's colour (by player number) for their tag and radar dot, alpha 0xFF */
static u32 coopHudPlayerColour(s32 player)
{
    static const u32 colours[COOP_MAX_PLAYERS] = { 0xFFFF80FF, 0x80E0FFFF, 0xFF90E0FF, 0x90FF90FF };

    return colours[(player >= 0 && player < COOP_MAX_PLAYERS) ? player : 0];
}

/* the radar's centre, as radar.c lays it out for one player */
#define COOP_RADAR_TOP   0x1a
#define COOP_RADAR_RIGHT 0x29

/* the small font in capitals, ASCII only (a byte >= 0x80 starts a two-byte glyph) */
static void coopHudText(char *out, s32 cap, const char *in)
{
    s32 n = 0;
    s32 i;

    for (i = 0; in != NULL && in[i] != '\0' && in[i] != '\n' && n < cap - 2; i++)
    {
        char c = in[i];

        if ((u8) c >= 0x80 || (u8) c < 0x20)
        {
            c = '?';
        }
        else if (c >= 'a' && c <= 'z')
        {
            c -= 'a' - 'A';
        }
        out[n++] = c;
    }
    out[n++] = '\n';
    out[n] = '\0';
}

/* cut a line (ending in '\n') with "..." so it is no wider than maxw */
static void coopHudFit(char *text, s32 maxw)
{
    char orig[128];
    s32 th = 0, tw = 0;
    s32 k;

    textMeasure(&th, &tw, text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    if (tw <= maxw)
    {
        return;
    }
    snprintf(orig, sizeof(orig), "%s", text);
    for (k = (s32) strlen(orig) - 1; k > 1; k--)
    {
        sprintf(text, "%.*s...\n", (int) k, orig);
        textMeasure(&th, &tw, text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
        if (tw <= maxw)
        {
            return;
        }
    }
}

static Gfx *coopHudPrint(Gfx *gdl, s32 x, s32 y, char *text, u32 colour)
{
    return textRenderOutlined(gdl, &x, &y, text, ptrFontBankGothicChars, ptrFontBankGothic, colour, COOP_HUD_COL_SHADOW,
                              (s32) viGetX(), viGetY(), 0, 0);
}

static Gfx *coopHudBar(Gfx *gdl, s32 x, s32 y, s32 w, f32 frac, u32 colour)
{
    s32 fill;

    if (frac < 0.0f)
    {
        frac = 0.0f;
    }
    if (frac > 1.0f)
    {
        frac = 1.0f;
    }
    fill = (s32) (frac * (f32) w + 0.5f);
    gdl = microcode_constructor_related_to_menus(gdl, x - 1, y - 1, x + w + 1, y + 3, 0x00000090);
    if (fill > 0)
    {
        gdl = microcode_constructor_related_to_menus(gdl, x, y, x + fill, y + 2, colour);
    }
    return gdl;
}

/* The name tags' size and ink: the text is drawn at full size about the
 * point over the head and scaled there (gfx_pc.cpp's 0x5A / 0x58 tags), from
 * COOP_TAG_NEAR_SCALE at COOP_TAG_NEAR and closer to COOP_TAG_FAR_SCALE at
 * COOP_TAG_FAR and beyond. Right beside us the tag fades out (it would cover
 * them); far off it fades to COOP_TAG_FAR_ALPHA but stays, to find them by. */
#define COOP_TAG_NEAR        300.0f     /* 3 m */
#define COOP_TAG_FAR         3000.0f    /* 30 m */
#define COOP_TAG_NEAR_SCALE  0.55f
#define COOP_TAG_FAR_SCALE   0.32f
#define COOP_TAG_HIDE        150.0f     /* closer than this: no tag */
#define COOP_TAG_FADE_IN     300.0f     /* full ink from here */
#define COOP_TAG_FADE_FROM   4000.0f    /* and faint from here out */
#define COOP_TAG_FADE_TO     7000.0f
#define COOP_TAG_FAR_ALPHA   0x60

static u32 coopHudWithAlpha(u32 colour, f32 a)
{
    u32 alpha = (u32) ((f32) (colour & 0xFF) * a + 0.5f);

    return (colour & ~0xFFu) | (alpha > 0xFF ? 0xFF : alpha);
}

/* the partner's name and distance over their head, when they are in front of us */
static Gfx *coopHudNameTag(Gfx *gdl, CoopHudPartner *p)
{
    PropRecord *self = g_CurrentPlayer->prop;
    coord3d v;
    coord2d s;
    char name[40];
    char text[56];
    f32 dx, dy, dz, dist;
    f32 k, scale, alpha;
    s32 th = 0, tw = 0;
    s32 x, y, px, py;

    if (self == NULL || (p->dead && !p->revivable))
    {
        return gdl;
    }
    v.f[0] = p->pos.f[0];
    v.f[1] = p->pos.f[1] + 35.0f;   /* the state's y is Bond's eyes; the tag floats over the head */
    v.f[2] = p->pos.f[2];
    dx = v.f[0] - self->pos.f[0];
    dy = v.f[1] - self->pos.f[1];
    dz = v.f[2] - self->pos.f[2];
    dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist < COOP_TAG_HIDE && !p->dead)
    {
        return gdl;   /* right beside us (or inside us, just after a respawn) */
    }
    mtx4TransformVecInPlace(camGetWorldToScreenMtxf(), &v);
    if (v.f[2] > -10.0f)
    {
        return gdl;   /* behind the camera */
    }
    transform3Dto2DCoords(&v, &s);
    s.f[0] = bondviewAimPinX(s.f[0]);

    k = (dist - COOP_TAG_NEAR) / (COOP_TAG_FAR - COOP_TAG_NEAR);
    k = (k < 0.0f) ? 0.0f : (k > 1.0f) ? 1.0f : k;
    scale = COOP_TAG_NEAR_SCALE + (COOP_TAG_FAR_SCALE - COOP_TAG_NEAR_SCALE) * k;
    alpha = 1.0f;
    if (!p->dead && dist < COOP_TAG_FADE_IN)
    {
        alpha = (dist - COOP_TAG_HIDE) / (COOP_TAG_FADE_IN - COOP_TAG_HIDE);
    }
    else if (dist > COOP_TAG_FADE_FROM)
    {
        f32 f = (dist - COOP_TAG_FADE_FROM) / (COOP_TAG_FADE_TO - COOP_TAG_FADE_FROM);

        f = (f > 1.0f) ? 1.0f : f;
        alpha = 1.0f - f * (1.0f - COOP_TAG_FAR_ALPHA / 255.0f);
    }
    if (p->dead)
    {
        alpha = 1.0f;   /* a partner to revive: always plain to see */
    }

    coopHudText(name, sizeof(name), p->name);
    name[strlen(name) - 1] = '\0';
    if (coopHudPlayerLost(p->player))
    {
        snprintf(text, sizeof(text), "%s - LOST\n", name);
    }
    else if (p->dead)
    {
        snprintf(text, sizeof(text), "REVIVE %s %dM\n", name, (int) (dist / 100.0f + 0.5f));
    }
    else
    {
        snprintf(text, sizeof(text), "%s %dM\n", name, (int) (dist / 100.0f + 0.5f));
    }
    textMeasure(&th, &tw, text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    /* laid out at full size with its bottom centre on the pivot, then scaled about it */
    px = (s32) s.f[0];
    py = (s32) s.f[1];
    x = px - tw / 2;
    y = py - th;
    if (px - (s32) (tw * scale) / 2 < viGetViewLeft() + 2 || px + (s32) (tw * scale) / 2 > viGetViewLeft() + viGetViewWidth() - 2
        || py - (s32) (th * scale) < viGetViewTop() + 2 || py > viGetViewTop() + viGetViewHeight() - 2)
    {
        return gdl;
    }
    gdl = microcode_constructor(gdl);
    {
        s32 x4 = px * 4, y4 = py * 4;

        x4 = (x4 < 0) ? 0 : (x4 > 0xFFE) ? 0xFFE : x4;
        y4 = (y4 < 0) ? 0 : (y4 > 0xFFE) ? 0xFFE : y4;
        gDPNoOpTag(gdl++, 0x5A000000u | (u32) (scale * 256.0f + 0.5f));
        gDPNoOpTag(gdl++, 0x58000000u | ((u32) x4 << 12) | (u32) y4);
    }
    gdl = textRenderOutlined(gdl, &x, &y, text, ptrFontBankGothicChars, ptrFontBankGothic,
                             coopHudWithAlpha(p->dead ? COOP_HUD_COL_FAIL : coopHudPlayerColour(p->player), alpha),
                             coopHudWithAlpha(COOP_HUD_COL_SHADOW, alpha), (s32) viGetX(), viGetY(), 0, 0);
    gDPNoOpTag(gdl++, 0x58FFFFFFu);
    return combiner_bayer_lod_perspective(gdl);
}

/* a filled diamond of radius r about (px, py), as rows of rectangles */
static Gfx *coopHudDiamond(Gfx *gdl, s32 px, s32 py, s32 r, u32 colour)
{
    s32 k;

    for (k = -r; k <= r; k++)
    {
        s32 w = r - (k < 0 ? -k : k);

        gdl = microcode_constructor_related_to_menus(gdl, px - w, py + k, px + w + 1, py + k + 1, colour);
    }
    return gdl;
}

/* A ping: the diamond where it is (or at the edge of the screen toward it),
 * a ring that shrinks onto it for its first half second, and the pinger's
 * name and the distance over it. It fades out over its last two seconds. */
#define COOP_PING_FADE   120
#define COOP_PING_RING   30
#define COOP_PING_MARGIN 12
static Gfx *coopHudPing(Gfx *gdl, CoopPingMark *m)
{
    PropRecord *self = g_CurrentPlayer->prop;
    coord3d v;
    coord2d s;
    char name[40];
    char text[64];
    f32 dx, dy, dz, dist, alpha;
    s32 th = 0, tw = 0;
    s32 left = viGetViewLeft(), top = viGetViewTop();
    s32 right = left + viGetViewWidth(), bottom = top + viGetViewHeight();
    s32 px, py, x, y;
    s32 edge = 0;
    u32 colour = coopHudPlayerColour(m->player);

    if (self == NULL)
    {
        return gdl;
    }
    dx = m->pos.f[0] - self->pos.f[0];
    dy = m->pos.f[1] - self->pos.f[1];
    dz = m->pos.f[2] - self->pos.f[2];
    dist = sqrtf(dx * dx + dy * dy + dz * dz);
    alpha = (m->age > COOP_PING_LIFE - COOP_PING_FADE) ? (f32) (COOP_PING_LIFE - m->age) / (f32) COOP_PING_FADE : 1.0f;

    v = m->pos;
    mtx4TransformVecInPlace(camGetWorldToScreenMtxf(), &v);
    if (v.f[2] < -10.0f)
    {
        transform3Dto2DCoords(&v, &s);
        s.f[0] = bondviewAimPinX(s.f[0]);
        px = (s32) s.f[0];
        py = (s32) s.f[1];
        edge = px < left + COOP_PING_MARGIN || px > right - COOP_PING_MARGIN || py < top + COOP_PING_MARGIN
            || py > bottom - COOP_PING_MARGIN;
    }
    else
    {
        edge = 1;   /* behind us */
    }
    if (edge)
    {   /* on the edge of the view, the way to turn to face it */
        f32 cx = (f32) (left + right) * 0.5f, cy = (f32) (top + bottom) * 0.5f;
        f32 ex = v.f[0], ey = -v.f[1];
        f32 hw = (f32) (right - left) * 0.5f - COOP_PING_MARGIN, hh = (f32) (bottom - top) * 0.5f - COOP_PING_MARGIN;
        f32 t;

        if (ex * ex + ey * ey < 1.0e-4f)
        {
            ey = 1.0f;   /* straight behind: along the bottom */
        }
        t = 1.0e30f;
        if (ex != 0.0f)
        {
            t = hw / (ex < 0.0f ? -ex : ex);
        }
        if (ey != 0.0f && hh / (ey < 0.0f ? -ey : ey) < t)
        {
            t = hh / (ey < 0.0f ? -ey : ey);
        }
        px = (s32) (cx + ex * t);
        py = (s32) (cy + ey * t);
    }

    gdl = microcode_constructor(gdl);
    if (m->age < COOP_PING_RING && !edge)
    {   /* a square ring closing in on it */
        s32 r = 6 + (COOP_PING_RING - m->age) / 2;
        u32 rc = coopHudWithAlpha(colour, 0.8f);

        gdl = microcode_constructor_related_to_menus(gdl, px - r, py - r, px + r + 1, py - r + 1, rc);
        gdl = microcode_constructor_related_to_menus(gdl, px - r, py + r, px + r + 1, py + r + 1, rc);
        gdl = microcode_constructor_related_to_menus(gdl, px - r, py - r, px - r + 1, py + r + 1, rc);
        gdl = microcode_constructor_related_to_menus(gdl, px + r, py - r, px + r + 1, py + r + 1, rc);
    }
    gdl = coopHudDiamond(gdl, px, py, 6, coopHudWithAlpha(m->kind == COOPPING_ENEMY ? COOP_HUD_COL_FAIL : COOP_HUD_COL_SHADOW, alpha));
    gdl = coopHudDiamond(gdl, px, py, 4, coopHudWithAlpha(colour, alpha));
    gdl = coopHudDiamond(gdl, px, py, 1, coopHudWithAlpha(COOP_HUD_COL_SHADOW, alpha));

    coopHudText(name, sizeof(name), m->name);
    name[strlen(name) - 1] = '\0';
    if (m->kind == COOPPING_ENEMY)
    {
        snprintf(text, sizeof(text), "ENEMY %dM\n", (int) (dist / 100.0f + 0.5f));
    }
    else
    {
        snprintf(text, sizeof(text), "%s %dM\n", name, (int) (dist / 100.0f + 0.5f));
    }
    textMeasure(&th, &tw, text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    /* the label at 0.6 scale, its bottom centre just over the diamond, kept on screen */
    {
        const f32 scale = 0.6f;
        s32 sw = (s32) (tw * scale), sh = (s32) (th * scale);
        s32 lx = px, ly = py - 8;
        s32 x4, y4;

        if (lx - sw / 2 < left + 2)
        {
            lx = left + 2 + sw / 2;
        }
        if (lx + sw / 2 > right - 2)
        {
            lx = right - 2 - sw / 2;
        }
        if (ly - sh < top + 2)
        {
            ly = py + 8 + sh;   /* no room over it: under it */
        }
        x = lx - tw / 2;
        y = ly - th;
        x4 = lx * 4;
        y4 = ly * 4;
        x4 = (x4 < 0) ? 0 : (x4 > 0xFFE) ? 0xFFE : x4;
        y4 = (y4 < 0) ? 0 : (y4 > 0xFFE) ? 0xFFE : y4;
        gDPNoOpTag(gdl++, 0x5A000000u | (u32) (scale * 256.0f + 0.5f));
        gDPNoOpTag(gdl++, 0x58000000u | ((u32) x4 << 12) | (u32) y4);
        gdl = textRenderOutlined(gdl, &x, &y, text, ptrFontBankGothicChars, ptrFontBankGothic,
                                 coopHudWithAlpha(m->kind == COOPPING_ENEMY ? COOP_HUD_COL_FAIL : colour, alpha),
                                 coopHudWithAlpha(COOP_HUD_COL_SHADOW, alpha), (s32) viGetX(), viGetY(), 0, 0);
        gDPNoOpTag(gdl++, 0x58FFFFFFu);
    }
    return combiner_bayer_lod_perspective(gdl);
}

/* the multiplayer radar with a dot per partner */
static Gfx *coopHudRadar(Gfx *gdl)
{
    CoopHudPartner pp;
    CoopHudPartner *p = &pp;
    s32 i;
    PropRecord *self = g_CurrentPlayer->prop;
    s32 left = (viGetViewLeft() + viGetViewWidth()) - COOP_RADAR_RIGHT;
    s32 top = viGetViewTop() + COOP_RADAR_TOP;
    f32 dx, dz, ang, r;
    s32 colour;
    s32 px, py;

    if (self == NULL || g_CurrentPlayer->bonddead)
    {
        return gdl;
    }
    if (hudLayoutWide())
    {
        PORT_HUD_ANCHOR(gdl, PORT_HUD_ANCHOR_RIGHT);
    }
    gdl = portHudScaleBegin(gdl, (f32) (viGetViewLeft() + viGetViewWidth()), (f32) viGetViewTop());
    gdl = microcode_constructor(gdl);
    /* a dark square with a pencil edge: the MP radar's disc texture does not
     * draw outside multiplayer */
    gdl = microcode_constructor_related_to_menus(gdl, left - 0x11, top - 0x11, left + 0x11, top + 0x11, 0xFFFFFF30);
    gdl = microcode_constructor_related_to_menus(gdl, left - 0x10, top - 0x10, left + 0x10, top + 0x10, 0x00000080);
    gdl = microcode_constructor_related_to_menus(gdl, left - 2, top - 2, left + 2, top + 2, 0x40);
    gdl = microcode_constructor_related_to_menus(gdl, left - 1, top - 1, left + 1, top + 1, -0x60);
    {   /* pings: a hollow square in the pinger's colour (red edge for an enemy), under the partners' dots */
        CoopPingMark m;

        for (i = 0; coopPingAt(i, &m); i++)
        {
            u32 edge = (m.kind == COOPPING_ENEMY) ? 0xFF3030E0 : 0x000000C0;

            dx = m.pos.f[0] - self->pos.f[0];
            dz = m.pos.f[2] - self->pos.f[2];
            ang = ((atan2f(dx, dz) * 180.0f) / M_PI_F) + g_CurrentPlayer->vv_theta + 180.0f;
            r = sqrtf(dx * dx + dz * dz) * (16.0f / 4000.0f);
            colour = (coopHudPlayerColour(m.player) & ~0xFF) | 0xF0;
            if (r >= 16.0f)
            {
                r = 16.0f;
                colour = (colour & ~0xFF) | 0x80;
            }
            if ((m.age / 8) % 2 == 1 && m.age < 64)
            {
                continue;   /* blinks for its first second */
            }
            px = (s32) (sinf(ang * 0.017453292f) * r) + left;
            py = (s32) (cosf(ang * 0.017453292f) * r) + top;
            gdl = microcode_constructor_related_to_menus(gdl, px - 3, py - 3, px + 3, py + 3, edge);
            gdl = microcode_constructor_related_to_menus(gdl, px - 2, py - 2, px + 2, py + 2, colour);
            gdl = microcode_constructor_related_to_menus(gdl, px - 1, py - 1, px + 1, py + 1, edge);
        }
    }
    for (i = 0; coopHudPartnerAt(i, p); i++)
    {
        if (p->dead && !p->revivable)
        {
            continue;
        }
        colour = (coopHudPlayerColour(p->player) & ~0xFF) | 0xE0;
        if (p->dead)
        {
            colour = 0xFF3030E0;   /* down where they fell: go and get them */
        }
        dx = p->pos.f[0] - self->pos.f[0];
        dz = p->pos.f[2] - self->pos.f[2];
        ang = ((atan2f(dx, dz) * 180.0f) / M_PI_F) + g_CurrentPlayer->vv_theta + 180.0f;
        r = sqrtf(dx * dx + dz * dz) * (16.0f / 4000.0f);
        if (r >= 16.0f)
        {
            r = 16.0f;   /* out of range: on the rim, fainter */
            colour = (colour & ~0xFF) | 0x60;
        }
        px = (s32) (sinf(ang * 0.017453292f) * r) + left;
        py = (s32) (cosf(ang * 0.017453292f) * r) + top;
        gdl = microcode_constructor_related_to_menus(gdl, px - 2, py - 2, px + 2, py + 2, 0x40);
        gdl = microcode_constructor_related_to_menus(gdl, px - 1, py - 1, px + 1, py + 1, colour);
    }
    gdl = portHudScaleEnd(gdl);
    if (hudLayoutWide())
    {
        PORT_HUD_ANCHOR_OFF(gdl);
    }
    return combiner_bayer_lod_perspective(gdl);
}

/* the partner's name with health and armour bars, top left */
static Gfx *coopHudPartnerStatus(Gfx *gdl, CoopHudPartner *p, s32 row)
{
    char name[40];
    char text[48];
    s32 x = viGetViewLeft() + 0x14;
    s32 y = viGetViewTop() + 0x0E + row * 0x18;   /* a block per partner, down the left */

    if (hudAnchorsToEdges())
    {
        PORT_HUD_ANCHOR(gdl, PORT_HUD_ANCHOR_LEFT);
    }
    gdl = portHudScaleBegin(gdl, (f32) viGetViewLeft(), (f32) viGetViewTop());
    gdl = microcode_constructor(gdl);
    coopHudText(name, sizeof(name), p->name);
    coopHudFit(name, 0x70);
    gdl = coopHudPrint(gdl, x, y, name, p->dead ? COOP_HUD_COL_DIM : COOP_HUD_COL_TEXT);
    y += 0x0B;
    if (coopHudPlayerLost(p->player))
    {
        snprintf(text, sizeof(text), "CONNECTION LOST\n");
        gdl = coopHudPrint(gdl, x, y, text, COOP_HUD_COL_DIM);
    }
    else if (p->dead)
    {
        s32 secs = (p->respawn + 59) / 60;

        if (p->revivable && coopHudDownRule() == COOPDOWN_REVIVE)
        {
            snprintf(text, sizeof(text), "DOWN - REVIVE THEM %d\n", (int) secs);   /* before they bleed out */
        }
        else if (p->revivable && p->respawn >= 0)
        {
            snprintf(text, sizeof(text), "DOWN - REVIVE OR %d\n", (int) secs);
        }
        else if (p->revivable)
        {
            snprintf(text, sizeof(text), "DOWN - REVIVE THEM\n");
        }
        else if (p->respawn >= 0)
        {
            snprintf(text, sizeof(text), "DOWN - BACK IN %d\n", (int) secs);
        }
        else if (coopHudDownRule() == COOPDOWN_REVIVE)
        {
            snprintf(text, sizeof(text), "OUT\n");   /* bled out */
        }
        else
        {
            snprintf(text, sizeof(text), "DOWN\n");
        }
        gdl = coopHudPrint(gdl, x, y, text, COOP_HUD_COL_FAIL);
    }
    else
    {
        gdl = coopHudBar(gdl, x, y + 1, 0x40, p->health, 0xD02020E0);
        if (p->armour > 0.0f)
        {
            gdl = coopHudBar(gdl, x, y + 6, 0x40, p->armour, 0x4080FFE0);
        }
    }
    gdl = combiner_bayer_lod_perspective(gdl);
    gdl = portHudScaleEnd(gdl);
    if (hudAnchorsToEdges())
    {
        PORT_HUD_ANCHOR_OFF(gdl);
    }
    return gdl;
}

/* our own wait, centred low on the screen, and a revive's progress under it */
static Gfx *coopHudRespawn(Gfx *gdl, s32 eta)
{
    char text[56];
    s32 th = 0, tw = 0;
    s32 x, y;
    s32 hold = coopHudReviveHold();
    s32 secs = (eta + 59) / 60;

    if (coopHudBledOut())
    {
        snprintf(text, sizeof(text), "BLED OUT - OUT UNTIL THE MISSION ENDS\n");
    }
    else if (eta < 0)
    {
        return gdl;
    }
    else if (hold > 0)
    {
        snprintf(text, sizeof(text), "BEING REVIVED\n");
    }
    else if (coopHudDownRule() == COOPDOWN_REVIVE)
    {
        snprintf(text, sizeof(text), "WAIT FOR A REVIVE - BLEEDING OUT IN %d\n", (int) secs);
    }
    else if (coopHudDownRule() == COOPDOWN_BOTH)
    {
        snprintf(text, sizeof(text), "WAIT FOR A REVIVE OR BACK IN %d\n", (int) secs);
    }
    else
    {
        snprintf(text, sizeof(text), "BACK BESIDE YOUR PARTNER IN %d\n", (int) secs);
    }
    textMeasure(&th, &tw, text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    x = viGetViewLeft() + (viGetViewWidth() - tw) / 2;
    y = viGetViewTop() + viGetViewHeight() * 2 / 3;
    gdl = microcode_constructor(gdl);
    gdl = coopHudPrint(gdl, x, y, text, coopHudBledOut() ? COOP_HUD_COL_FAIL : COOP_HUD_COL_TEXT);
    if (hold > 0)
    {
        gdl = coopHudBar(gdl, viGetViewLeft() + (viGetViewWidth() - 0x50) / 2, y + th + 3, 0x50,
                         (f32) hold / (f32) COOP_REVIVE_HOLD, COOP_HUD_COL_DONE);
    }
    return combiner_bayer_lod_perspective(gdl);
}

/* whose eyes we look out of while we are down, centred near the top */
static Gfx *coopHudSpectating(Gfx *gdl, s32 who)
{
    char name[40];
    char text[72];
    s32 th = 0, tw = 0;
    s32 x, y;
    s32 p, others = 0;

    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        others += (p != who && p != coopExtSelf() && coopLobbyPlayerHere(p));
    }
    coopHudText(name, sizeof(name), coopPlayerName(who));   /* with "\n" */
    coopHudFit(name, viGetViewWidth() / 2);
    name[strlen(name) - 1] = '\0';
    snprintf(text, sizeof(text), others ? "WATCHING %s - FIRE FOR NEXT\n" : "WATCHING %s\n", name);
    textMeasure(&th, &tw, text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    x = viGetViewLeft() + (viGetViewWidth() - tw) / 2;
    y = viGetViewTop() + 0x18;
    gdl = microcode_constructor(gdl);
    gdl = coopHudPrint(gdl, x, y, text, COOP_HUD_COL_TAG);
    return combiner_bayer_lod_perspective(gdl);
}

/* the session banner (coop.c coopHudSessionBanner): two centred lines a
 * quarter of the way down, the first in red */
static Gfx *coopHudBanner(Gfx *gdl)
{
    char l1[56], l2[56], t1[64], t2[64];
    s32 th = 0, tw = 0, w2 = 0;
    s32 x, y;

    if (!coopHudSessionBanner(l1, l2, sizeof(l1)))
    {
        return gdl;
    }
    coopHudText(t1, sizeof(t1), l1);
    coopHudText(t2, sizeof(t2), l2);
    gdl = microcode_constructor(gdl);
    y = viGetViewTop() + viGetViewHeight() * 3 / 8;
    if (l2[0] != '\0')
    {
        textMeasure(&th, &w2, t2, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    }
    textMeasure(&th, &tw, t1, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    {   /* a dark band behind it, to read over the objectives and the scene */
        s32 w = (tw > w2 ? tw : w2) + 16;
        s32 bx = viGetViewLeft() + (viGetViewWidth() - w) / 2;

        gdl = microcode_constructor_related_to_menus(gdl, bx, y - 4, bx + w, y + (l2[0] != '\0' ? 2 * th + 7 : th + 4), 0x000000A0);
        gdl = microcode_constructor(gdl);
    }
    x = viGetViewLeft() + (viGetViewWidth() - tw) / 2;
    gdl = coopHudPrint(gdl, x, y, t1, COOP_HUD_COL_FAIL);
    if (l2[0] != '\0')
    {
        textMeasure(&th, &tw, t2, ptrFontBankGothicChars, ptrFontBankGothic, 0);
        x = viGetViewLeft() + (viGetViewWidth() - tw) / 2;
        gdl = coopHudPrint(gdl, x, y + th + 3, t2, COOP_HUD_COL_TEXT);
    }
    return combiner_bayer_lod_perspective(gdl);
}

/* the mission's objectives, right-aligned in the top-right corner */
static Gfx *coopHudObjectives(Gfx *gdl, s32 below)
{
    const s32 right = viGetViewLeft() + viGetViewWidth() - 0x10;
    const s32 maxw = viGetViewWidth() * 2 / 5;
    s32 y = viGetViewTop() + below;
    s32 letter = 0;
    s32 i;

    if (objectiveGetCount() <= 0 || objective_ptrs[0] == NULL)
    {
        return gdl;
    }
    if (hudAnchorsToEdges())
    {
        PORT_HUD_ANCHOR(gdl, PORT_HUD_ANCHOR_RIGHT);
    }
    gdl = portHudScaleBegin(gdl, (f32) (viGetViewLeft() + viGetViewWidth()), (f32) viGetViewTop());
    gdl = microcode_constructor(gdl);
    for (i = 0; i < objectiveGetCount() && i < OBJECTIVES_MAX; i++)
    {
        const char *raw;
        char body[80];
        char text[112];
        const char *mark;
        u32 colour = COOP_HUD_COL_TEXT;
        s32 th = 0, tw = 0;
        OBJECTIVESTATUS st;

        if (objective_ptrs[i] == NULL || get_difficulty_for_objective(i) > lvlGetSelectedDifficulty())
        {
            continue;
        }
        st = get_status_of_objective(i);
        raw = (const char *) get_text_for_objective(i);
        coopHudText(body, sizeof(body), raw);
        body[strlen(body) - 1] = '\0';
        mark = "";
        if (st == OBJECTIVESTATUS_COMPLETE)
        {
            mark = " - DONE";
            colour = COOP_HUD_COL_DONE;
        }
        else if (st == OBJECTIVESTATUS_FAILED)
        {
            mark = " - FAILED";
            colour = COOP_HUD_COL_FAIL;
        }
        snprintf(text, sizeof(text), "%c. %s%s", 'A' + letter, body, mark);
        letter++;
        {   /* word-wrapped, right-aligned, at most three lines */
            const char *w = text;
            s32 lines = 0;

            while (*w != '\0' && lines < 3)
            {
                char line[96];
                s32 n = 0;
                s32 lastspace = -1;

                while (w[n] != '\0' && n < (s32) sizeof(line) - 2)
                {
                    line[n] = w[n];
                    line[n + 1] = '\n';
                    line[n + 2] = '\0';
                    textMeasure(&th, &tw, line, ptrFontBankGothicChars, ptrFontBankGothic, 0);
                    if (tw > maxw && lastspace > 0)
                    {
                        break;
                    }
                    if (w[n] == ' ')
                    {
                        lastspace = n;
                    }
                    n++;
                }
                if (w[n] != '\0' && lastspace > 0)
                {
                    n = lastspace;   /* break at the last space that fit */
                }
                memcpy(line, w, (size_t) n);
                line[n] = '\n';
                line[n + 1] = '\0';
                if (lines == 2 && w[n] != '\0')
                {
                    coopHudFit(line, maxw - 12);
                }
                textMeasure(&th, &tw, line, ptrFontBankGothicChars, ptrFontBankGothic, 0);
                gdl = coopHudPrint(gdl, right - tw, y, line, colour);
                y += th + 1;
                lines++;
                w += n;
                while (*w == ' ')
                {
                    w++;
                }
            }
        }
        y += 2;
    }
    gdl = combiner_bayer_lod_perspective(gdl);
    gdl = portHudScaleEnd(gdl);
    if (hudAnchorsToEdges())
    {
        PORT_HUD_ANCHOR_OFF(gdl);
    }
    return gdl;
}

Gfx *coopHudRender(Gfx *gdl)
{
    CoopHudPartner p;
    s32 partner;
    s32 eta;
    s32 i;

    if (getPlayerCount() != 1 || g_CurrentPlayer->watch_animation_state != 0)
    {
        return gdl;   /* solo missions only, and not over the watch */
    }
    gdl = coopHudBanner(gdl);
    memset(&p, 0, sizeof(p));
    partner = coopHudPartner(&p);
    if (partner)
    {
        for (i = 0; coopHudPartnerAt(i, &p); i++)
        {
            if (p.player != coopSpectateTarget())
            {
                gdl = coopHudNameTag(gdl, &p);   /* not over the head we look out of */
            }
            if (!weapmenuIsOpen())
            {
                gdl = coopHudPartnerStatus(gdl, &p, i);   /* the menu draws there */
            }
        }
    }
    {
        CoopPingMark m;

        for (i = 0; coopPingAt(i, &m); i++)
        {
            gdl = coopHudPing(gdl, &m);
        }
    }
    if (partner)
    {
        gdl = coopHudRadar(gdl);
    }
    if (coopSpectateTarget() >= 0)
    {
        gdl = coopHudSpectating(gdl, coopSpectateTarget());
    }
    eta = coopHudRespawnEta();
    if (eta >= 0 || coopHudBledOut())
    {
        gdl = coopHudRespawn(gdl, eta);
    }
    if (portObjectivesHudShown() && !g_CurrentPlayer->bonddead && !weapmenuIsOpen())
    {
        gdl = coopHudObjectives(gdl, partner ? COOP_RADAR_TOP + 0x16 : 0x10);
    }
    gdl = coopFeedRender(gdl);   /* chat lines and toasts (coop_chat.c) */
    gdl = weapmenuRender(gdl);
    return gdl;
}
