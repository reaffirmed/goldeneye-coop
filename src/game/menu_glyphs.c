/**
 * Button prompts (port only): the menus name their buttons the way the pad
 * in your hands draws them, and follow whatever was pressed last
 * (input_pc.c portInputGlyphStyle): keycaps after the keyboard or mouse,
 * Xbox's coloured letters, PlayStation's cross and circle, a Switch pad's
 * A and B, or the N64's blue A and green B for a pad in N64 mode.
 *
 *   - the hint bar: the folder menus' Select and Back (and a menu's own
 *     extras) along the top of the screen, on the felt above the paper.
 *   - menuGlyphDraw: one prompt anywhere, for a panel's OK (coop_screens.c).
 *
 * The prompts are the default buttons: Accept is A (Enter on the keyboard),
 * Back is B (Escape), Change is L and R. A rebound pad still confirms with
 * its Use bind, which the game takes as A in every menu.
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
#include "othermodemicrocode.h"
#include "bondview.h"
#include "menu_glyphs.h"

extern s32 portInputGlyphStyle(void);

/* input_pc.c's PORT_GLYPH_* */
#define GLYPH_KEYS      0
#define GLYPH_XBOX      1
#define GLYPH_PS        2
#define GLYPH_NINTENDO  3
#define GLYPH_N64       4

#define GLYPH_H         12      /* a prompt's height */
#define GLYPH_GAP       3       /* between a prompt and its word */
#define HINT_SPACE      10      /* between one prompt-and-word and the next */
#define HINT_Y          0x0C    /* the bar's top: on the felt above the paper (it starts at 0x1E) */
#define HINT_RIGHT      0x183   /* the paper's right edge */

struct GlyphLook
{
    const char *text;   /* what is printed on it */
    u32 face;           /* the button's colour */
    u32 ink;            /* the text's */
    s32 round;          /* 1 a face button (a disc), 0 a key or shoulder (a cap) */
};

static struct GlyphLook glyphLook(s32 action)
{
    struct GlyphLook g = { "?", 0x303030FF, 0xFFFFFFFF, 1 };
    const s32 style = portInputGlyphStyle();

    if (style == GLYPH_KEYS)
    {   /* a light keycap with dark letters */
        g.face = 0xE4E0D4FF;
        g.ink = 0x202020FF;
        g.round = 0;
        g.text = (action == MENUGLYPH_ACCEPT) ? "ENTER" : (action == MENUGLYPH_BACK) ? "ESC"
               : (action == MENUGLYPH_CHAT) ? "T" : "CLICK";   /* L and R: a click steps the row */
        return g;
    }
    if (action == MENUGLYPH_LEFT || action == MENUGLYPH_RIGHT)
    {
        const s32 l = (action == MENUGLYPH_LEFT);

        g.round = 0;
        g.face = 0x383838FF;
        g.text = (style == GLYPH_XBOX) ? (l ? "LB" : "RB") : (style == GLYPH_PS) ? (l ? "L1" : "R1") : (l ? "L" : "R");
        return g;
    }
    if (action == MENUGLYPH_CHAT)
    {
        g.round = 0;
        g.face = 0xE4E0D4FF;
        g.ink = 0x202020FF;
        g.text = "T";   /* the chat key is the keyboard's */
        return g;
    }
    switch (style)
    {
        case GLYPH_XBOX:
            g.text = (action == MENUGLYPH_ACCEPT) ? "A" : "B";
            g.face = (action == MENUGLYPH_ACCEPT) ? 0x5BA82EFF : 0xC8312BFF;
            break;
        case GLYPH_PS:
            /* the shapes in their colours on a black button: the font's X
             * and O, which read as the cross and the circle at this size */
            g.text = (action == MENUGLYPH_ACCEPT) ? "X" : "O";
            g.face = 0x1C1C1CFF;
            g.ink = (action == MENUGLYPH_ACCEPT) ? 0x8AB4F0FF : 0xF07070FF;
            break;
        case GLYPH_N64:
            g.text = (action == MENUGLYPH_ACCEPT) ? "A" : "B";
            g.face = (action == MENUGLYPH_ACCEPT) ? 0x2A4FC4FF : 0x2F9A3CFF;
            break;
        default:    /* Switch: the letters on dark grey */
            g.text = (action == MENUGLYPH_ACCEPT) ? "A" : "B";
            g.face = 0x3A3A3AFF;
            break;
    }
    return g;
}

static s32 glyphTextW(const char *text)
{
    s32 th = 0, tw = 0;
    char t[16];

    snprintf(t, sizeof(t), "%s\n", text);
    textMeasure(&th, &tw, t, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    return tw;
}

s32 menuGlyphWidth(s32 action)
{
    const struct GlyphLook g = glyphLook(action);
    const s32 tw = glyphTextW(g.text);

    return g.round ? GLYPH_H : ((tw + 8 > GLYPH_H) ? tw + 8 : GLYPH_H);
}

/* a filled shape GLYPH_H high: a disc (corners cut in steps) or a cap (corners cut once) */
static Gfx *glyphShape(Gfx *gdl, s32 x, s32 y, s32 w, u32 c, s32 round)
{
    if (round)
    {
        gdl = microcode_constructor_related_to_menus(gdl, x + 4, y, x + w - 4, y + GLYPH_H, c);
        gdl = microcode_constructor_related_to_menus(gdl, x + 2, y + 1, x + w - 2, y + GLYPH_H - 1, c);
        gdl = microcode_constructor_related_to_menus(gdl, x + 1, y + 2, x + w - 1, y + GLYPH_H - 2, c);
        gdl = microcode_constructor_related_to_menus(gdl, x, y + 4, x + w, y + GLYPH_H - 4, c);
    }
    else
    {
        gdl = microcode_constructor_related_to_menus(gdl, x + 1, y, x + w - 1, y + GLYPH_H, c);
        gdl = microcode_constructor_related_to_menus(gdl, x, y + 1, x + w, y + GLYPH_H - 1, c);
    }
    return gdl;
}

Gfx *menuGlyphDraw(Gfx *gdl, s32 x, s32 y, s32 action, u32 alpha)
{
    const struct GlyphLook g = glyphLook(action);
    const s32 w = menuGlyphWidth(action);
    const s32 tw = glyphTextW(g.text);
    const u32 a = alpha & 0xFF;
    char t[16];
    s32 tx, ty;

    gdl = glyphShape(gdl, x, y + 1, w, (g.face & 0xFFFFFF00) | ((0x60 * a) / 0xFF) /* a shadow */, g.round);
    if (!g.round && g.face == 0xE4E0D4FF)
    {   /* a keycap's darker lower edge */
        gdl = glyphShape(gdl, x, y + 1, w, (0x8A8676FF & 0xFFFFFF00) | a, 0);
    }
    gdl = glyphShape(gdl, x, y, w, (g.face & 0xFFFFFF00) | (((g.face & 0xFF) * a) / 0xFF), g.round);
    gdl = microcode_constructor(gdl);
    snprintf(t, sizeof(t), "%s\n", g.text);
    tx = x + (w - tw) / 2;
    ty = y + 2;
    gdl = textRender(gdl, &tx, &ty, t, ptrFontBankGothicChars, ptrFontBankGothic,
                     (g.ink & 0xFFFFFF00) | (((g.ink & 0xFF) * a) / 0xFF), (s32) viGetX(), viGetY(), 0, 0);
    return gdl;
}

/* ---- the hint bar ---- */

static Gfx *hintWord(Gfx *gdl, s32 x, s32 y, const char *word)
{
    char t[24];

    snprintf(t, sizeof(t), "%s\n", word);
    return textRenderOutlined(gdl, &x, &y, t, ptrFontBankGothicChars, ptrFontBankGothic, 0xF0F0F0FF, 0x000000A0,
                              (s32) viGetX(), viGetY(), 0, 0);
}

/* a hint's L stands with its R on a pad; the keyboard has one click for both */
static s32 hintPaired(const struct MenuHint *h)
{
    return h->action == MENUGLYPH_LEFT && portInputGlyphStyle() != GLYPH_KEYS;
}

static s32 hintItemW(const struct MenuHint *h)
{
    s32 w = menuGlyphWidth(h->action) + GLYPH_GAP + glyphTextW(h->word);

    if (hintPaired(h))
    {   /* L and R share one word */
        w += 2 + menuGlyphWidth(MENUGLYPH_RIGHT);
    }
    return w;
}

Gfx *menuHintBar(Gfx *gdl, const struct MenuHint *hints, s32 n)
{
    s32 i, x, w = 0;

    for (i = 0; i < n; i++)
    {
        w += hintItemW(&hints[i]) + ((i > 0) ? HINT_SPACE : 0);
    }
    x = HINT_RIGHT - w;
    for (i = 0; i < n; i++)
    {
        const struct MenuHint *h = &hints[i];

        gdl = menuGlyphDraw(gdl, x, HINT_Y, h->action, 0xFF);
        x += menuGlyphWidth(h->action);
        if (hintPaired(h))
        {
            gdl = menuGlyphDraw(gdl, x + 2, HINT_Y, MENUGLYPH_RIGHT, 0xFF);
            x += 2 + menuGlyphWidth(MENUGLYPH_RIGHT);
        }
        x += GLYPH_GAP;
        gdl = hintWord(gdl, x, HINT_Y + 2, h->word);
        x += glyphTextW(h->word) + HINT_SPACE;
    }
    return combiner_bayer_lod_perspective(gdl);
}
