/**
 * weapon_menu.c with two players on one screen (no game, no ROM):
 *
 *   gcc -m32 -O1 -w -fms-extensions -I. -Iinclude -Iinclude/PR -Iinclude/ultra64 -Iport/include \
 *       -Isrc -Isrc/game -DNONMATCHING -DAVOID_UB -D_LANGUAGE_C -o weapmenu_split_test coop/ci/weapmenu_split_test.c
 *   ./weapmenu_split_test
 *
 * Split-screen story runs each player's input and HUD through the weapon menu
 * in turn. v0.9.0 kept one menu for all of them, so a menu opened by player 1
 * showed in player 2's view too, player 2's next shot took the highlighted
 * weapon out of player 2's own inventory instead of firing, and player 1's
 * confirm then fired instead of picking (testers' report). Both players carry
 * a PP7 and a KF7, KF7 in hand; player 1 opens the pistol slot, player 2
 * shoots, player 1 confirms. Exit 1 if player 2's menu opens, player 2's shot
 * is eaten or changes their gun, or player 1 doesn't end up with the PP7.
 */
#include <stdio.h>
#include <string.h>
#include "../../src/game/weapon_menu.c"

struct player *g_CurrentPlayer;
s32 g_PlayerIsInTank;
struct font *ptrFontBankGothic;
struct fontchar *ptrFontBankGothicChars;

static struct player sPl[2];
static InvItem sInv[2][2];
static s32 sHeld[2][2];   /* [player][hand] */
static s32 sCur;
static s32 sSlotPress[2];

/* the game around the menu */
s32 get_cur_playernum(void) { return sCur; }
s32 getPlayerCount(void) { return 2; }
s32 splitStoryActive(void) { return 1; }
s32 coopVersus(void) { return 0; }
MPSCENARIOS get_scenario(void) { return 0; }
s32 bondinvIsAliveWithFlag(void) { return 0; }
s32 bondinvCountTotalItemsInInv(void) { return 0; }
s32 bondinvGetTextbyInvIndex(s32 i) { (void) i; return 0; }
u16 *bondinvGetNameByIndex(s32 i) { (void) i; return NULL; }
u16 *get_ptr_short_watch_text_for_item(ITEM_IDS i) { (void) i; return NULL; }
ITEM_IDS getCurrentPlayerWeaponId(GUNHAND h) { return sHeld[sCur][h]; }
void gunRequestHandWeaponChange(enum GUNHAND h, s32 w, s32 d) { (void) d; sHeld[sCur][h] = w; }
s32 portWeaponMenuOn(void) { return 1; }
s32 portJoyPortWeaponSlotPressed(void) { s32 s = sSlotPress[sCur]; sSlotPress[sCur] = 0; return s; }
void portConLogf(const char *fmt, ...) { (void) fmt; }

/* drawing: weapmenuRender is not called here */
Gfx *combiner_bayer_lod_perspective(Gfx *gdl) { return gdl; }
Gfx *microcode_constructor(Gfx *gdl) { return gdl; }
Gfx *microcode_constructor_related_to_menus(Gfx *gdl, s32 ulx, s32 uly, s32 lrx, s32 lry, u32 color) { return gdl; }
Gfx *portHudScaleBegin(Gfx *gdl, f32 x, f32 y) { return gdl; }
Gfx *portHudScaleEnd(Gfx *gdl) { return gdl; }
u32 portHudAnchorFrac(void) { return 0; }
s32 hudAnchorsToEdges(void) { return 0; }
void textMeasure(s32 *th, s32 *tw, char *text, struct fontchar *f1, struct font *f2, s32 lh) { }
Gfx *textRenderOutlined(Gfx *gdl, s32 *x, s32 *y, char *text, struct fontchar *chars, struct font *font, u32 c, u32 c2,
                        s32 w, s32 h, s32 yo, s32 lh) { return gdl; }
s16 viGetViewLeft(void) { return 0; }
s16 viGetViewTop(void) { return 0; }
s16 viGetX(void) { return 320; }
s16 viGetY(void) { return 240; }

static void use(s32 p)
{
    sCur = p;
    g_CurrentPlayer = &sPl[p];
}

static u16 sOld[2];

/* one tick: each player's buttons through the fire filter, then their slot key */
static void tick(u16 b0, s32 slot0, u16 b1, s32 slot1, u16 out[2])
{
    const u16 b[2] = { b0, b1 };
    const s32 sl[2] = { slot0, slot1 };
    s32 p;

    for (p = 0; p < 2; p++)
    {
        use(p);
        out[p] = weapmenuFilterButtons(b[p], sOld[p]);
        sOld[p] = b[p];
        sSlotPress[p] = sl[p];
        weapmenuInput(0);
    }
}

static s32 sFails;

static void check(s32 ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    sFails += !ok;
}

int main(void)
{
    u16 out[2];
    s32 p;

    for (p = 0; p < 2; p++)
    {
        sInv[p][0].type = INV_ITEM_WEAPON;
        sInv[p][0].type_inv_item.type_weap.weapon = ITEM_WPPK;
        sInv[p][1].type = INV_ITEM_WEAPON;
        sInv[p][1].type_inv_item.type_weap.weapon = ITEM_AK47;
        sInv[p][0].next = &sInv[p][1];
        sInv[p][1].next = &sInv[p][0];
        sPl[p].ptr_inventory_first_in_cycle = &sInv[p][0];
        sHeld[p][GUNRIGHT] = ITEM_AK47;
        sHeld[p][GUNLEFT] = ITEM_UNARMED;
    }

    tick(0, 2, 0, 0, out);   /* player 1: the pistol slot key */
    use(0);
    check(weapmenuIsOpen(), "player 1's menu opens");
    use(1);
    check(!weapmenuIsOpen(), "player 2's view has no menu");

    tick(0, 0, Z_TRIG, 0, out);   /* player 2 shoots */
    check((out[1] & Z_TRIG) != 0, "player 2's shot reaches the gun");
    check(sHeld[1][GUNRIGHT] == ITEM_AK47, "player 2 keeps the KF7");
    tick(0, 0, 0, 0, out);

    tick(Z_TRIG, 0, 0, 0, out);   /* player 1 confirms */
    check((out[0] & Z_TRIG) == 0, "player 1's confirm doesn't fire");
    check(sHeld[0][GUNRIGHT] == ITEM_WPPK, "player 1 takes the PP7");
    check(sHeld[1][GUNRIGHT] == ITEM_AK47, "player 2 still has the KF7");
    use(0);
    check(!weapmenuIsOpen(), "player 1's menu closes");

    printf(sFails ? "FAIL\n" : "PASS\n");
    return sFails != 0;
}
