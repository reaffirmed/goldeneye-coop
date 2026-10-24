/**
 * Story co-op: give ammo, and the gun if they lack it, to the partner.
 *
 * The give_partner key (default G) works when the partner is alive, within
 * COOPGIVE_RANGE and roughly in front of you. It sends half the reserve of the
 * ammo type in your right hand (at least a magazine's worth when you have
 * that much; the loaded magazine is never touched). The rounds leave your
 * reserve at once and ride a reliable COOPEV_GIVE event; the receiver adds
 * what fits under the type's maximum, adds the gun when it is new to them,
 * and answers with COOPEV_GIVEBACK, which refunds the rounds that did not fit
 * and shows the giver what arrived.
 *
 * The giver keeps the gun. Only the ammo is shared, never made, so a copied
 * gun is worth the rounds that came with it; mission gadgets (ammo types past
 * AMMO_GGUN) and guns with no ammo are never given.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include <boss.h>
#include "math_atan2f.h"
#include "player.h"
#include "bondview.h"
#include "bondinv.h"
#include "gun.h"
#include "coop.h"

extern void portConLogf(const char *fmt, ...);
extern void hudmsgBottomShow(char *mess);
extern int bondinvHasInvItem(ITEM_IDS item);
extern void gunRequestHandWeaponChange(enum GUNHAND hand, s32 nextWeapon, s32 cycleDirection);

#define COOPGIVE_RANGE   250.0f /* horizontal, about 2.5 m */
#define COOPGIVE_HEIGHT  200.0f /* not across floors */
#define COOPGIVE_FRONT   0.5f   /* cos of the half angle in front of you: 60 degrees */

/* ammo you can hand over: real rounds, not gadgets or the watch */
static s32 coopGiveAmmoOk(s32 type)
{
    return type > AMMO_NONE && type <= AMMO_GGUN;
}

/* guns that may go along with their ammo */
static s32 coopGiveWeaponOk(s32 weapon)
{
    return weapon >= ITEM_THROWKNIFE && weapon <= ITEM_REMOTEMINE && weapon != ITEM_WATCHLASER
        && coopGiveAmmoOk(get_ammo_type_for_weapon(weapon));
}

/* menu font safe copy of a game or Steam string, one line */
static void coopGiveCopy(char *out, s32 cap, const char *in)
{
    s32 k = 0;

    for (; in != NULL && *in != '\0' && *in != '\n' && k < cap - 1; in++)
    {
        out[k++] = ((u8) *in >= 0x80 || (u8) *in < 0x20) ? '?' : *in;
    }
    out[k] = '\0';
}

static void coopGiveRounds(char *out, s32 cap, s32 type, s32 n)
{
    const char *what;

    switch (type)
    {
        case AMMO_SHOTGUN:     what = (n == 1) ? "shell" : "shells"; break;
        case AMMO_GRENADE:     what = (n == 1) ? "grenade" : "grenades"; break;
        case AMMO_ROCKETS:     what = (n == 1) ? "rocket" : "rockets"; break;
        case AMMO_REMOTEMINE:
        case AMMO_PROXMINE:
        case AMMO_TIMEDMINE:   what = (n == 1) ? "mine" : "mines"; break;
        case AMMO_KNIFE:       what = (n == 1) ? "knife" : "knives"; break;
        case AMMO_GRENADEROUND: what = (n == 1) ? "grenade round" : "grenade rounds"; break;
        default:               what = (n == 1) ? "round" : "rounds"; break;
    }
    snprintf(out, cap, "%d %s", n, what);
}

static void coopGiveWeaponName(char *out, s32 cap, s32 weapon)
{
    coopGiveCopy(out, cap, (const char *) get_ptr_short_watch_text_for_item(weapon));
}

/* the give_partner key */
void coopGivePress(void)
{
    CoopHudPartner p;
    CoopGive g;
    PropRecord *bond = getCurrentPlayerProp();
    char name[17];
    char msg[96];
    f32 dx, dz, dist;
    s32 weapon, type, reserve, amount, mag;

    if (!coopHudPartner(&p))
    {   /* solo, or the partner is not in this mission */
        if (coopSessionActive())
        {
            portConLogf("ge: coop: give: refused: %s", "Your partner is not here.\n");
            hudmsgBottomShow("Your partner is not here.\n");
        }
        return;
    }
    if (bond == NULL || g_CurrentPlayer->bonddead)
    {
        portConLogf("ge: coop: give: refused: we are down\n");
        return;
    }
    coopGiveCopy(name, sizeof(name), p.name);
    if (p.dead)
    {
        portConLogf("ge: coop: give: refused: the partner is down\n");
        snprintf(msg, sizeof(msg), "%s is down.\n", name);
        hudmsgBottomShow(msg);
        return;
    }
    dx = p.pos.f[0] - bond->pos.f[0];
    dz = p.pos.f[2] - bond->pos.f[2];
    dist = sqrtf(dx * dx + dz * dz);
    if (dist > COOPGIVE_RANGE || fabsf(p.pos.f[1] - bond->pos.f[1]) > COOPGIVE_HEIGHT)
    {
        portConLogf("ge: coop: give: partner too far (%.0f %.0f %.0f vs %.0f %.0f %.0f)\n", p.pos.f[0], p.pos.f[1], p.pos.f[2],
                    bond->pos.f[0], bond->pos.f[1], bond->pos.f[2]);
        snprintf(msg, sizeof(msg), "Get closer to %s.\n", name);
        hudmsgBottomShow(msg);
        return;
    }
    {   /* facing is (-sin, cos) of vv_theta, as bondview2.c builds theta_transform */
        f32 t = g_CurrentPlayer->vv_theta * (M_PI_F / 180.0f);
        if (dist > 1.0f && -sinf(t) * dx + cosf(t) * dz < COOPGIVE_FRONT * dist)
        {
            portConLogf("ge: coop: give: partner not in front\n");
            snprintf(msg, sizeof(msg), "Face %s to give.\n", name);
            hudmsgBottomShow(msg);
            return;
        }
    }

    weapon = getCurrentPlayerWeaponId(GUNRIGHT);
    type = get_ammo_type_for_weapon(weapon);
    if (!coopGiveAmmoOk(type))
    {
        portConLogf("ge: coop: give: refused: %s", "Nothing to give with this weapon.\n");
        hudmsgBottomShow("Nothing to give with this weapon.\n");
        return;
    }
    reserve = g_CurrentPlayer->ammoheldarr[type];
    if (reserve <= 0)
    {
        portConLogf("ge: coop: give: refused: %s", "No spare ammo to give.\n");
        hudmsgBottomShow("No spare ammo to give.\n");
        return;
    }
    mag = get_ptr_item_statistics(weapon)->MagSize;
    amount = reserve / 2;
    if (amount < mag)
    {
        amount = (reserve < mag) ? reserve : mag;
    }
    if (amount < 1)
    {
        amount = 1;
    }

    g_CurrentPlayer->ammoheldarr[type] = reserve - amount;
    memset(&g, 0, sizeof(g));
    g.weapon = coopGiveWeaponOk(weapon) ? weapon : ITEM_NOTHING;
    g.ammotype = type;
    g.amount = amount;
    g.stage = bossGetStageNum();
    coopGiveSend(0, &g);
    portConLogf("ge: coop: give: weapon %d ammotype %d, %d of %d reserve to the partner (dist %.0f), left %d\n",
                weapon, type, amount, reserve, dist, g_CurrentPlayer->ammoheldarr[type]);
}

/* the partner's give: keep what fits, answer with the rest */
void coopGiveReceive(CoopGive *give)
{
    CoopGive a = *give;
    char name[17];
    char rounds[40];
    char gun[24];
    char msg[96];
    s32 type = give->ammotype;
    s32 before = 0;
    s32 after = 0;

    a.amount = 0;
    a.refund = give->amount;
    a.gotweapon = 0;
    if (give->stage == bossGetStageNum() && getCurrentPlayerProp() != NULL && !g_CurrentPlayer->bonddead
        && coopGiveAmmoOk(type) && give->amount > 0 && give->amount <= 10000)
    {
        s32 room;

        before = g_CurrentPlayer->ammoheldarr[type];
        if (give->weapon != ITEM_NOTHING && coopGiveWeaponOk(give->weapon) && get_ammo_type_for_weapon(give->weapon) == type
            && !bondinvHasInvItem(give->weapon))
        {
            bondinvAddInvItem(give->weapon);
            a.gotweapon = 1;
        }
        room = get_max_ammo_for_type(type) - before;
        a.amount = (room < 0) ? 0 : ((room < give->amount) ? room : give->amount);
        a.refund = give->amount - a.amount;
        if (a.amount > 0)
        {
            give_cur_player_ammo(type, before + a.amount);  /* as an ammo pickup does */
        }
        after = g_CurrentPlayer->ammoheldarr[type];

        coopGiveCopy(name, sizeof(name), coopLobbyPartnerName());
        coopGiveRounds(rounds, sizeof(rounds), type, a.amount);
        coopGiveWeaponName(gun, sizeof(gun), give->weapon);
        if (a.gotweapon)
        {   /* two short lines (the HUD queues them): one long one runs into the ammo counter */
            snprintf(msg, sizeof(msg), "%s gave you the %s.\n", name, gun);
            hudmsgBottomShow(msg);
        }
        if (a.amount > 0)
        {
            snprintf(msg, sizeof(msg), "%s gave you %s.\n", name, rounds);
            hudmsgBottomShow(msg);
        }
        else if (!a.gotweapon)
        {
            hudmsgBottomShow("No room for more of that ammo.\n");
        }
    }
    portConLogf("ge: coop: given by the partner: weapon %d ammotype %d, %d sent, took %d (held %d -> %d), refund %d, new gun %d\n",
                give->weapon, type, give->amount, a.amount, before, after, a.refund, a.gotweapon);
    coopGiveSend(1, &a);
}

/* the partner's answer to our give */
void coopGiveReturned(CoopGive *give)
{
    char name[17];
    char rounds[40];
    char gun[24];
    char msg[96];
    s32 type = give->ammotype;

    if (give->stage != bossGetStageNum() || !coopGiveAmmoOk(type))
    {
        return;  /* a give from the last mission: the ammo is gone with it */
    }
    if (give->refund > 0 && give->refund <= 10000)
    {
        give_cur_player_ammo(type, g_CurrentPlayer->ammoheldarr[type] + give->refund);
    }
    portConLogf("ge: coop: give answered: took %d, refund %d, new gun %d, held now %d\n",
                give->amount, give->refund, give->gotweapon, g_CurrentPlayer->ammoheldarr[type]);

    coopGiveCopy(name, sizeof(name), coopLobbyPartnerName());
    coopGiveRounds(rounds, sizeof(rounds), type, give->amount);
    coopGiveWeaponName(gun, sizeof(gun), give->weapon);
    if (give->gotweapon)
    {
        snprintf(msg, sizeof(msg), "Gave %s the %s.\n", name, gun);
        hudmsgBottomShow(msg);
    }
    if (give->amount > 0)
    {
        snprintf(msg, sizeof(msg), "Gave %s to %s.\n", rounds, name);
        hudmsgBottomShow(msg);
    }
    else if (!give->gotweapon)
    {
        snprintf(msg, sizeof(msg), "%s is full on that ammo.\n", name);
        hudmsgBottomShow(msg);
    }
}


/* ---- test harness (coop.c GE_COOP_TESTGIVE) ------------------------------ */

void coopGiveTestDraw(s32 weapon)
{
    bondinvAddInvItem(weapon);
    add_ammo_to_weapon(weapon, g_CurrentPlayer->ammoheldarr[get_ammo_type_for_weapon(weapon)] + 100);
    gunRequestHandWeaponChange(GUNRIGHT, weapon, 0);
    portConLogf("ge: coop: testgive: drew item %d, held %d\n", weapon,
                g_CurrentPlayer->ammoheldarr[get_ammo_type_for_weapon(weapon)]);
}

/* turn to the partner, then the same path as the key */
void coopGiveTestPress(void)
{
    CoopHudPartner p;
    PropRecord *bond = getCurrentPlayerProp();

    if (coopHudPartner(&p) && bond != NULL)
    {
        f32 dx = p.pos.f[0] - bond->pos.f[0];
        f32 dz = p.pos.f[2] - bond->pos.f[2];

        g_CurrentPlayer->vv_theta = -(atan2f(dx, dz) * 180.0f) / M_PI_F;
        if (g_CurrentPlayer->vv_theta < 0.0f)
        {
            g_CurrentPlayer->vv_theta += 360.0f;
        }
    }
    portConLogf("ge: coop: testgive: pressing give_partner\n");
    coopGivePress();
}
