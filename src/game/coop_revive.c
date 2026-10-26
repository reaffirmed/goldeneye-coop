/**
 * Co-op revive, the standing player's side (port only).
 *
 * A partner who is down under a When Down rule with revives comes back when a
 * living player holds Use beside where they fell (coop.c coopDeadTick counts
 * the hold on the downed player's game). Here we pick whom we are reviving:
 * the nearest revivable partner within COOP_REVIVE_DIST while Use is held.
 * Our STATE carries their player number + 1 (coopReviveTarget); the downed
 * game sends its count back in its own STATE, which the HUD shows as a bar
 * (coop_hud.c). When that partner stands up while we hold, the revive is
 * ours (COOPSTAT_REVIVES).
 *
 * Harness: GE_COOP_TESTREVIVE=<s>: from <s> seconds into the mission, walk to
 * the first downed partner that can be revived and hold Use.
 */
#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include "player.h"
#include "stan.h"
#include "coop.h"

extern void portConLogf(const char *fmt, ...);
extern s32 portJoyPortUseHeldInPlay(void);
ITEM_IDS getCurrentPlayerWeaponId(GUNHAND hand);
void hudmsgBottomShow(char *mess);

static s32 sTarget = -1;    /* the partner we hold Use over, -1 none */
static s32 sNear = -1;      /* the partner we could revive from here, -1 none */
static s32 sLastHold;       /* their count last tick, while we held */
static s32 sTestAt = -2;    /* GE_COOP_TESTREVIVE, in mission ticks */
static s32 sTestHold;

static s32 coopReviveInReach(const CoopHudPartner *p, PropRecord *bond)
{
    f32 dx = p->pos.f[0] - bond->pos.f[0];
    f32 dz = p->pos.f[2] - bond->pos.f[2];
    f32 dy = p->pos.f[1] - bond->pos.f[1];

    return dx * dx + dz * dz < COOP_REVIVE_DIST * COOP_REVIVE_DIST && dy < 250.0f && dy > -250.0f;
}

/* harness: step beside a downed partner, then hold Use */
static void coopReviveTestTick(s32 missionticks, PropRecord *bond)
{
    CoopHudPartner p;
    s32 i;

    if (sTestAt == -2)
    {
        const char *e = getenv("GE_COOP_TESTREVIVE");

        sTestAt = (e != NULL) ? atoi(e) * 60 : -1;
    }
    sTestHold = 0;
    if (sTestAt < 0 || missionticks < sTestAt)
    {
        return;
    }
    for (i = 0; coopHudPartnerAt(i, &p); i++)
    {
        if (p.dead && p.revivable)
        {
            break;
        }
    }
    if (!coopHudPartnerAt(i, &p))
    {
        return;
    }
    if (!coopReviveInReach(&p, bond))
    {
        coord3d to = p.pos;
        StandTile *stan;

        to.f[0] += 60.0f;
        to.f[1] += 50.0f;
        stan = stanFindTileBelowPos(&to, NULL, NULL);
        if (stan == NULL)
        {
            to.f[0] -= 60.0f;
            stan = stanFindTileBelowPos(&to, NULL, NULL);
        }
        if (stan != NULL)
        {
            coopRespawnPlayerAt(&to, stan, 0.0f, getCurrentPlayerWeaponId(GUNRIGHT));
            portConLogf("ge: coop: testrevive: stepped beside player %d\n", p.player + 1);
        }
        return;
    }
    sTestHold = 1;
}

/* every mission tick while we are in it (coop.c coopTick) */
void coopReviveTick(s32 missionticks)
{
    PropRecord *bond = getCurrentPlayerProp();
    CoopHudPartner p;
    f32 bestd = 0.0f;
    s32 i, held, was = sTarget;

    sNear = -1;
    sTarget = -1;
    if (bond == NULL || g_CurrentPlayer->bonddead)
    {
        return;
    }
    coopReviveTestTick(missionticks, bond);
    for (i = 0; coopHudPartnerAt(i, &p); i++)
    {
        f32 dx = p.pos.f[0] - bond->pos.f[0];
        f32 dz = p.pos.f[2] - bond->pos.f[2];

        if (p.dead && p.revivable && coopReviveInReach(&p, bond) && (sNear < 0 || dx * dx + dz * dz < bestd))
        {
            sNear = p.player;
            bestd = dx * dx + dz * dz;
        }
        if (p.player == was && !p.dead && sLastHold >= COOP_REVIVE_HOLD / 2)
        {   /* up while we held, and not by a timer that ran out under us */
            char name[40], msg[64];

            coopAsciiName(name, sizeof(name), p.name, 0);
            snprintf(msg, sizeof(msg), "You revived %s.\n", name);
            hudmsgBottomShow(msg);
            coopStatsNote(COOPSTAT_REVIVES);
            portConLogf("ge: coop: revived player %d\n", p.player + 1);
        }
    }
    held = portJoyPortUseHeldInPlay() || sTestHold;
    sLastHold = 0;
    if (held && sNear >= 0)
    {
        sTarget = sNear;
        for (i = 0; coopHudPartnerAt(i, &p); i++)
        {
            if (p.player == sTarget)
            {
                sLastHold = p.revivehold;
            }
        }
        if (was != sTarget)
        {
            portConLogf("ge: coop: reviving player %d\n", sTarget + 1);
        }
    }
}

/* our STATE's revive byte while we stand: whom we are reviving, + 1 */
s32 coopReviveTarget(void)
{
    return (sTarget >= 0) ? sTarget + 1 : 0;
}

/* the HUD: a partner we could revive from here (1), and how far along
 * (*hold, ticks of COOP_REVIVE_HOLD, -1 while we are not holding) */
s32 coopReviveHud(CoopHudPartner *out, s32 *hold)
{
    s32 i;

    if (sNear < 0)
    {
        return 0;
    }
    for (i = 0; coopHudPartnerAt(i, out); i++)
    {
        if (out->player == sNear)
        {
            *hold = (sTarget == sNear) ? out->revivehold : -1;
            return 1;
        }
    }
    return 0;
}
