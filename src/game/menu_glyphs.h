#ifndef _MENU_GLYPHS_H_
#define _MENU_GLYPHS_H_
#include <ultra64.h>

/* menu_glyphs.c: button prompts in the style of the last pad (or keyboard) used */
enum
{
    MENUGLYPH_ACCEPT,
    MENUGLYPH_BACK,
    MENUGLYPH_LEFT,     /* L; in a hint, L and R together */
    MENUGLYPH_RIGHT,
    MENUGLYPH_CHAT,
    MENUGLYPH_ARROWS,   /* a list stepped both ways: the arrow keys, or L and R on a pad */
    MENUGLYPH_TABS,     /* the previous tab (watch_prev's comma, or L); in a hint, both tabs */
    MENUGLYPH_TABS_R,   /* the next tab (Tab on the keyboard, or R) */
    MENUGLYPH_DPAD      /* a row's value stepped both ways: the arrow keys, or the D-pad */
};

struct MenuHint
{
    s32 action;         /* MENUGLYPH_* */
    const char *word;   /* capitals: the small font has no lowercase */
};

s32 menuGlyphWidth(s32 action);
Gfx *menuGlyphDraw(Gfx *gdl, s32 x, s32 y, s32 action, u32 alpha);
Gfx *menuHintBar(Gfx *gdl, const struct MenuHint *hints, s32 n);
/* the bar from x (left to right) at y; edges, when given, gets each hint's left and right */
Gfx *menuHintBarAt(Gfx *gdl, const struct MenuHint *hints, s32 n, s32 x, s32 y, s32 *edges);

#endif
