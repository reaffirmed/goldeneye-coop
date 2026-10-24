/**
 * Weapon menu (port only), the way Half-Life picks weapons.
 *
 * Weapons sit in five slots: 1 melee, 2 pistols, 3 SMGs and rifles, 4 heavy,
 * 5 explosives. A weapon_slot_N key opens the menu on that slot, and pressing
 * it again steps through the slot; next / previous weapon step through every
 * slot. Fire takes the highlighted weapon, and the menu closes by itself,
 * changing nothing, after three seconds without a press. The game keeps
 * running while it is open.
 *
 * [display] weapon_menu (default on). Off: the slot keys still work, taking
 * the slot's next weapon straight away, and next / previous weapon cycle as
 * they always have. Solo only; multiplayer keeps Rare's cycling.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include "textrelated.h"
#include "fr.h"
#include "player.h"
#include "othermodemicrocode.h"
#include "bondview.h"
#include "bondinv.h"
#include "gun.h"
#include "options.h"
#include "language.h"
#include "coop.h"

extern Gfx *portHudScaleBegin(Gfx *gdl, f32 x, f32 y);
extern Gfx *portHudScaleEnd(Gfx *gdl);
extern s32 portWeaponMenuOn(void);                  /* video.c [display] weapon_menu; weak 0 headless */
extern s32 portJoyPortWeaponSlotPressed(void);      /* joy_port.c: 1..5 or 0 */
extern void gunRequestHandWeaponChange(enum GUNHAND hand, s32 nextWeapon, s32 cycleDirection);
extern u16 *get_ptr_short_watch_text_for_item(ITEM_IDS item);

#define WM_SLOTS        5
#define WM_PER_SLOT     16
#define WM_TIMEOUT      180   /* ticks without a press before the menu closes */

typedef struct
{
    s16 right;
    s16 left;   /* ITEM_UNARMED unless a pair */
} WmEntry;

static WmEntry sEnt[WM_SLOTS][WM_PER_SLOT];
static s32 sNum[WM_SLOTS];
static s32 sOpen;
static s32 sSlot;
static s32 sIdx;
static s32 sIdle;
static s32 sSwallow;    /* fire was the confirm: keep it from the gun until released */
static s32 sTestFire;   /* GE_WEAPMENU_TEST: a fire press for weapmenuFilterButtons */

extern char *getenv(const char *);
extern void portConLogf(const char *fmt, ...);

/* GE_WEAPMENU_TEST=1 (harness only): from 5 s in, slot 1, fire, next weapon,
 * fire, slot 2 twice, then leave it to time out. Returns a slot or step to
 * inject as {slot, step}. */
static void wmHarness(s32 *slot, s32 *step)
{
    static s32 on = -1;
    static s32 t;

    if (on < 0)
    {
        on = getenv("GE_WEAPMENU_TEST") != NULL;
    }
    if (!on)
    {
        return;
    }
    t++;
    switch (t)
    {
        case 300: *slot = 1; break;
        case 420: sTestFire = TRUE; break;
        case 540: *step = 1; break;
        case 560: *step = 1; break;
        case 660: sTestFire = TRUE; break;
        case 780: *slot = 2; break;
        case 800: *slot = 2; break;
        default: break;
    }
    if (*slot != 0 || *step != 0 || sTestFire)
    {
        portConLogf("ge: weapmenu test t=%d slot=%d step=%d fire=%d\n", t, *slot, *step, sTestFire);
    }
}

static s32 wmSlotOf(s32 item)
{
    switch (item)
    {
        case ITEM_UNARMED: case ITEM_FIST: case ITEM_KNIFE: case ITEM_THROWKNIFE: case ITEM_TASER:
            return 0;
        case ITEM_WPPK: case ITEM_WPPKSIL: case ITEM_TT33: case ITEM_RUGER: case ITEM_GOLDENGUN:
        case ITEM_SILVERWPPK: case ITEM_GOLDWPPK: case ITEM_LASER:
            return 1;
        case ITEM_SKORPION: case ITEM_AK47: case ITEM_UZI: case ITEM_MP5K: case ITEM_MP5KSIL:
        case ITEM_SPECTRE: case ITEM_M16: case ITEM_FNP90: case ITEM_SHOTGUN: case ITEM_AUTOSHOT:
        case ITEM_SNIPERRIFLE:
            return 2;
        case ITEM_GRENADELAUNCH: case ITEM_ROCKETLAUNCH:
            return 3;
        case ITEM_GRENADE: case ITEM_TIMEDMINE: case ITEM_PROXIMITYMINE: case ITEM_REMOTEMINE:
        case ITEM_WATCHLASER:
            return 4;
        default:
            return -1;   /* the tank's shells, the detonator, gadgets: not on the menu */
    }
}

static void wmAdd(s32 right, s32 left)
{
    s32 s = wmSlotOf(right);
    s32 i, k;

    if (s < 0 || sNum[s] >= WM_PER_SLOT)
    {
        return;
    }
    for (i = 0; i < sNum[s]; i++)
    {
        if (sEnt[s][i].right == right && sEnt[s][i].left == left)
        {
            return;
        }
    }
    /* by item number, a pair after its single, as Rare's cycle orders them */
    for (i = 0; i < sNum[s] && (sEnt[s][i].right < right || (sEnt[s][i].right == right && sEnt[s][i].left < left)); i++)
    {
    }
    for (k = sNum[s]; k > i; k--)
    {
        sEnt[s][k] = sEnt[s][k - 1];
    }
    sEnt[s][i].right = (s16) right;
    sEnt[s][i].left = (s16) left;
    sNum[s]++;
}

/* what Bond is carrying, slotted */
static void wmCollect(void)
{
    InvItem *first = g_CurrentPlayer->ptr_inventory_first_in_cycle;
    InvItem *item = first;
    s32 i;

    memset(sNum, 0, sizeof(sNum));
    while (item != NULL)
    {
        if (item->type == INV_ITEM_WEAPON && item->type_inv_item.type_weap.weapon < ITEM_BOMBCASE)
        {
            wmAdd(item->type_inv_item.type_weap.weapon, ITEM_UNARMED);
        }
        else if (item->type == INV_ITEM_DUAL)
        {
            wmAdd(item->type_inv_item.type_dual.weapon_right, item->type_inv_item.type_dual.weapon_left);
        }
        item = item->next;
        if (item == first)
        {
            break;
        }
    }
    if (g_CurrentPlayer->equipallguns)
    {   /* the All Guns cheat carries everything */
        for (i = ITEM_FIST; i < ITEM_TANKSHELLS; i++)
        {
            wmAdd(i, ITEM_UNARMED);
        }
    }
}

static s32 wmIsCurrent(const WmEntry *e)
{
    return e->right == getCurrentPlayerWeaponId(GUNRIGHT) && e->left == getCurrentPlayerWeaponId(GUNLEFT);
}

static void wmEquip(const WmEntry *e)
{
    portConLogf("ge: weapmenu: take %d/%d (holding %d/%d)\n", e->right, e->left,
                getCurrentPlayerWeaponId(GUNRIGHT), getCurrentPlayerWeaponId(GUNLEFT));
    if (wmIsCurrent(e))
    {
        return;
    }
    gunRequestHandWeaponChange(GUNRIGHT, e->right, 1);
    gunRequestHandWeaponChange(GUNLEFT, e->left, 1);
}

static s32 wmLive(void)
{
    return getPlayerCount() == 1 && !g_CurrentPlayer->bonddead && g_CurrentPlayer->watch_animation_state == 0
        && g_PlayerIsInTank != 1;   /* the tank has its own guns */
}

/* the entry after the one in hand within slot s, or its first */
static s32 wmNextInSlot(s32 s)
{
    s32 i;

    for (i = 0; i < sNum[s]; i++)
    {
        if (wmIsCurrent(&sEnt[s][i]))
        {
            return (i + 1) % sNum[s];
        }
    }
    return 0;
}

/* step through every slot's weapons in order */
static void wmStepAll(s32 dir)
{
    s32 tries;

    for (tries = 0; tries < WM_SLOTS * WM_PER_SLOT; tries++)
    {
        sIdx += dir;
        if (sIdx >= sNum[sSlot])
        {
            sSlot = (sSlot + 1) % WM_SLOTS;
            sIdx = 0;
        }
        else if (sIdx < 0)
        {
            sSlot = (sSlot + WM_SLOTS - 1) % WM_SLOTS;
            sIdx = sNum[sSlot] - 1;
        }
        if (sIdx >= 0 && sIdx < sNum[sSlot])
        {
            return;
        }
    }
}

static void wmOpenOnCurrent(void)
{
    s32 s, i;

    sSlot = 0;
    sIdx = 0;
    for (s = 0; s < WM_SLOTS; s++)
    {
        for (i = 0; i < sNum[s]; i++)
        {
            if (wmIsCurrent(&sEnt[s][i]))
            {
                sSlot = s;
                sIdx = i;
            }
        }
    }
    sOpen = TRUE;
}

/**
 * bondviewProcessInput, where next / previous weapon are read. `step` is
 * joy_port's -1 / 0 / +1. Returns the step Rare's cycling should still act on
 * (0 once the menu took it).
 */
s32 weapmenuInput(s32 step)
{
    s32 slot = portJoyPortWeaponSlotPressed();
    const s32 menu = portWeaponMenuOn();

    if (!wmLive())
    {
        sOpen = FALSE;
        return step;
    }
    wmHarness(&slot, &step);
    if (slot == 0 && (step == 0 || !menu))
    {
        if (sOpen && ++sIdle > WM_TIMEOUT)
        {
            sOpen = FALSE;   /* timed out: nothing changes */
        }
        return step;
    }
    wmCollect();
    sIdle = 0;
    if (slot != 0)
    {
        const s32 s = slot - 1;

        if (sNum[s] == 0)
        {
            return 0;   /* nothing in that slot */
        }
        if (!menu)
        {
            wmEquip(&sEnt[s][wmNextInSlot(s)]);
            return 0;
        }
        if (sOpen && sSlot == s)
        {
            sIdx = (sIdx + 1) % sNum[s];
        }
        else
        {
            sOpen = TRUE;
            sSlot = s;
            sIdx = wmNextInSlot(s);
        }
        return 0;
    }
    /* next / previous weapon with the menu on */
    if (!sOpen)
    {
        wmOpenOnCurrent();
    }
    wmStepAll(step > 0 ? 1 : -1);
    return 0;
}

/**
 * Before bondviewProcessInput: with the menu up, a fresh fire press takes the
 * highlighted weapon instead of shooting, and fire stays off the gun until it
 * is let go.
 */
u16 weapmenuFilterButtons(u16 buttons, u16 oldbuttons)
{
    if (sTestFire)
    {
        sTestFire = FALSE;
        buttons |= Z_TRIG;
        oldbuttons &= ~Z_TRIG;
    }
    if (sOpen && wmLive() && (buttons & ~oldbuttons & Z_TRIG))
    {
        wmCollect();
        if (sSlot >= 0 && sSlot < WM_SLOTS && sIdx >= 0 && sIdx < sNum[sSlot])
        {
            wmEquip(&sEnt[sSlot][sIdx]);
        }
        sOpen = FALSE;
        sSwallow = TRUE;
    }
    if (sSwallow)
    {
        if (!(buttons & Z_TRIG))
        {
            sSwallow = FALSE;
        }
        return buttons & ~Z_TRIG;
    }
    return buttons;
}

/* one line of the small font, in capitals */
static void wmName(char *out, s32 cap, const WmEntry *e)
{
    const char *n = (const char *) get_ptr_short_watch_text_for_item(e->right);
    s32 k = 0;
    s32 i;

    if (e->left != ITEM_UNARMED && k < cap - 4)
    {
        out[k++] = '2';
        out[k++] = 'X';
        out[k++] = ' ';
    }
    for (i = 0; n != NULL && n[i] != '\0' && n[i] != '\n' && k < cap - 2; i++)
    {
        char c = n[i];

        if ((u8) c >= 0x80 || (u8) c < 0x20)
        {
            c = '?';
        }
        else if (c >= 'a' && c <= 'z')
        {
            c -= 'a' - 'A';
        }
        out[k++] = c;
    }
    out[k++] = '\n';
    out[k] = '\0';
}

static Gfx *wmText(Gfx *gdl, s32 x, s32 y, char *text, u32 colour)
{
    return textRenderOutlined(gdl, &x, &y, text, ptrFontBankGothicChars, ptrFontBankGothic, colour, 0x000000A0,
                              (s32) viGetX(), viGetY(), 0, 0);
}

/* along the top: a numbered box per slot, the open one listing its weapons */
Gfx *weapmenuRender(Gfx *gdl)
{
    s32 x = viGetViewLeft() + 0x24;
    const s32 top = viGetViewTop() + 0x0C;
    s32 s, i;

    if (!sOpen || !wmLive())
    {
        return gdl;
    }
    wmCollect();
    if (sIdx >= sNum[sSlot])
    {
        sIdx = sNum[sSlot] - 1;
    }
    if (hudAnchorsToEdges())
    {
        PORT_HUD_ANCHOR(gdl, PORT_HUD_ANCHOR_LEFT);
    }
    gdl = portHudScaleBegin(gdl, (f32) viGetViewLeft(), (f32) viGetViewTop());
    gdl = microcode_constructor(gdl);
    for (s = 0; s < WM_SLOTS; s++)
    {
        char num[4];
        s32 w = 12;

        if (s == sSlot)
        {   /* the open slot is as wide as its longest name */
            for (i = 0; i < sNum[s]; i++)
            {
                char name[40];
                s32 th = 0, tw = 0;

                wmName(name, sizeof(name), &sEnt[s][i]);
                textMeasure(&th, &tw, name, ptrFontBankGothicChars, ptrFontBankGothic, 0);
                if (tw + 8 > w)
                {
                    w = tw + 8;
                }
            }
        }
        gdl = microcode_constructor_related_to_menus(gdl, x, top, x + w, top + 11, (sNum[s] > 0) ? 0x000000A0 : 0x00000050);
        sprintf(num, "%d\n", (int) s + 1);
        gdl = wmText(gdl, x + 3, top + 2, num, (s == sSlot) ? 0xFFC040FF : (sNum[s] > 0 ? 0xFFFFFFFF : 0x808080A0));
        if (s == sSlot)
        {
            for (i = 0; i < sNum[s]; i++)
            {
                char name[40];
                const s32 y = top + 13 + i * 11;
                const s32 hot = (i == sIdx);

                wmName(name, sizeof(name), &sEnt[s][i]);
                gdl = microcode_constructor_related_to_menus(gdl, x, y, x + w, y + 10, hot ? 0xD08000C0 : 0x00000090);
                gdl = wmText(gdl, x + 4, y + 2, name, hot ? 0xFFFFFFFF : (wmIsCurrent(&sEnt[s][i]) ? 0xFFC040FF : 0xD0D0D0FF));
            }
        }
        x += w + 3;
    }
    gdl = combiner_bayer_lod_perspective(gdl);
    gdl = portHudScaleEnd(gdl);
    if (hudAnchorsToEdges())
    {
        PORT_HUD_ANCHOR_OFF(gdl);
    }
    return gdl;
}

/* the partner status sits where the menu draws: coop_hud.c skips it while open */
s32 weapmenuIsOpen(void)
{
    return sOpen && wmLive();
}
