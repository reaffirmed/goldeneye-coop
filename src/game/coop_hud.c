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

/* the partner's name and distance over their head, when they are in front of us */
static Gfx *coopHudNameTag(Gfx *gdl, CoopHudPartner *p)
{
    PropRecord *self = g_CurrentPlayer->prop;
    coord3d v;
    coord2d s;
    char name[40];
    char text[56];
    f32 dx, dy, dz, dist;
    s32 th = 0, tw = 0;
    s32 x, y;

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
    if (dist < 150.0f && !p->dead)
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
    coopHudText(name, sizeof(name), p->name);
    name[strlen(name) - 1] = '\0';
    if (p->dead)
    {
        snprintf(text, sizeof(text), "REVIVE %s %dM\n", name, (int) (dist / 100.0f + 0.5f));
    }
    else
    {
        snprintf(text, sizeof(text), "%s %dM\n", name, (int) (dist / 100.0f + 0.5f));
    }
    textMeasure(&th, &tw, text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    x = (s32) s.f[0] - tw / 2;
    y = (s32) s.f[1] - th;
    if (x < viGetViewLeft() + 2 || x + tw > viGetViewLeft() + viGetViewWidth() - 2
        || y < viGetViewTop() + 2 || y + th > viGetViewTop() + viGetViewHeight() - 2)
    {
        return gdl;
    }
    gdl = microcode_constructor(gdl);
    gdl = coopHudPrint(gdl, x, y, text, p->dead ? COOP_HUD_COL_FAIL : coopHudPlayerColour(p->player));
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
    if (p->dead)
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
    memset(&p, 0, sizeof(p));
    partner = coopHudPartner(&p);
    if (partner)
    {
        for (i = 0; coopHudPartnerAt(i, &p); i++)
        {
            gdl = coopHudNameTag(gdl, &p);
            if (!weapmenuIsOpen())
            {
                gdl = coopHudPartnerStatus(gdl, &p, i);   /* the menu draws there */
            }
        }
        gdl = coopHudRadar(gdl);
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
    gdl = weapmenuRender(gdl);
    return gdl;
}
