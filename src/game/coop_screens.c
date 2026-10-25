/**
 * Co-op screens (port only): what a player sees while the game waits on the
 * network, in place of a frozen view or a line of small print.
 *
 *   - the mission card: a co-op mission holds still on every game until all
 *     the players who were in the session have loaded in (coop.c
 *     coopHoldStart). The card covers the view: the mission's name and
 *     difficulty, its objectives, and each player as IN or LOADING.
 *   - reconnecting: the host stopped answering, or a new host is being
 *     reached. The view dims under a panel with who it is, what happens next
 *     and the countdown (the countdown and rules are coop.c's,
 *     coopHudSessionBanner).
 *   - errors: why we are back in the lobby, or why a join did not work. The
 *     lobby shows the newest one as a panel until it is dismissed (A, Start,
 *     B, Enter, Escape or a click); the lobby's own rows wait under it.
 *
 * Drawn from lv.c lvlRender (in a stage) and front.c's lobby.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include "textrelated.h"
#include "front.h"
#include "fr.h"
#include "player.h"
#include "othermodemicrocode.h"
#include "bondview.h"
#include "lv.h"
#include "boss.h"
#include "objective_status.h"
#include "coop.h"
#include "menu_glyphs.h"
#include <joy.h>
#include <snd.h>
#include <music.h>

extern void portConLogf(const char *fmt, ...);
extern char *getenv(const char *);
extern int atoi(const char *);
extern void *memset(void *, int, unsigned int);
extern int strncmp(const char *, const char *, unsigned int);
u8 *get_text_for_objective(int objectiveIndex);
s32 get_difficulty_for_objective(s32 objectiveIndex);
Gfx *frontPrintText(Gfx *gdl, s32 *x, s32 *y, s8 *text, s32 second_font_table, s32 first_font_table, s32 arg6, s32 view_x, s32 view_y, s32 arg9, s32 arga);

#define SCR_COL_TEXT    0xFFFFFFFF
#define SCR_COL_DIM     0x9A9A9AFF
#define SCR_COL_GOLD    0xE8C060FF      /* the briefing folder's brass */
#define SCR_COL_IN      0x70F070FF
#define SCR_COL_ALERT   0xFF5050FF
#define SCR_COL_SHADOW  0x000000A0
#define SCR_CARD_FADE   20              /* ticks the card takes to fade out once everyone is in */

static u32 sFrames;         /* counts rendered frames: the dots and spinners */
static s32 sCardFade;       /* ticks of fade left after the card */
static s32 sCardUp;         /* the card showed last frame */

static Gfx *scrPrint(Gfx *gdl, s32 x, s32 y, const char *text, u32 colour)
{
    return textRenderOutlined(gdl, &x, &y, (char *) text, ptrFontBankGothicChars, ptrFontBankGothic, colour,
                              (SCR_COL_SHADOW & 0xFFFFFF00) | ((colour & 0xFF) * 0xA0 / 0xFF), (s32) viGetX(), viGetY(), 0, 0);
}

static s32 scrWidth(const char *text)
{
    s32 th = 0, tw = 0;

    textMeasure(&th, &tw, (char *) text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    return tw;
}

static u32 scrAlpha(u32 colour, s32 a)
{
    return (colour & 0xFFFFFF00) | (u32) (((colour & 0xFF) * (u32) a) / 0xFF);
}

/* the small font in capitals, ASCII, ending in '\n' */
static void scrText(char *out, s32 cap, const char *in)
{
    s32 n;

    coopAsciiName(out, cap - 1, in, 1);
    n = (s32) strlen(out);
    out[n++] = '\n';
    out[n] = '\0';
}

/* text centred on cx, scaled about its middle (gfx_pc.cpp's 0x5A / 0x58 tags) */
static Gfx *scrPrintBig(Gfx *gdl, s32 cx, s32 y, const char *text, f32 scale, u32 colour)
{
    s32 th = 0, tw = 0;
    s32 x4, y4;

    textMeasure(&th, &tw, (char *) text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    x4 = cx * 4;
    y4 = (y + th / 2) * 4;
    x4 = (x4 < 0) ? 0 : (x4 > 0xFFE) ? 0xFFE : x4;
    y4 = (y4 < 0) ? 0 : (y4 > 0xFFE) ? 0xFFE : y4;
    gDPNoOpTag(gdl++, 0x5A000000u | (u32) (scale * 256.0f + 0.5f));
    gDPNoOpTag(gdl++, 0x58000000u | ((u32) x4 << 12) | (u32) y4);
    gdl = scrPrint(gdl, cx - tw / 2, y, text, colour);
    gDPNoOpTag(gdl++, 0x58FFFFFFu);
    return gdl;
}

/* a thin brass rule with a darker shadow line, like the folders' pencil rules */
static Gfx *scrRule(Gfx *gdl, s32 x1, s32 x2, s32 y, s32 a)
{
    gdl = microcode_constructor_related_to_menus(gdl, x1, y, x2, y + 1, scrAlpha(SCR_COL_GOLD, a));
    return microcode_constructor_related_to_menus(gdl, x1, y + 1, x2, y + 2, scrAlpha(0x000000FF, a));
}

/* three dots that fill in turn, for anything still going */
static const char *scrDots(void)
{
    static const char *const dots[] = { "   ", ".  ", ".. ", "..." };

    return dots[(sFrames / 15) % 4];
}

static const char *scrDifficultyName(s32 d)
{
    switch (d)
    {
        case DIFFICULTY_AGENT:  return "AGENT";
        case DIFFICULTY_SECRET: return "SECRET AGENT";
        case DIFFICULTY_00:     return "00 AGENT";
        case DIFFICULTY_007:    return "007";
        default:                return "";
    }
}

/* ---- the mission card -------------------------------------------------------- */

static Gfx *scrMissionCard(Gfx *gdl, u8 arrived, s32 secs, s32 a)
{
    const s32 left = viGetViewLeft();
    const s32 top = viGetViewTop();
    const s32 w = viGetViewWidth();
    const s32 h = viGetViewHeight();
    const s32 cx = left + w / 2;
    const s32 colw = (w > 0x150) ? 0x150 : w - 0x20;
    const s32 cl = cx - colw / 2;
    const s32 cr = cx + colw / 2;
    const char *mission = coopMissionName(bossGetStageNum());
    char line[112];
    char name[48];
    s32 y = top + h / 7;
    s32 p, in = 0, total = 0;
    s32 letter = 0;
    s32 i;

    /* the whole frame, pillars included, so nothing of the held view shows */
    gdl = microcode_constructor_related_to_menus(gdl, 0, 0, (s32) viGetX(), (s32) viGetY(), scrAlpha(0x000000FF, a));
    gdl = microcode_constructor(gdl);

    gdl = scrPrint(gdl, cl, y - 0x14, "CO-OP MISSION\n", scrAlpha(SCR_COL_GOLD, a));
    scrText(name, sizeof(name), (mission != NULL) ? mission : "");
    gdl = scrPrintBig(gdl, cx, y + 2, name, 2.0f, scrAlpha(SCR_COL_TEXT, a));
    y += 0x1C;
    snprintf(line, sizeof(line), "%s\n", scrDifficultyName(lvlGetSelectedDifficulty()));
    gdl = scrPrint(gdl, cx - scrWidth(line) / 2, y, line, scrAlpha(SCR_COL_GOLD, a));
    y += 0x10;
    gdl = scrRule(gdl, cl, cr, y, a);
    gdl = microcode_constructor(gdl);
    y += 8;

    /* the objectives, as the briefing lists them */
    for (i = 0; i < objectiveGetCount() && i < OBJECTIVES_MAX && letter < 6; i++)
    {
        char body[80];

        if (objective_ptrs[i] == NULL || get_difficulty_for_objective(i) > lvlGetSelectedDifficulty())
        {
            continue;
        }
        scrText(body, sizeof(body), (const char *) get_text_for_objective(i));
        body[strlen(body) - 1] = '\0';
        snprintf(line, sizeof(line), "%c. %s\n", 'A' + letter, body);
        while (scrWidth(line) > colw && strlen(body) > 4)
        {   /* one line each: cut with "..." */
            body[strlen(body) - 1] = '\0';
            snprintf(line, sizeof(line), "%c. %s...\n", 'A' + letter, body);
        }
        letter++;
        gdl = scrPrint(gdl, cl, y, line, scrAlpha(SCR_COL_DIM, a));
        y += 0x0B;
    }
    y += 6;
    gdl = scrRule(gdl, cl, cr, y, a);
    gdl = microcode_constructor(gdl);
    y += 8;

    /* the players: who is in, who is still loading */
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        const s32 here = (p == coopExtSelf()) || coopLobbyPlayerHere(p);
        const s32 isin = (arrived >> p) & 1;

        if (!here)
        {
            continue;
        }
        total++;
        in += isin;
        scrText(name, sizeof(name), coopPlayerName(p));
        gdl = microcode_constructor_related_to_menus(gdl, cl, y + 2, cl + 5, y + 7, scrAlpha(coopHudColour(p), a));
        gdl = microcode_constructor(gdl);
        gdl = scrPrint(gdl, cl + 0x0A, y, name, scrAlpha(SCR_COL_TEXT, a));
        snprintf(line, sizeof(line), isin ? "IN\n" : "LOADING%s\n", scrDots());
        gdl = scrPrint(gdl, isin ? cr - scrWidth(line) : cr - scrWidth("LOADING...\n"), y, line,
                       scrAlpha(isin ? SCR_COL_IN : SCR_COL_DIM, a));
        y += 0x0C;
    }

    /* how far along everyone is, and when the host stops waiting */
    y = top + h - h / 6;
    gdl = microcode_constructor_related_to_menus(gdl, cl, y, cr, y + 3, scrAlpha(0x303030FF, a));
    if (total > 0)
    {
        gdl = microcode_constructor_related_to_menus(gdl, cl, y, cl + (cr - cl) * in / total, y + 3, scrAlpha(SCR_COL_GOLD, a));
    }
    gdl = microcode_constructor(gdl);
    y += 8;
    if (in < total)
    {
        snprintf(line, sizeof(line), "WAITING FOR EVERYONE TO LOAD IN - %d\n", (int) secs);
    }
    else
    {
        snprintf(line, sizeof(line), "EVERYONE IS IN\n");
    }
    gdl = scrPrint(gdl, cx - scrWidth(line) / 2, y, line, scrAlpha((in < total) ? SCR_COL_DIM : SCR_COL_IN, a));
    return combiner_bayer_lod_perspective(gdl);
}

/* ---- reconnecting --------------------------------------------------------------- */

static Gfx *scrReconnect(Gfx *gdl, const char *l1, const char *l2)
{
    const s32 left = viGetViewLeft();
    const s32 top = viGetViewTop();
    const s32 w = viGetViewWidth();
    const s32 h = viGetViewHeight();
    const s32 cx = left + w / 2;
    const s32 pw = (w > 0x150) ? 0x150 : w - 0x10;
    const s32 ph = 0x48;
    const s32 px = cx - pw / 2;
    const s32 py = top + (h - ph) / 2 - 0x10;
    const char *hint;
    char line[80];
    s32 k;

    if (strncmp(l2, "CONNECTING", 10) == 0)
    {
        hint = "THE MISSION CARRIES ON WITH THE NEW HOST";
    }
    else if (coopSetting(COOPSET_HOSTLEAVES) == 1)
    {
        hint = "IF THEY STAY GONE, THE MISSION ENDS";
    }
    else
    {
        hint = "IF THEY STAY GONE, THE NEXT PLAYER HOSTS";
    }

    /* the view stays, dimmed */
    gdl = microcode_constructor_related_to_menus(gdl, 0, 0, (s32) viGetX(), (s32) viGetY(), 0x000000A0);
    gdl = microcode_constructor_related_to_menus(gdl, px, py, px + pw, py + ph, 0x101010E8);
    gdl = microcode_constructor_related_to_menus(gdl, px, py, px + pw, py + 2, SCR_COL_ALERT);
    gdl = microcode_constructor(gdl);

    scrText(line, sizeof(line), l1);
    gdl = scrPrint(gdl, cx - scrWidth(line) / 2, py + 0x09, line, SCR_COL_ALERT);
    scrText(line, sizeof(line), l2);
    gdl = scrPrint(gdl, cx - scrWidth(line) / 2, py + 0x18, line, SCR_COL_TEXT);

    /* a sweep of eight blocks, one lit and moving: still trying */
    for (k = 0; k < 8; k++)
    {
        const s32 bx = cx - 8 * 6 / 2 + k * 6;
        const s32 lit = (s32) ((sFrames / 6) % 8) == k;

        gdl = microcode_constructor_related_to_menus(gdl, bx, py + 0x29, bx + 4, py + 0x2D, lit ? SCR_COL_GOLD : 0x404040FF);
    }
    gdl = microcode_constructor(gdl);
    scrText(line, sizeof(line), hint);
    gdl = scrPrint(gdl, cx - scrWidth(line) / 2, py + 0x35, line, SCR_COL_DIM);
    return combiner_bayer_lod_perspective(gdl);
}

/* lv.c lvlRender, after the views: the card or the reconnecting panel */
Gfx *coopScreensRender(Gfx *gdl)
{
    u8 arrived = 0;
    s32 secs = 0;
    char l1[56], l2[56];

    sFrames++;
    if (bossGetStageNum() == LEVELID_TITLE || getPlayerCount() != 1)
    {
        sCardUp = 0;
        sCardFade = 0;
        return gdl;
    }
    if (coopHoldInfo(&arrived, &secs))
    {
        sCardUp = 1;
        return scrMissionCard(gdl, arrived, secs, 0xFF);
    }
    if (sCardUp)
    {   /* the card just went: fade it off the live view */
        sCardUp = 0;
        sCardFade = SCR_CARD_FADE;
    }
    if (sCardFade > 0)
    {
        u8 all = 0;
        s32 p;

        for (p = 0; p < COOP_MAX_PLAYERS; p++)
        {
            all |= (u8) (((p == coopExtSelf()) || coopLobbyPlayerHere(p)) << p);
        }
        sCardFade--;
        return scrMissionCard(gdl, all, 0, sCardFade * 0xFF / SCR_CARD_FADE);
    }
    if (g_CurrentPlayer != NULL && g_CurrentPlayer->watch_animation_state == 0 && coopHudSessionBanner(l1, l2, sizeof(l1)))
    {
        return scrReconnect(gdl, l1, l2);
    }
    return gdl;
}

/* ---- errors --------------------------------------------------------------------- */

static char sErrTitle[48];
static char sErrText[3][64];
static s32 sErrUp;
static s32 sErrAge;

/* a panel for the lobby: title, and text cut into up to three lines at '\n' */
void coopScreenError(const char *title, const char *text)
{
    s32 i = 0;

    snprintf(sErrTitle, sizeof(sErrTitle), "%s", title);
    memset(sErrText, 0, sizeof(sErrText));
    while (text != NULL && *text != '\0' && i < 3)
    {
        const char *nl = strchr(text, '\n');
        const s32 n = (nl != NULL) ? (s32) (nl - text) : (s32) strlen(text);

        snprintf(sErrText[i], sizeof(sErrText[i]), "%.*s", (int) n, text);
        i++;
        text += n + (nl != NULL);
    }
    sErrUp = 1;
    sErrAge = 0;
    portConLogf("ge: coop: error screen: %s - %s %s %s\n", sErrTitle, sErrText[0], sErrText[1], sErrText[2]);
}

s32 coopScreenErrorUp(void)
{
    return sErrUp;
}

/* front.c's lobby, before its own input: 1 while the panel takes the keys */
s32 coopScreenErrorInput(void)
{
    if (!sErrUp)
    {
        return 0;
    }
    sErrAge++;
    {   /* GE_COOP_ERRSHOT=<ticks>: leave it up that long, then dismiss (harness, for the screenshot) */
        static s32 shot = -2;

        if (shot == -2)
        {
            const char *e = getenv("GE_COOP_ERRSHOT");

            shot = (e != NULL) ? atoi(e) : -1;
        }
        if (shot >= 0 && sErrAge > shot)
        {
            sErrUp = 0;
            return 1;
        }
    }
    if (sErrAge > 20 && joyGetButtonsPressedThisFrame(PLAYER_1, START_BUTTON | A_BUTTON | B_BUTTON | Z_TRIG))
    {   /* not a press left over from the screen before */
        sndPlaySfx(g_musicSfxBufferPtr, DOOR_METAL_CLOSE2_SFX, 0);
        sErrUp = 0;
    }
    return 1;
}

/* the panel over the lobby's paper (menu coordinates, the sheets' 4:3) */
Gfx *coopScreenErrorRender(Gfx *gdl)
{
    const s32 px = 0x30, pw = 0xF0;
    const s32 py = 0x48, ph = 0x5C;
    s32 x, y;
    s32 i;

    if (!sErrUp)
    {
        return gdl;
    }
    gdl = microcode_constructor_related_to_menus(gdl, 0, 0, (s32) viGetX(), (s32) viGetY(), 0x00000070);
    gdl = microcode_constructor_related_to_menus(gdl, px + 3, py + 3, px + pw + 3, py + ph + 3, 0x00000080);
    gdl = microcode_constructor_related_to_menus(gdl, px, py, px + pw, py + ph, 0xEFE6CCFF);    /* the folders' paper */
    gdl = microcode_constructor_related_to_menus(gdl, px, py, px + pw, py + 0x14, 0x8A1414FF);  /* a red CONFIDENTIAL band */
    gdl = microcode_constructor(gdl);
    x = px + 8;
    y = py + 3;
    {
        char t[56];

        snprintf(t, sizeof(t), "%s\n", sErrTitle);
        gdl = frontPrintText(gdl, &x, &y, (s8 *) t, (s32) ptrFontZurichBoldChars, (s32) ptrFontZurichBold, 0xFFFFFFFF, viGetX(), viGetY(), 0, 0);
    }
    for (i = 0; i < 3; i++)
    {
        char t[72];

        if (sErrText[i][0] == '\0')
        {
            continue;
        }
        snprintf(t, sizeof(t), "%s\n", sErrText[i]);
        x = px + 8;
        y = py + 0x1A + i * 0x0F;
        gdl = frontPrintText(gdl, &x, &y, (s8 *) t, (s32) ptrFontZurichBoldChars, (s32) ptrFontZurichBold, 0x000000FF, viGetX(), viGetY(), 0, 0);
    }
    {   /* OK, with the button that says it (menu_glyphs.c) */
        const s32 gw = menuGlyphWidth(MENUGLYPH_ACCEPT);

        x = px + pw - 0x1C - gw;
        y = py + ph - 0x12;
        gdl = menuGlyphDraw(gdl, x, y + 1, MENUGLYPH_ACCEPT, 0xFF);
        x += gw + 4;
        gdl = frontPrintText(gdl, &x, &y, (s8 *) "OK\n", (s32) ptrFontZurichBoldChars, (s32) ptrFontZurichBold,
                             ((sFrames / 20) & 1) ? 0x8A1414FF : 0x000000FF, viGetX(), viGetY(), 0, 0);
    }
    sFrames++;
    return combiner_bayer_lod_perspective(gdl);
}
