/**
 * Co-op difficulty (port only): the host's lobby setting COOPSET_DIFFICULTY
 * makes a two-player mission harder. Plain solo, and Normal, are untouched.
 *
 * Two players bring twice the guns and split the guards' fire between two
 * targets, so the modes give guards more health, a little more damage and
 * aim on top of the mission difficulty, and a few more guards:
 *
 *                 health   damage   aim     extra guards
 *     Normal      x1       x1       x1      none
 *     Hard        x1.5     x1.15    x1.15   1 per 4 armed soldiers, up to 6
 *     Brutal      x2       x1.3     x1.3    1 per 2 armed soldiers, up to 12
 *
 * Health is the guards' maxdamage (chrAllocate and the scripts' set-health go
 * through get_007_health_mod), so it covers every weapon and explosions. Damage
 * and aim scale g_AiDamageModifier and g_AiAccuracyModifier each tick, after
 * lvlSetMultipliersForDifficulty sets the mission's own values. Aim 1.15 means
 * 15% more of a guard's shots land; with damage that is about 1.3x (Hard) and
 * 1.7x (Brutal) the damage a guard deals per second, roughly what the second
 * target takes away from each player.
 *
 * All of it runs on the host, whose game owns the guards: damage to the
 * partner is computed there and sent as a hit, and the extra guards reach the
 * joiner as mid-mission spawns (coopMirrorSpawn). Both games allocate the
 * extra chr slots, so the joiner has room for the mirrors.
 *
 * Extra guards are copies of generic soldiers (no named characters, no
 * civilians, no hidden or invincible script chrs), placed beside their
 * source 2 s after both players are in the mission, with the source's body,
 * head, gun and ratings and the standard guard AI (idle until they see or
 * hear a player). They take chrnums from 20000 up, clear of the scripts'
 * clones (chrnum + 10000) and the partner's puppet (30000).
 */
#include <ultra64.h>
#include <bondaicommands.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include "chr.h"
#include "chrai.h"
#include "chraction.h"
#include "model.h"
#include "objecthandler.h"
#include "coop.h"
#include "coop_net.h"

s32 chrGetNumFree(void);
PropRecord *chrSpawnAtCoord(s32 bodynum, s32 headnum, coord3d *pos, StandTile *stan, f32 angle, AIListRecord *ailist, s32 spawnflags);
extern void portConLogf(const char *fmt, ...);

typedef struct
{
    const char *name;
    f32 health;     /* guard maxdamage */
    f32 damage;     /* g_AiDamageModifier */
    f32 aim;        /* g_AiAccuracyModifier */
    s32 every;      /* one extra guard per this many armed soldiers, 0 none */
    s32 maxextra;
} CoopDiffMode;

static const CoopDiffMode sModes[] =
{
    { "Normal", 1.0f, 1.0f,  1.0f,  0, 0 },
    { "Hard",   1.5f, 1.15f, 1.15f, 4, 6 },
    { "Brutal", 2.0f, 1.3f,  1.3f,  2, 12 },
};
#define COOPDIFF_COUNT      ((s32) (sizeof(sModes) / sizeof(sModes[0])))
#define COOPDIFF_MAXCANDS   64      /* sources looked at; a copy with no room tries the next */
#define COOPDIFF_RESERVE    6       /* chr slots left for the level's own spawns */
#define COOPDIFF_CHRNUM     20000
#define COOPDIFF_DELAY      120     /* ticks with both players in before spawning */

static s16 sPlan[COOPDIFF_MAXCANDS];    /* host: soldiers to copy, the evenly spread ones first */
static s32 sPlanCount;
static s32 sPlanNext;
static s32 sPlanned;
static s32 sWanted;     /* extra guards this mission */
static s32 sPlaced;
static s32 sSpawned;
static s32 sBothInTicks;
static s32 sMisses;     /* copies in a row with no room */
static s32 sLogged;

s32 coopDifficultyCount(void)
{
    return COOPDIFF_COUNT;
}

const char *coopDifficultyName(s32 mode)
{
    return (mode >= 0 && mode < COOPDIFF_COUNT) ? sModes[mode].name : "?";
}

/* the mode in force: the host's setting, only with a partner connected */
static s32 coopDifficultyMode(void)
{
    s32 mode = coopSetting(COOPSET_DIFFICULTY);

    if (mode <= 0 || !coopSessionActive())
    {
        return 0;
    }
    return (mode < COOPDIFF_COUNT) ? mode : COOPDIFF_COUNT - 1;
}

/* get_007_health_mod: guard health, on the host */
f32 coopDifficultyHealthMul(void)
{
    return coopNetIsHost() ? sModes[coopDifficultyMode()].health : 1.0f;
}

/* the host's guard health multiplier, on any game (a joiner predicting its shots, coop.c) */
f32 coopDifficultyGuardHealthMul(void)
{
    return sModes[coopDifficultyMode()].health;
}

/* alloc_init_GUARDdata_entries: room for the extra guards, on both games */
s32 coopDifficultyExtraSlots(void)
{
    return sModes[coopDifficultyMode()].maxextra;
}

void coopDifficultyStageStart(void)
{
    sPlanCount = sPlanNext = sPlanned = sWanted = sPlaced = sSpawned = sMisses = 0;
    sBothInTicks = 0;
    sLogged = 0;
}

/* a generic armed soldier, up and about */
static s32 coopDifficultyCanCopy(ChrRecord *chr)
{
    if (chr->model == NULL || chr->prop == NULL || chr->chrnum >= COOPDIFF_CHRNUM)
    {
        return 0;   /* free slot, or one of ours, or the partner's puppet */
    }
    if (chr->chrflags & (CHRFLAG_HIDDEN | CHRFLAG_INVINCIBLE))
    {
        return 0;
    }
    if (chr->actiontype == ACT_DIE || chr->actiontype == ACT_DEAD || chr->prop->stan == NULL)
    {
        return 0;
    }
    switch (chr->bodynum)
    {
        case BODY_Jungle_Commando:
        case BODY_St_Petersburg_Guard:
        case BODY_Russian_Soldier:
        case BODY_Russian_Infantry:
        case BODY_Janus_Special_Forces:
        case BODY_Janus_Marine:
        case BODY_Naval_Officer:
        case BODY_Siberian_Special_Forces:
        case BODY_Siberian_Guard_2:
        case BODY_Arctic_Commando:
        case BODY_Moonraker_Elite_1_Male:
        case BODY_Moonraker_Elite_2_Female:
            break;
        default:
            return 0;
    }
    return chrGetEquippedWeaponProp(chr, GUNRIGHT) != NULL;
}

static void coopDifficultyPlan(const CoopDiffMode *m)
{
    s32 i;
    s32 pass;
    s32 n = 0;
    f32 health = 0.0f;

    sPlanned = 1;
    for (pass = 0; pass < 2; pass++)
    {   /* every m->every'th soldier, then the rest as stand-ins */
        n = 0;
        for (i = 0; i < g_NumChrSlots && sPlanCount < COOPDIFF_MAXCANDS; i++)
        {
            ChrRecord *chr = &g_ChrSlots[i];

            if (!coopDifficultyCanCopy(chr))
            {
                continue;
            }
            health = chr->maxdamage;
            if (((++n % m->every) == 0) == (pass == 0))
            {
                sPlan[sPlanCount++] = chr->chrnum;
            }
        }
    }
    sWanted = n / m->every;
    if (sWanted > m->maxextra)
    {
        sWanted = m->maxextra;
    }
    portConLogf("ge: coop: difficulty %s: %d armed soldiers (health %.1f), %d extra guards wanted (free slots %d)\n",
                m->name, n, health, sWanted, chrGetNumFree());
}

static s32 coopDifficultySpawn(s16 srcnum)
{
    ChrRecord *src = chrFindByLiteralId(srcnum);
    PropRecord *gun;
    PropRecord *np;
    ChrRecord *chr;
    s32 chrnum;

    if (src == NULL || !coopDifficultyCanCopy(src))
    {
        return 0; /* died or went away since the plan */
    }
    gun = chrGetEquippedWeaponProp(src, GUNRIGHT);
    np = chrSpawnAtCoord(src->bodynum, src->headnum, &src->prop->pos, src->prop->stan, getsubroty(src->model),
                         (AIListRecord *) ailistFindById(GAILIST_STANDARD_GUARD), 0x10);
    if (np == NULL)
    {
        portConLogf("ge: coop: difficulty: no room beside chr %d\n", srcnum);
        return 0;
    }
    chr = np->chr;
    chrnum = COOPDIFF_CHRNUM + sSpawned;
    while (chrFindByLiteralId(chrnum) != NULL)
    {
        chrnum++;
    }
    chr->chrnum = chrnum;
    sSpawned = chrnum - COOPDIFF_CHRNUM + 1;
    chr->accuracyrating = src->accuracyrating;
    chr->speedrating = src->speedrating;
    chr->arghrating = src->arghrating;
    chr->grenadeprob = src->grenadeprob;
    if (gun != NULL && gun->weapon != NULL)
    {
        chrGiveWeapon(chr, gun->weapon->obj, gun->weapon->weaponnum, 0);
    }
    portConLogf("ge: coop: difficulty: extra guard %d beside chr %d at %.0f %.0f %.0f (health %.1f, free %d)\n",
                chrnum, srcnum, np->pos.f[0], np->pos.f[1], np->pos.f[2], chr->maxdamage, chrGetNumFree());
    return 1;
}

/* lvlTick, after lvlSetMultipliersForDifficulty */
void coopDifficultyTick(void)
{
    s32 mode = coopDifficultyMode();
    const CoopDiffMode *m = &sModes[mode];
    CoopHudPartner partner;
    s32 i;

    if (mode == 0)
    {
        return;
    }
    if (!sLogged)
    {
        sLogged = 1;
        portConLogf("ge: coop: difficulty %s (%s): guard health x%.2f, damage x%.2f, aim x%.2f, extra 1 per %d up to %d\n",
                    m->name, coopNetIsHost() ? "applied here" : "the host applies it",
                    m->health, m->damage, m->aim, m->every, m->maxextra);
    }
    if (!coopNetIsHost())
    {
        return;
    }
    g_AiDamageModifier *= m->damage;
    g_AiAccuracyModifier *= m->aim;

    if (!coopHudPartner(&partner))
    {
        sBothInTicks = 0;   /* wait for both players to be in the mission */
        return;
    }
    if (++sBothInTicks < COOPDIFF_DELAY || (sPlanned && sPlaced >= sWanted))
    {
        return;
    }
    if (!sPlanned)
    {
        coopDifficultyPlan(m);
    }
    for (i = 0; i < 2 && sPlaced < sWanted; i++)
    {   /* two a tick: the joiner mirrors up to four */
        s32 ok;

        if (sPlanNext >= sPlanCount || sMisses >= 4 || chrGetNumFree() < 3 + COOPDIFF_RESERVE)
        {
            portConLogf("ge: coop: difficulty: out of %s, %d extra guards placed\n",
                        (sPlanNext >= sPlanCount) ? "soldiers to copy" : "room", sPlaced);
            sWanted = sPlaced;
            break;
        }
        ok = coopDifficultySpawn(sPlan[sPlanNext++]);
        sPlaced += ok;
        sMisses = ok ? 0 : sMisses + 1;
    }
    if (sPlaced >= sWanted)
    {
        portConLogf("ge: coop: difficulty: %d extra guards placed, modifiers now damage %.2f aim %.2f\n",
                    sPlaced, g_AiDamageModifier, g_AiAccuracyModifier);
    }
}
