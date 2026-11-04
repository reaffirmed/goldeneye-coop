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
 *     REVIVE tag over where they fell; beside them, "HOLD USE TO REVIVE"
 *     and the revive's progress while we hold it (coop_revive.c)
 *   - under the Revive rule, how long we have before we bleed out, as a
 *     draining bar (the lobby's Bleed Out time)
 *   - with the lobby's Lives set, our lives left while we are down, and OUT
 *     for a partner who has none left
 *   - partners' pings (coop_ping.c): a diamond in the pinger's colour where
 *     they pinged, with their name and the distance, held to the edge of the
 *     screen when it is off it, and a square on the radar
 *   - a partner whose connection dropped: LOST on their tag and status
 *     (the host not answering and a new host taking over are toasts at the
 *     bottom left, coop_chat.c)
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
#include "chr.h"
#include "coop.h"
#include "bg.h"
#include "stan.h"
#include "port_lang.h"   /* portTr: the port's own text in the player's language */

extern Gfx *portHudScaleBegin(Gfx *gdl, f32 x, f32 y);
extern Gfx *portHudScaleEnd(Gfx *gdl);
extern s32 portObjectivesHudShown(void);
extern void portConLogf(const char *fmt, ...);
extern s32 portVideoShowFps(void);          /* video.c: [display] show_fps */    /* video.c: [display] objectives_hud and its key */

#define COOP_HUD_COL_TEXT   0xFFFFFFFF
#define COOP_HUD_COL_DIM    0xB0B0B0C0
#define COOP_HUD_COL_DONE   0x60FF60D0
#define COOP_HUD_COL_FAIL   0xFF4040FF
#define COOP_HUD_COL_WARN   0xF0C850FF  /* a fair line (coopNetLink) */
#define COOP_HUD_COL_SHADOW 0x000000A0

/* each player's colour (by player number) for their tag and radar dot, alpha 0xFF */
static u32 coopHudPlayerColour(s32 player)
{
    return coopPlayerColour(player);    /* coop_access.c: the colour-blind set when it is on */
}

/* coop_screens.c: a player's colour */
u32 coopHudColour(s32 player)
{
    return coopHudPlayerColour(player);
}

/* the radar's centre, as radar.c lays it out for one player */
#define COOP_RADAR_TOP   0x1a
#define COOP_RADAR_RIGHT 0x29

/* the small font in capitals, ASCII only (a byte >= 0x80 starts a two-byte glyph) */
static void coopHudText(char *out, s32 cap, const char *in)
{
    s32 n;

    coopAsciiName(out, cap - 1, in, 1);
    n = (s32) strlen(out);
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

/* the colour's RGB times k (0..1), alpha kept */
static u32 coopHudShade(u32 colour, f32 k)
{
    u32 r = (u32) ((f32) ((colour >> 24) & 0xFF) * k + 0.5f);
    u32 g = (u32) ((f32) ((colour >> 16) & 0xFF) * k + 0.5f);
    u32 b = (u32) ((f32) ((colour >> 8) & 0xFF) * k + 0.5f);

    return (r << 24) | (g << 16) | (b << 8) | (colour & 0xFF);
}

static u32 coopHudNowMs(void)
{
    return (u32) (osGetTime() / 46875ull);
}

/* A partner behind a wall: their tag goes darker and fainter, so it reads as
 * "over there, out of sight". Looked up every COOP_TAG_LOOK_MS per partner:
 * hidden when their room wasn't drawn this frame (a wall or a shut door
 * between), or when the level's walls cut both lines from our eyes to their
 * eyes and to their chest. The dim eases in and out (COOP_TAG_DIM_MS) so a
 * door frame doesn't make it flicker. */
#define COOP_TAG_LOOK_MS   100
#define COOP_TAG_DIM_MS    120.0f
#define COOP_TAG_DIM_ALPHA 0.45f    /* hidden: this much of the ink */
#define COOP_TAG_DIM_SHADE 0.6f     /* and the colour this dark */

static struct
{
    u32 looked;     /* wall clock of the last look */
    u32 ms;         /* of the last ease */
    s32 hidden;
    f32 dim;        /* 0 in sight .. 1 hidden, eased */
} sTagSight[COOP_MAX_PLAYERS];

/* 1 when the level's walls cut the line from `from` to `to` in a room on screen
 * (or ours). The test can report a wall past `to` along the same line, so a
 * hit counts only short of it. */
static s32 coopHudWallBetween(coord3d *from, coord3d *to, s32 *rooms, s32 n)
{
    f32 wscale = get_room_data_float2();
    f32 dx = to->f[0] - from->f[0], dy = to->f[1] - from->f[1], dz = to->f[2] - from->f[2];
    f32 len2 = dx * dx + dy * dy + dz * dz;
    s32 i;

    for (i = 0; i < n; i++)
    {
        HitThing hit;

        if (rooms[i] > 0 && rooms[i] < 256 && bgTestBulletHitBackground(from, to, rooms[i], &hit))
        {
            f32 hx = hit.hitpos.f[0] * wscale - from->f[0];
            f32 hy = hit.hitpos.f[1] * wscale - from->f[1];
            f32 hz = hit.hitpos.f[2] * wscale - from->f[2];

            if (hx * hx + hy * hy + hz * hz < len2)
            {
                return 1;
            }
        }
    }
    return 0;
}

static s32 coopHudPartnerHidden(CoopHudPartner *p)
{
    CoopExtView view;
    PropRecord *self = g_CurrentPlayer->prop;
    s32 rooms[MAXROOMCOUNT];
    coord3d eye, head, chest;
    s32 n, i, room, drawn = 0;

    if (self == NULL || self->stan == NULL || !coopExtRemoteView(p->player, &view) || view.stan == NULL)
    {
        return 0;   /* nothing to go by: in sight */
    }
    room = getTileRoom(view.stan);
    rooms[0] = getTileRoom(self->stan);
    n = 1 + bgCopyDrawnRoomsToList(&rooms[1], MAXROOMCOUNT - 1);
    for (i = 0; i < n && !drawn; i++)
    {
        drawn = (rooms[i] == room);
    }
    if (!drawn)
    {
        return 1;
    }
    eye = *bondviewGetCurrentPlayersPosition();
    head = p->pos;
    chest = p->pos;
    chest.f[1] -= 60.0f;
    return coopHudWallBetween(&eye, &head, rooms, n) && coopHudWallBetween(&eye, &chest, rooms, n);
}

/* how dim partner p's tag is now, 0 .. 1 */
static f32 coopHudTagDim(CoopHudPartner *p)
{
    u32 now = coopHudNowMs();
    s32 k = p->player;
    f32 t;

    if (k < 0 || k >= COOP_MAX_PLAYERS)
    {
        return 0.0f;
    }
    if (now - sTagSight[k].looked >= COOP_TAG_LOOK_MS || now - sTagSight[k].ms > 500u)
    {
        s32 hidden = coopHudPartnerHidden(p);

        if (now - sTagSight[k].ms > 500u)
        {
            sTagSight[k].dim = hidden ? 1.0f : 0.0f;   /* not drawn for a while: no easing from stale */
        }
        if (hidden != sTagSight[k].hidden)
        {
            portConLogf("ge: coop: hud: player %d's tag %s\n", k + 1, hidden ? "dimmed: behind a wall" : "bright: in sight");
        }
        sTagSight[k].hidden = hidden;
        sTagSight[k].looked = now;
    }
    t = (f32) (now - sTagSight[k].ms) / COOP_TAG_DIM_MS;
    sTagSight[k].dim += ((sTagSight[k].hidden ? 1.0f : 0.0f) - sTagSight[k].dim) * ((t > 1.0f) ? 1.0f : t);
    sTagSight[k].ms = now;
    return sTagSight[k].dim;
}

/* A speaker, 10 x 7 with its top left at (x, y): the box and cone, and while
 * `waves` the sound coming off it, the outer wave blinking. Voice chat's
 * marker (coop_voice.c); a muted partner's is dim with a red bar through it. */
static Gfx *coopHudSpeaker(Gfx *gdl, s32 x, s32 y, u32 colour, s32 waves, s32 muted)
{
    s32 k;

    if (x - 1 < 0 || y < 0)
    {   /* gDPFillRectangle keeps 10 bits: a negative edge wraps and the rect runs across the screen */
        return gdl;
    }
    if (muted)
    {
        colour = COOP_HUD_COL_DIM;
    }
    gdl = microcode_constructor_related_to_menus(gdl, x, y + 2, x + 2, y + 5, colour);
    for (k = 0; k < 3; k++)
    {   /* the cone widens away from the box */
        gdl = microcode_constructor_related_to_menus(gdl, x + 2 + k, y + 2 - k, x + 3 + k, y + 5 + k, colour);
    }
    if (waves)
    {
        gdl = microcode_constructor_related_to_menus(gdl, x + 6, y + 2, x + 7, y + 5, colour);
        if ((osGetTime() / 46875ull / 250ull) & 1)
        {
            gdl = microcode_constructor_related_to_menus(gdl, x + 8, y + 1, x + 9, y + 6, colour);
        }
    }
    if (muted)
    {
        gdl = microcode_constructor_related_to_menus(gdl, x - 1, y + 3, x + 10, y + 4, COOP_HUD_COL_FAIL);
    }
    return gdl;
}

/* a smarter guard's callout over its head (coop_ai.c barks), when it is in
 * front of us and within 40 m; sized like a name tag, a grenade warning red */
static Gfx *coopHudBark(Gfx *gdl, CoopBarkMark *m)
{
    PropRecord *self = g_CurrentPlayer->prop;
    ChrRecord *chr = chrFindByLiteralId(m->chrnum);
    coord3d v;
    coord2d s;
    const char *text;
    f32 dx, dy, dz, dist, k, scale;
    s32 th = 0, tw = 0;
    s32 x, y, px, py;

    if (self == NULL || chr == NULL || chr->prop == NULL || chr->model == NULL)
    {
        return gdl;
    }
    v.f[0] = chr->prop->pos.f[0];
    v.f[1] = chr->ground + chr->chrheight - 5.0f;   /* just over the head */
    v.f[2] = chr->prop->pos.f[2];
    dx = v.f[0] - self->pos.f[0];
    dy = v.f[1] - self->pos.f[1];
    dz = v.f[2] - self->pos.f[2];
    dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist > 4000.0f)
    {
        return gdl;
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
    text = coopAiBarkText(m->kind);
    textMeasure(&th, &tw, (char *) text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
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
        gDPNoOpTag(gdl++, 0x5A000000u | (u32) (scale * coopTextScale() * 256.0f + 0.5f));   /* times Text Size */
        gDPNoOpTag(gdl++, 0x58000000u | ((u32) x4 << 12) | (u32) y4);
    }
    gdl = textRenderOutlined(gdl, &x, &y, (char *) text, ptrFontBankGothicChars, ptrFontBankGothic,
                             coopHudWithAlpha(m->kind == COOPBARK_GRENADE ? COOP_HUD_COL_FAIL : COOP_HUD_COL_WARN, m->alpha),
                             coopHudWithAlpha(COOP_HUD_COL_SHADOW, m->alpha), (s32) viGetX(), viGetY(), 0, 0);
    gDPNoOpTag(gdl++, 0x58FFFFFFu);
    return combiner_bayer_lod_perspective(gdl);
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
    u32 ink;

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
        snprintf(text, sizeof(text), portTr("%s - LOST\n"), name);
    }
    else if (p->dead && p->respawn >= 0)
    {   /* the seconds they have left: to bleed out, or until they are back anyway */
        snprintf(text, sizeof(text), portTr("REVIVE %s %dM %dS\n"), name, (int) (dist / 100.0f + 0.5f),
                 (int) ((p->respawn + 59) / 60));
    }
    else if (p->dead)
    {
        snprintf(text, sizeof(text), portTr("REVIVE %s %dM\n"), name, (int) (dist / 100.0f + 0.5f));
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
    ink = p->dead ? COOP_HUD_COL_FAIL : coopHudPlayerColour(p->player);
    if (p->dead)
    {   /* a REVIVE tag pulses, faster as the time to bleed out runs low; steady while someone revives them */
        if (p->revivehold <= 0 && p->respawn >= 0 && coopHudDownRule() == COOPDOWN_REVIVE)
        {
            f32 hz = (p->respawn <= 5 * 60) ? 4.0f : (p->respawn <= 10 * 60) ? 2.0f : 1.0f;
            f32 w = sinf((f32) (coopHudNowMs() % 4000u) * 0.001f * hz * 6.2831855f);

            alpha = 0.7f + 0.3f * w;
        }
    }
    else
    {   /* behind a wall: darker and fainter */
        f32 dim = coopHudTagDim(p);

        ink = coopHudShade(ink, 1.0f - dim * (1.0f - COOP_TAG_DIM_SHADE));
        alpha *= 1.0f - dim * (1.0f - COOP_TAG_DIM_ALPHA);
    }
    gdl = microcode_constructor(gdl);
    {
        s32 x4 = px * 4, y4 = py * 4;

        x4 = (x4 < 0) ? 0 : (x4 > 0xFFE) ? 0xFFE : x4;
        y4 = (y4 < 0) ? 0 : (y4 > 0xFFE) ? 0xFFE : y4;
        gDPNoOpTag(gdl++, 0x5A000000u | (u32) (scale * coopTextScale() * 256.0f + 0.5f));   /* times Text Size */
        gDPNoOpTag(gdl++, 0x58000000u | ((u32) x4 << 12) | (u32) y4);
    }
    gdl = textRenderOutlined(gdl, &x, &y, text, ptrFontBankGothicChars, ptrFontBankGothic,
                             coopHudWithAlpha(ink, alpha),
                             coopHudWithAlpha(COOP_HUD_COL_SHADOW, alpha), (s32) viGetX(), viGetY(), 0, 0);
    gDPNoOpTag(gdl++, 0x58FFFFFFu);
    gdl = combiner_bayer_lod_perspective(gdl);
    if (coopVoiceTalking(p->player))
    {   /* talking: a speaker just left of the tag, unscaled (right of it at the screen's left edge) */
        s32 sx = px - (s32) (tw * scale) / 2 - 13;

        if (sx - 1 < viGetViewLeft())
        {
            sx = px + (s32) (tw * scale) / 2 + 3;
        }
        gdl = microcode_constructor(gdl);
        gdl = coopHudSpeaker(gdl, sx, py - (s32) (th * scale) / 2 - 4, coopHudWithAlpha(ink, alpha), 1, 0);
        gdl = combiner_bayer_lod_perspective(gdl);
    }
    return gdl;
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
        snprintf(text, sizeof(text), portTr("ENEMY %dM\n"), (int) (dist / 100.0f + 0.5f));
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
        gDPNoOpTag(gdl++, 0x5A000000u | (u32) (scale * coopTextScale() * 256.0f + 0.5f));   /* times Text Size */
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

/* A hit on a partner: the bar they lost stays as a pale piece behind what is
 * left for COOP_HIT_HOLD_MS, then shrinks into it, and the bar's frame flashes
 * red for COOP_HIT_FLASH_MS, so a partner in trouble catches the eye. */
#define COOP_HIT_HOLD_MS  450u
#define COOP_HIT_SHRINK_MS 250.0f   /* time constant of the shrink */
#define COOP_HIT_FLASH_MS 300u

typedef struct
{
    f32 last;       /* what the bar showed last frame, -1 none yet */
    f32 ghost;      /* the pale piece reaches to here */
    u32 hit;        /* wall clock of the last drop */
    u32 ms;         /* of the last frame */
} CoopHitBar;

static CoopHitBar sHitBar[COOP_MAX_PLAYERS][2];   /* health, armour */

static void coopHudHitTrack(CoopHitBar *b, f32 now_frac, u32 now)
{
    if (b->last < 0.0f || now - b->ms > 1000u || now_frac > b->last + 0.001f)
    {   /* the first frame, back after a while, or healed: nothing to show */
        b->ghost = now_frac;
    }
    else if (now_frac < b->last - 0.001f)
    {
        if (b->ghost < b->last)
        {
            b->ghost = b->last;
        }
        b->hit = now;
    }
    if (b->ghost > now_frac && now - b->hit > COOP_HIT_HOLD_MS)
    {
        f32 t = (f32) (now - b->ms) / COOP_HIT_SHRINK_MS;

        b->ghost += (now_frac - b->ghost) * ((t > 1.0f) ? 1.0f : t);
        if (b->ghost - now_frac < 0.004f)
        {
            b->ghost = now_frac;
        }
    }
    b->last = now_frac;
    b->ms = now;
}

/* a bar as coopHudBar draws it, with the hit's pale piece and the frame's flash */
static Gfx *coopHudHitBar(Gfx *gdl, s32 x, s32 y, s32 w, f32 frac, u32 colour, CoopHitBar *b)
{
    u32 now = coopHudNowMs();
    s32 fill, ghost;

    frac = (frac < 0.0f) ? 0.0f : (frac > 1.0f) ? 1.0f : frac;
    coopHudHitTrack(b, frac, now);
    if (now - b->hit < COOP_HIT_FLASH_MS)
    {
        f32 a = 1.0f - (f32) (now - b->hit) / (f32) COOP_HIT_FLASH_MS;

        gdl = microcode_constructor_related_to_menus(gdl, x - 2, y - 2, x + w + 2, y + 4, coopHudWithAlpha(0xFF3030FF, a));
    }
    gdl = coopHudBar(gdl, x, y, w, frac, colour);
    fill = (s32) (frac * (f32) w + 0.5f);
    ghost = (s32) (b->ghost * (f32) w + 0.5f);
    if (ghost > fill)
    {
        gdl = microcode_constructor_related_to_menus(gdl, x + fill, y, x + ghost, y + 2, 0xFFF0E0D0);
    }
    return gdl;
}

/* the partner's name with health and armour bars, top left */
static Gfx *coopHudPartnerStatus(Gfx *gdl, CoopHudPartner *p, s32 row)
{
    char name[40];
    char text[48];
    s32 x = viGetViewLeft() + 0x14;
    s32 y = viGetViewTop() + 0x0E + row * 0x18;   /* a block per partner, down the left */

    if (portVideoShowFps())
    {
        y += 0x0A;  /* below the FPS counter (bondview2.c), which sits over the name's first letters */
    }

    if (hudAnchorsToEdges())
    {
        PORT_HUD_ANCHOR(gdl, PORT_HUD_ANCHOR_LEFT);
    }
    gdl = coopTextScaleBegin(gdl, (f32) viGetViewLeft(), (f32) viGetViewTop());   /* HUD Scale and Text Size */
    gdl = microcode_constructor(gdl);
    coopHudText(name, sizeof(name), p->name);
    coopHudFit(name, 0x70);
    gdl = coopHudPrint(gdl, x, y, name, p->dead ? COOP_HUD_COL_DIM : COOP_HUD_COL_TEXT);
    {
        s32 th = 0, tw = 0;
        CoopLink l;

        textMeasure(&th, &tw, name, ptrFontBankGothicChars, ptrFontBankGothic, 0);
        tw += 4;
        if (coopVoiceTalking(p->player) || coopVoiceMuted(p->player))
        {   /* voice chat: talking now, or muted on our game */
            gdl = coopHudSpeaker(gdl, x + tw, y + 1, coopHudPlayerColour(p->player), coopVoiceTalking(p->player),
                                 coopVoiceMuted(p->player));
            tw += 14;
        }
        if (!coopHudPlayerLost(p->player) && coopNetLink(p->player, &l) && l.quality >= COOPLINK_FAIR)
        {   /* a slow or lossy line (coopNetLink): its round trip after the name, gold or red */
            if (l.loss >= COOP_LINK_FAIR_LOSS)
            {
                snprintf(text, sizeof(text), "%dMS %d%%\n", (int) l.rtt, (int) l.loss);
            }
            else
            {
                snprintf(text, sizeof(text), "%dMS\n", (int) l.rtt);
            }
            gdl = coopHudPrint(gdl, x + tw, y, text, l.quality == COOPLINK_POOR ? COOP_HUD_COL_FAIL : COOP_HUD_COL_WARN);
        }
    }
    y += 0x0B;
    if (coopHudPlayerLost(p->player))
    {
        snprintf(text, sizeof(text), "CONNECTION LOST\n");
        gdl = coopHudPrint(gdl, x, y, text, COOP_HUD_COL_DIM);
    }
    else if (p->dead)
    {
        s32 secs = (p->respawn + 59) / 60;

        if (p->out)
        {
            snprintf(text, sizeof(text), "OUT\n");   /* no lives left, or bled out */
        }
        else if (p->revivable && coopHudDownRule() == COOPDOWN_REVIVE)
        {
            snprintf(text, sizeof(text), portTr("DOWN - REVIVE THEM %d\n"), (int) secs);   /* before they bleed out */
        }
        else if (p->revivable && p->respawn >= 0)
        {
            snprintf(text, sizeof(text), portTr("DOWN - REVIVE OR %d\n"), (int) secs);
        }
        else if (p->revivable)
        {
            snprintf(text, sizeof(text), "DOWN - REVIVE THEM\n");
        }
        else if (p->respawn >= 0)
        {
            snprintf(text, sizeof(text), portTr("DOWN - BACK IN %d\n"), (int) secs);
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
    else if (p->player >= 0 && p->player < COOP_MAX_PLAYERS)
    {
        CoopHitBar *hb = sHitBar[p->player];

        gdl = coopHudHitBar(gdl, x, y + 1, 0x40, p->health, 0xD02020E0, &hb[0]);
        if (p->armour > 0.0f || (hb[1].ghost > 0.0f && coopHudNowMs() - hb[1].ms < 1000u))
        {   /* the armour bar stays while its last piece shrinks away */
            gdl = coopHudHitBar(gdl, x, y + 6, 0x40, p->armour, 0x4080FFE0, &hb[1]);
        }
    }
    gdl = combiner_bayer_lod_perspective(gdl);
    gdl = coopTextScaleEnd(gdl);
    if (hudAnchorsToEdges())
    {
        PORT_HUD_ANCHOR_OFF(gdl);
    }
    return gdl;
}

/* our mic is open (push to talk held, or open mic): a speaker and TALK at the
 * top, in the middle; the waves while Steam is actually sending our words */
static Gfx *coopHudSelfVoice(Gfx *gdl)
{
    char text[8] = "TALK\n";
    s32 th = 0, tw = 0;
    s32 x, y;

    textMeasure(&th, &tw, text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    x = viGetViewLeft() + viGetViewWidth() / 2 - (tw + 14) / 2;
    y = viGetViewTop() + 0x0E;
    gdl = portHudScaleBegin(gdl, (f32) viGetViewLeft(), (f32) viGetViewTop());
    gdl = microcode_constructor(gdl);
    gdl = coopHudSpeaker(gdl, x, y + 1, coopHudPlayerColour(coopExtSelf()), coopVoiceSelfTalking(), 0);
    gdl = coopHudPrint(gdl, x + 14, y, text, COOP_HUD_COL_TEXT);
    gdl = combiner_bayer_lod_perspective(gdl);
    return portHudScaleEnd(gdl);
}

/* our own wait, centred low on the screen, and a revive's progress under it */
static Gfx *coopHudRespawn(Gfx *gdl, s32 eta)
{
    char text[56];
    s32 th = 0, tw = 0;
    s32 x, y;
    s32 hold = coopHudReviveHold();
    s32 secs = (eta + 59) / 60;

    if (coopHudBledOut() == COOPOUT_LIVES)
    {
        snprintf(text, sizeof(text), "NO LIVES LEFT - OUT UNTIL THE MISSION ENDS\n");
    }
    else if (coopHudBledOut())
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
        snprintf(text, sizeof(text), portTr("WAIT FOR A REVIVE - BLEEDING OUT IN %d\n"), (int) secs);
    }
    else if (coopHudDownRule() == COOPDOWN_BOTH)
    {
        snprintf(text, sizeof(text), portTr("WAIT FOR A REVIVE OR BACK IN %d\n"), (int) secs);
    }
    else
    {
        snprintf(text, sizeof(text), portTr("BACK BESIDE YOUR PARTNER IN %d\n"), (int) secs);
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
    else if (coopHudBleedOut() >= 0)
    {   /* the time left to be revived in, draining */
        gdl = coopHudBar(gdl, viGetViewLeft() + (viGetViewWidth() - 0x50) / 2, y + th + 3, 0x50,
                         (f32) coopHudBleedOut() / (f32) coopBleedOutTicks(), 0xD02020E0);
    }
    if (!coopHudBledOut() && coopHudLives() > 0)
    {   /* the lobby's Lives: what is left after this down */
        if (coopHudLives() == 1)
        {
            snprintf(text, sizeof(text), "LAST LIFE\n");
        }
        else
        {
            snprintf(text, sizeof(text), portTr("%d LIVES LEFT\n"), (int) coopHudLives());
        }
        textMeasure(&th, &tw, text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
        gdl = coopHudPrint(gdl, viGetViewLeft() + (viGetViewWidth() - tw) / 2, y + th + 12, text, COOP_HUD_COL_DIM);
    }
    return combiner_bayer_lod_perspective(gdl);
}

/* beside a downed partner: how to revive them, and the revive's progress
 * while we hold Use (coop_revive.c) */
static Gfx *coopHudReviving(Gfx *gdl)
{
    CoopHudPartner p;
    char name[40];
    char text[64];
    s32 th = 0, tw = 0;
    s32 x, y, hold;

    if (!coopReviveHud(&p, &hold))
    {
        return gdl;
    }
    coopHudText(name, sizeof(name), p.name);
    coopHudFit(name, viGetViewWidth() / 2);
    name[strlen(name) - 1] = '\0';
    snprintf(text, sizeof(text), (hold >= 0) ? portTr("REVIVING %s\n") : portTr("HOLD USE TO REVIVE %s\n"), name);
    textMeasure(&th, &tw, text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    x = viGetViewLeft() + (viGetViewWidth() - tw) / 2;
    y = viGetViewTop() + viGetViewHeight() * 2 / 3;
    gdl = microcode_constructor(gdl);
    gdl = coopHudPrint(gdl, x, y, text, (hold >= 0) ? COOP_HUD_COL_DONE : COOP_HUD_COL_TEXT);
    if (hold >= 0)
    {
        gdl = coopHudBar(gdl, viGetViewLeft() + (viGetViewWidth() - 0x50) / 2, y + th + 3, 0x50,
                         (f32) hold / (f32) COOP_REVIVE_HOLD, COOP_HUD_COL_DONE);
    }
    return combiner_bayer_lod_perspective(gdl);
}

/* end a HUD line at its '\n' */
static void coopHudChop(char *text)
{
    for (; *text != '\0' && *text != '\n'; text++)
    {
    }
    *text = '\0';
}

/* what a bind is on the device pressed last, in capitals ("G"), or "" when
 * it is unbound or there is no keyboard (headless) */
static void coopHudBindText(const char *bind, char *out, s32 cap)
{
    char key[40];

    key[0] = '\0';
#ifdef GE_VIDEO
    {
        extern s32 portInputGlyphStyle(void);
        extern s32 portVideoIniGetKey(const char *key, char *buf, s32 n);
        extern s32 portInputDeviceBindText(s32 dev, const char *name, char *buf, s32 len);

        if (!((portInputGlyphStyle() == 0) ? portVideoIniGetKey(bind, key, (s32) sizeof(key))
                                             : portInputDeviceBindText(1, bind, key, (s32) sizeof(key))))
        {
            key[0] = '\0';
        }
    }
#endif
    coopHudChop(key);
    coopHudText(out, cap, key);
    coopHudChop(out);
}

/* Looking at a partner we could give ammo to now (coop_give.c's reach, and
 * spare rounds for the gun in hand): "G: GIVE AMMO TO NAME" on a dark band just
 * under the crosshair, drawn after the toasts so a tall stack of them doesn't
 * cover it. It fades in and out over
 * COOP_GIVE_HINT_MS so the edge of the reach doesn't make it blink. */
#define COOP_GIVE_HINT_MS 150.0f
static Gfx *coopHudGiveHint(Gfx *gdl, s32 show)
{
    static f32 a;
    static u32 ms;
    static char name[40];
    static char key[40];
    CoopHudPartner p;
    char text[96];
    u32 now = coopHudNowMs();
    s32 th = 0, tw = 0;
    s32 x, y;
    f32 t = (f32) (now - ms) / COOP_GIVE_HINT_MS;

    if (show && coopGiveHint(&p))
    {
        coopHudText(name, sizeof(name), p.name);
        coopHudFit(name, viGetViewWidth() / 3);
        coopHudChop(name);
        coopHudBindText("give_partner", key, sizeof(key));
        a += (1.0f - a) * ((t > 1.0f) ? 1.0f : t);
    }
    else
    {
        a -= a * ((t > 1.0f) ? 1.0f : t);
    }
    ms = now;
    if (a < 0.02f || name[0] == '\0')
    {
        return gdl;
    }
    if (key[0] != '\0')
    {
        snprintf(text, sizeof(text), portTr("%s: GIVE AMMO TO %s\n"), key, name);
    }
    else
    {
        snprintf(text, sizeof(text), portTr("GIVE AMMO TO %s\n"), name);
    }
    textMeasure(&th, &tw, text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    x = viGetViewLeft() + (viGetViewWidth() - tw) / 2;
    y = viGetViewTop() + viGetViewHeight() / 2 + 0x14;
    gdl = microcode_constructor(gdl);
    gdl = microcode_constructor_related_to_menus(gdl, x - 3, y - 2, x + tw + 3, y + th, coopHudWithAlpha(0x00000080, a));   /* a band like a toast's */
    gdl = textRenderOutlined(gdl, &x, &y, text, ptrFontBankGothicChars, ptrFontBankGothic,
                             coopHudWithAlpha(0xFFFFFFD0, a), coopHudWithAlpha(COOP_HUD_COL_SHADOW, a),
                             (s32) viGetX(), viGetY(), 0, 0);
    return combiner_bayer_lod_perspective(gdl);
}

/* whose eyes we look out of while we are down (or the free camera), centred
 * near the top, and the keys along the foot */
static Gfx *coopHudSpectating(Gfx *gdl, s32 mode, s32 who)
{
    char name[40];
    char text[72];
    char keys[72];
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
    if (mode == 2)
    {
        snprintf(text, sizeof(text), "FREE CAMERA\n");
        snprintf(keys, sizeof(keys), portTr("FIRE: WATCH %s   USE: BACK\n"), name);
    }
    else
    {
        snprintf(text, sizeof(text), portTr("WATCHING %s\n"), name);
        snprintf(keys, sizeof(keys), others ? portTr("FIRE / AIM: NEXT / BACK   USE: FREE CAMERA%s\n") : portTr("USE: FREE CAMERA%s\n"),
                 coopSpectateEndAsked() ? "   START: END" : "");
    }
    if (coopSpectateEndAsked() == 2)
    {
        snprintf(keys, sizeof(keys), coopHostAbortAsks() ? "START AGAIN: END THE MISSION FOR EVERYONE\n" : "START AGAIN: LEAVE THE MISSION\n");
    }
    textMeasure(&th, &tw, text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    x = viGetViewLeft() + (viGetViewWidth() - tw) / 2;
    y = viGetViewTop() + 0x18;
    gdl = microcode_constructor(gdl);
    gdl = coopHudPrint(gdl, x, y, text, (mode == 2) ? COOP_HUD_COL_TEXT : coopHudPlayerColour(who));
    textMeasure(&th, &tw, keys, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    x = viGetViewLeft() + (viGetViewWidth() - tw) / 2;
    gdl = coopHudPrint(gdl, x, viGetViewTop() + viGetViewHeight() - 0x1C, keys, COOP_HUD_COL_DIM);   /* clear of the partner rows */
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
    gdl = coopTextScaleBegin(gdl, (f32) (viGetViewLeft() + viGetViewWidth()), (f32) viGetViewTop());   /* HUD Scale and Text Size */
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
    gdl = coopTextScaleEnd(gdl);
    if (hudAnchorsToEdges())
    {
        PORT_HUD_ANCHOR_OFF(gdl);
    }
    return gdl;
}

/* in a cutscene, once someone pressed skip: how many want it and who we wait
 * for, bottom right where solo's skip hint goes (coop_vote.c) */
Gfx *coopHudSkipVote(Gfx *gdl)
{
    char text[96];
    char name[40];
    s32 th = 0, tw = 0;
    s32 need, mine, n, i, who, len;

    n = coopSkipVotes(&need, &mine);
    if (n <= 0)
    {
        return gdl;
    }
    len = snprintf(text, sizeof(text), portTr("SKIP %d/%d"), (int) n, (int) need);
    if (!mine)
    {
        len += snprintf(text + len, sizeof(text) - len, "%s", portTr(" - PRESS TO VOTE"));
    }
    else
    {
        for (i = 0; (who = coopSkipWaitingFor(i)) >= 0 && len < (s32) sizeof(text) - 24; i++)
        {
            coopHudText(name, sizeof(name), coopPlayerName(who));
            coopHudFit(name, viGetViewWidth() / 4);
            name[strlen(name) - 1] = '\0';
            len += snprintf(text + len, sizeof(text) - len, (i == 0) ? portTr(" - WAITING FOR %s") : ", %s", name);
        }
    }
    snprintf(text + len, sizeof(text) - len, "\n");
    textMeasure(&th, &tw, text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    gdl = microcode_constructor(gdl);
    gdl = coopHudPrint(gdl, viGetViewLeft() + viGetViewWidth() - tw - 8, viGetViewTop() + viGetViewHeight() - th - 6, text,
                       mine ? COOP_HUD_COL_DONE : COOP_HUD_COL_TEXT);
    return combiner_bayer_lod_perspective(gdl);
}

/* Online versus (coop_versus.c): every PC has two to four players in the
 * match, so the rest of this file, which is about partners, stays off. What
 * isn't: the chat line and toasts, Sound Captions, our own voice and the
 * weapon menu; and a team-mate's ping. Rare's MP HUD has the radar, names
 * and scores. (No hit marker and no arrow toward who hit us: taken out.) */
static Gfx *coopHudVersus(Gfx *gdl)
{
    if (!g_CurrentPlayer->bonddead && !g_CurrentPlayer->mpmenuon)
    {
        CoopPingMark m;
        s32 i;

        for (i = 0; coopPingAt(i, &m); i++)
        {
            gdl = coopHudPing(gdl, &m);
        }
    }
    if (coopVoiceSelfOpen())
    {
        gdl = coopHudSelfVoice(gdl);
    }
    coopCaptionHarness();
    gdl = coopCaptionsRender(gdl);
    gdl = coopFeedRender(gdl);
    gdl = coopChatGridRender(gdl);
    if (!g_CurrentPlayer->mpmenuon)
    {
        gdl = weapmenuRender(gdl);   /* not under Rare's pause menu */
    }
    return gdl;
}

Gfx *coopHudRender(Gfx *gdl)
{
    CoopHudPartner p;
    s32 partner;
    s32 eta;
    s32 givehint = 0;
    s32 i;

    if (coopVersus())
    {
        return coopHudVersus(gdl);   /* one player per PC: the parts that aren't about partners */
    }
    if (getPlayerCount() != 1 || g_CurrentPlayer->watch_animation_state != 0)
    {
        return gdl;   /* solo missions only, and not over the watch */
    }
    if (coopCutsceneActive())
    {
        return coopHudSkipVote(gdl);   /* the partners are in the shot, not tagged */
    }
    {
        CoopBarkMark m;

        for (i = 0; coopAiBarkAt(i, &m); i++)
        {
            gdl = coopHudBark(gdl, &m);
        }
    }
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
    if (partner && coopVoiceSelfOpen())
    {
        gdl = coopHudSelfVoice(gdl);
    }
    {
        s32 who, mode = coopSpectateHud(&who);

        if (mode != 0)
        {
            gdl = coopHudSpectating(gdl, mode, who);
        }
    }
    eta = coopHudRespawnEta();
    if (eta >= 0 || coopHudBledOut())
    {
        gdl = coopHudRespawn(gdl, eta);
    }
    else
    {
        CoopHudPartner rp;
        s32 rh, who;
        s32 reviving = coopReviveHud(&rp, &rh);   /* the revive prompt has that spot */

        gdl = coopHudReviving(gdl);
        givehint = partner && !reviving && !weapmenuIsOpen() && !coopChatTyping() && coopSpectateHud(&who) == 0;
    }
    if (portObjectivesHudShown() && !g_CurrentPlayer->bonddead && !weapmenuIsOpen())
    {
        gdl = coopHudObjectives(gdl, partner ? COOP_RADAR_TOP + 0x16 : 0x10);
    }
    coopCaptionHarness();
    coopFadeHarness();
    gdl = coopCaptionsRender(gdl);   /* Sound Captions (coop_access.c) */
    gdl = coopFeedRender(gdl);   /* chat lines and toasts (coop_chat.c) */
    gdl = coopHudGiveHint(gdl, givehint);   /* over a tall stack of toasts */
    gdl = coopChatGridRender(gdl);   /* a pad player typing on the on-screen keyboard */
    gdl = coopHudSkipVote(gdl);  /* an ending that waits for a press, outside a cinema */
    gdl = weapmenuRender(gdl);
    return gdl;
}
