/**
 * Co-op mission stats and the co-op campaign record (port only).
 *
 * Stats: both games keep a column per player (kills, shots, hits, head hits,
 * deaths, times revived, respawns). Each game counts its own shots and deaths.
 * Kills and hits on guards belong to the host, which runs the guards: a shot
 * or blast the joiner sends is applied on the host between coopStatsBlameBegin
 * and coopStatsBlameEnd, which moves what it added to the host's own counters
 * into the partner's column. When a game is back in the menus it sends its
 * final column (the host adds the joiner's kills and hits) as one reliable
 * event; the debrief (front.c, menu0D) draws both columns.
 *
 * Campaign: missions finished together, per partner, in coop_campaign.txt next
 * to the game. The lobby asks coopCampaignNext for the mission after the
 * furthest one finished with the partner it has now.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include <boss.h>
#include "player.h"
#include "lv.h"
#include "front.h"
#include "objective_status.h"
#include "coop.h"
#include "coop_net.h"

extern void portConLogf(const char *fmt, ...);
extern const char *portRootPath(const char *name);

/* ---- mission stats --------------------------------------------------------- */

/* COOPEV_STATS payload (at most 28 bytes, see CoopEvent in coop.c) */
typedef struct
{
    s16 stage;
    u8 fromhost;
    u8 pad;
    u16 own[COOPSTAT_COUNT];    /* the sender's column (a joiner's kills and head hits mean nothing) */
    u16 yours[3];               /* host: the receiver's kills, hits on guards, head hits */
} CoopStatsMsg;

typedef struct
{
    s32 v[COOPSTAT_COUNT];
} CoopStatsCol;

static s32 sStage = -1;         /* the mission the table is for */
static s32 sCoop;               /* the partner was in it with us */
static s32 sLive;               /* in the mission; the final column is not sent yet */
static s32 sDown;               /* this death is counted */
static CoopStatsCol sSelf;      /* deaths, revived, respawns while live; everything once final */
static CoopStatsCol sPartner;   /* from the partner's event; host: kills, hits, head from blame */
static s32 sPartnerIn;          /* the partner's final column arrived */
static s32 sHostIn;             /* joiner: the host's numbers for our kills arrived */
static s32 sYours[3];           /* joiner: kills, hits on guards, head hits, from the host */
static char sNames[2][32];

/* host: the partner's shot or blast is being applied */
static s32 sBlame;
static s32 sBlameKills;
static s32 sBlameShots[7];

void coopSendStatsEvent(const void *payload, s32 len);  /* coop.c */
s32  coopApplyingPartnerExplosion(void);                /* coop.c */

static void coopStatsCopyName(char *out, s32 cap, const char *name)
{
    s32 i, n = 0;

    for (i = 0; name != NULL && name[i] != '\0' && n < cap - 1; i++)
    {
        const u8 ch = (u8) name[i];

        if (ch >= 0x20 && ch < 0x7F)
        {
            out[n++] = (char) ch;
        }
        else if (ch >= 0xC0)
        {
            out[n++] = '?';   /* one per UTF-8 character, as the lobby shows names */
        }
    }
    out[n] = '\0';
}

void coopStatsMissionStart(void)
{
    sStage = bossGetStageNum();
    sCoop = 0;
    sLive = 1;
    sDown = 0;
    sPartnerIn = 0;
    sHostIn = 0;
    sBlame = 0;
    memset(&sSelf, 0, sizeof(sSelf));
    memset(&sPartner, 0, sizeof(sPartner));
    memset(sYours, 0, sizeof(sYours));
}

/* every mission tick (coopTick); partnerhere: the partner is in this stage */
void coopStatsMissionTick(s32 partnerhere)
{
    if (!sLive)
    {
        return;
    }
    if (partnerhere && !sCoop)
    {
        sCoop = 1;
        coopStatsCopyName(sNames[0], sizeof(sNames[0]), coopLobbySelfName());
        coopStatsCopyName(sNames[1], sizeof(sNames[1]), coopLobbyPartnerName());
    }
    if (!g_CurrentPlayer->bonddead)
    {
        sDown = 0;
    }
}

/* our Bond is dead (coopDeadTick, every tick until the respawn or the end) */
void coopStatsDown(void)
{
    if (sLive && !sDown)
    {
        sDown = 1;
        sSelf.v[COOPSTAT_DEATHS]++;
    }
}

void coopStatsNote(s32 stat)
{
    if (sLive && stat >= 0 && stat < COOPSTAT_COUNT)
    {
        sSelf.v[stat]++;
    }
}

/* Host: what the code between Begin and End adds to our kills and hit
 * registers was the partner's doing. Calls do not nest. */
void coopStatsBlameBegin(s32 partner)
{
    if (!partner || sBlame || !sLive || g_playerPerm == NULL)
    {
        return;
    }
    sBlame = 1;
    sBlameKills = g_playerPerm->kill_count;
    memcpy(sBlameShots, g_playerPerm->shot_count, sizeof(sBlameShots));
}

void coopStatsBlameEnd(void)
{
    s32 i;
    s32 hits = 0;

    if (!sBlame)
    {
        return;
    }
    sBlame = 0;
    sPartner.v[COOPSTAT_KILLS] += g_playerPerm->kill_count - sBlameKills;
    g_playerPerm->kill_count = sBlameKills;
    for (i = SHOT_REGISTER_HEAD; i <= SHOT_REGISTER_OBJECT; i++)
    {
        hits += g_playerPerm->shot_count[i] - sBlameShots[i];
    }
    sPartner.v[COOPSTAT_HITS] += hits;
    sPartner.v[COOPSTAT_HEAD] += g_playerPerm->shot_count[SHOT_REGISTER_HEAD] - sBlameShots[SHOT_REGISTER_HEAD];
    memcpy(g_playerPerm->shot_count, sBlameShots, sizeof(sBlameShots));
}

/* explosion.c: the blasts coopApplyExplosion sets off for the partner */
#define COOP_STATS_BOOMS 8
static void *sPartnerBooms[COOP_STATS_BOOMS];

void coopStatsExplosionMade(void *explosion)
{
    s32 i;

    for (i = 0; i < COOP_STATS_BOOMS; i++)
    {
        if (sPartnerBooms[i] == explosion)
        {
            sPartnerBooms[i] = NULL;   /* the slot is reused */
        }
    }
    if (!coopApplyingPartnerExplosion())
    {
        return;
    }
    for (i = 0; i < COOP_STATS_BOOMS; i++)
    {
        if (sPartnerBooms[i] == NULL)
        {
            sPartnerBooms[i] = explosion;
            return;
        }
    }
    sPartnerBooms[0] = explosion;
}

s32 coopStatsExplosionIsPartner(void *explosion)
{
    s32 i;

    for (i = 0; explosion != NULL && i < COOP_STATS_BOOMS; i++)
    {
        if (sPartnerBooms[i] == explosion)
        {
            return 1;
        }
    }
    return 0;
}

static s32 coopStatsLocalHits(void)
{
    s32 i, hits = 0;

    for (i = SHOT_REGISTER_HEAD; i <= SHOT_REGISTER_OBJECT; i++)
    {
        hits += g_playerPerm->shot_count[i];
    }
    return hits;
}

static u16 coopStatsU16(s32 v)
{
    return (u16) ((v < 0) ? 0 : (v > 0xFFFF) ? 0xFFFF : v);
}

/* every frontend tick (coopFrontTick): the mission is over, send our column */
void coopStatsFrontTick(void)
{
    CoopStatsMsg m;
    s32 i;

    if (!sLive)
    {
        return;
    }
    sLive = 0;
    sBlame = 0;
    memset(sPartnerBooms, 0, sizeof(sPartnerBooms));
    if (!sCoop || g_playerPerm == NULL)
    {
        return;
    }
    sSelf.v[COOPSTAT_SHOTS] = g_playerPerm->shot_count[SHOT_REGISTER_TOTAL];
    sSelf.v[COOPSTAT_HITS] = coopStatsLocalHits();
    sSelf.v[COOPSTAT_HEAD] = g_playerPerm->shot_count[SHOT_REGISTER_HEAD];
    sSelf.v[COOPSTAT_KILLS] = coopNetIsHost() ? g_playerPerm->kill_count : 0;

    memset(&m, 0, sizeof(m));
    m.stage = (s16) sStage;
    m.fromhost = (u8) (coopNetIsHost() != 0);
    for (i = 0; i < COOPSTAT_COUNT; i++)
    {
        m.own[i] = coopStatsU16(sSelf.v[i]);
    }
    if (coopNetIsHost())
    {
        m.yours[0] = coopStatsU16(sPartner.v[COOPSTAT_KILLS]);
        m.yours[1] = coopStatsU16(sPartner.v[COOPSTAT_HITS]);
        m.yours[2] = coopStatsU16(sPartner.v[COOPSTAT_HEAD]);
    }
    coopSendStatsEvent(&m, sizeof(m));
    portConLogf("ge: coop: stats sent: kills %d shots %d hits %d head %d deaths %d revived %d respawns %d%s\n",
                sSelf.v[COOPSTAT_KILLS], sSelf.v[COOPSTAT_SHOTS], sSelf.v[COOPSTAT_HITS], sSelf.v[COOPSTAT_HEAD],
                sSelf.v[COOPSTAT_DEATHS], sSelf.v[COOPSTAT_REVIVED], sSelf.v[COOPSTAT_RESPAWNS],
                coopNetIsHost() ? "" : " (kills from the host)");
    if (coopNetIsHost())
    {
        portConLogf("ge: coop: stats: partner's kills %d hits %d head %d (from its shots here)\n",
                    sPartner.v[COOPSTAT_KILLS], sPartner.v[COOPSTAT_HITS], sPartner.v[COOPSTAT_HEAD]);
    }
}

/* COOPEV_STATS from the partner */
void coopStatsApply(const void *payload)
{
    CoopStatsMsg m;
    s32 i;

    memcpy(&m, payload, sizeof(m));
    if (m.stage != sStage || !sCoop)
    {
        return;
    }
    for (i = 0; i < COOPSTAT_COUNT; i++)
    {
        if (!m.fromhost && (i == COOPSTAT_KILLS || i == COOPSTAT_HEAD))
        {
            continue;   /* the host counted those (blame) */
        }
        if (!m.fromhost && i == COOPSTAT_HITS)
        {
            sPartner.v[i] += m.own[i];  /* its own hits (objects) on top of its hits on our guards */
            continue;
        }
        sPartner.v[i] = m.own[i];
    }
    sPartnerIn = 1;
    if (m.fromhost)
    {
        sYours[0] = m.yours[0];
        sYours[1] = m.yours[1];
        sYours[2] = m.yours[2];
        sHostIn = 1;
    }
    portConLogf("ge: coop: stats from the partner: kills %d shots %d hits %d head %d deaths %d revived %d respawns %d\n",
                sPartner.v[COOPSTAT_KILLS], sPartner.v[COOPSTAT_SHOTS], sPartner.v[COOPSTAT_HITS], sPartner.v[COOPSTAT_HEAD],
                sPartner.v[COOPSTAT_DEATHS], sPartner.v[COOPSTAT_REVIVED], sPartner.v[COOPSTAT_RESPAWNS]);
}

/* 1: the debrief on screen is for a co-op mission (draw the two columns) */
s32 coopStatsShown(void)
{
    return sCoop && !sLive;   /* the next mission's start clears sCoop */
}

/* player 0 us, 1 the partner; -1 while that number has not come in */
s32 coopStatsGet(s32 player, s32 stat)
{
    if (!coopStatsShown() || stat < 0 || stat >= COOPSTAT_COUNT)
    {
        return -1;
    }
    if (player != 0)
    {
        return sPartnerIn ? sPartner.v[stat] : -1;
    }
    if (coopNetIsHost())
    {
        return sSelf.v[stat];
    }
    switch (stat)
    {   /* joiner: our hits on guards and kills were counted on the host */
        case COOPSTAT_KILLS: return sHostIn ? sYours[0] : -1;
        case COOPSTAT_HITS:  return sHostIn ? sSelf.v[stat] + sYours[1] : -1;
        case COOPSTAT_HEAD:  return sHostIn ? sSelf.v[stat] + sYours[2] : -1;
        default:             return sSelf.v[stat];
    }
}

const char *coopStatsName(s32 player)
{
    const char *n = sNames[player != 0];

    return (n[0] != '\0') ? n : (player != 0 ? "Partner" : "You");
}


/* ---- the campaign record ----------------------------------------------------
 * coop_campaign.txt, one line per partner and mission, the best difficulty:
 *     <partner key> <mission> <difficulty> <partner name>
 * The key is steam:<SteamID> over Steam, else name:<name>. */

#define COOP_CAMPAIGN_FILE  "coop_campaign.txt"
#define COOP_CAMPAIGN_MAX   256
#define COOP_MISSIONS       20

typedef struct
{
    char key[40];
    char name[32];
    s8 mission;     /* SP_LEVEL_* order, 0 Dam .. 19 Egyptian */
    s8 difficulty;
} CoopCampaignEnt;

static const char *const sMissionKeys[COOP_MISSIONS] =
{
    "dam", "facility", "runway", "surface1", "bunker1", "silo", "frigate", "surface2", "bunker2", "statue",
    "archives", "streets", "depot", "train", "jungle", "control", "caverns", "cradle", "aztec", "egyptian"
};
static const char *const sMissionNames[COOP_MISSIONS] =
{
    "Dam", "Facility", "Runway", "Surface I", "Bunker I", "Silo", "Frigate", "Surface II", "Bunker II", "Statue",
    "Archives", "Streets", "Depot", "Train", "Jungle", "Control", "Caverns", "Cradle", "Aztec", "Egyptian"
};
static const char *const sDiffKeys[DIFFICULTY_MAX] = { "agent", "secret", "00", "007" };
static const char *const sDiffNames[DIFFICULTY_MAX] = { "Agent", "Secret Agent", "00 Agent", "007" };

static CoopCampaignEnt sCampaign[COOP_CAMPAIGN_MAX];
static s32 sCampaignCount = -1;     /* -1: not read yet */

static s32 coopCampaignLookup(const char *const *keys, s32 n, const char *word)
{
    s32 i;

    for (i = 0; i < n; i++)
    {
        if (strcmp(keys[i], word) == 0)
        {
            return i;
        }
    }
    return -1;
}

static void coopCampaignLoad(void)
{
    char line[160];
    FILE *f;

    if (sCampaignCount >= 0)
    {
        return;
    }
    sCampaignCount = 0;
    f = fopen(portRootPath(COOP_CAMPAIGN_FILE), "r");
    if (f == NULL)
    {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL && sCampaignCount < COOP_CAMPAIGN_MAX)
    {
        char key[40], mission[16], diff[16];
        s32 used = 0;
        s32 m, d;
        CoopCampaignEnt *e;
        char *name;

        if (line[0] == '#' || sscanf(line, "%39s %15s %15s %n", key, mission, diff, &used) < 3)
        {
            continue;
        }
        m = coopCampaignLookup(sMissionKeys, COOP_MISSIONS, mission);
        d = coopCampaignLookup(sDiffKeys, DIFFICULTY_MAX, diff);
        if (m < 0 || d < 0)
        {
            continue;
        }
        e = &sCampaign[sCampaignCount++];
        memset(e, 0, sizeof(*e));
        strncpy(e->key, key, sizeof(e->key) - 1);
        name = line + used;
        name[strcspn(name, "\r\n")] = '\0';
        strncpy(e->name, name, sizeof(e->name) - 1);
        e->mission = (s8) m;
        e->difficulty = (s8) d;
    }
    fclose(f);
}

static void coopCampaignSave(void)
{
    FILE *f = fopen(portRootPath(COOP_CAMPAIGN_FILE), "w");
    s32 i;

    if (f == NULL)
    {
        portConLogf("ge: coop: campaign: can't write %s\n", COOP_CAMPAIGN_FILE);
        return;
    }
    fprintf(f, "# Co-op campaign: the missions finished with each partner, at the best difficulty.\n");
    fprintf(f, "# partner  mission  difficulty (agent, secret, 00, 007)  partner's name\n");
    for (i = 0; i < sCampaignCount; i++)
    {
        fprintf(f, "%s %s %s %s\n", sCampaign[i].key, sMissionKeys[(s32) sCampaign[i].mission],
                sDiffKeys[(s32) sCampaign[i].difficulty], sCampaign[i].name);
    }
    fclose(f);
}

/* the current partner's key; 0 when there is no partner */
static s32 coopCampaignKey(char *key, s32 cap)
{
    unsigned long long id = coopLobbyIsSteam() ? coopSteamPeerId() : 0;
    s32 i;

    if (!coopSessionActive())
    {
        return 0;
    }
    if (id != 0)
    {
        snprintf(key, cap, "steam:%llu", id);
        return 1;
    }
    snprintf(key, cap, "name:%s", coopLobbyPartnerName());
    for (i = 0; key[i] != '\0'; i++)
    {
        if ((u8) key[i] <= ' ' || (u8) key[i] >= 0x7F)
        {
            key[i] = '_';
        }
    }
    return 1;
}

static s32 coopCampaignMissionOf(s32 stage)
{
    s32 i;

    for (i = 0; mission_folder_setup_entries[i].folder_text_preset != 0; i++)
    {
        if (mission_folder_setup_entries[i].stage_id == stage && mission_folder_setup_entries[i].mission_num >= 0)
        {
            return mission_folder_setup_entries[i].mission_num;
        }
    }
    return -1;
}

static s32 coopCampaignStageOf(s32 mission)
{
    s32 i;

    for (i = 0; mission_folder_setup_entries[i].folder_text_preset != 0; i++)
    {
        if (mission_folder_setup_entries[i].mission_num == mission && mission_folder_setup_entries[i].stage_id >= 0)
        {
            return mission_folder_setup_entries[i].stage_id;
        }
    }
    return -1;
}

/* A co-op mission ended in a win (coopMissionComplete, both games): remember
 * it for this partner. GE_COOP_TESTWIN=1 counts any end as a win (harness only). */
void coopCampaignMissionEnd(void)
{
    char key[40];
    s32 mission = coopCampaignMissionOf(bossGetStageNum());
    s32 diff = lvlGetSelectedDifficulty();
    s32 i;
    CoopCampaignEnt *e = NULL;

    if (!sCoop || mission < 0 || mission >= COOP_MISSIONS || diff < DIFFICULTY_AGENT || diff >= DIFFICULTY_MAX
        || !coopCampaignKey(key, sizeof(key)))
    {
        return;
    }
    if (!objectiveIsAllComplete() && getenv("GE_COOP_TESTWIN") == NULL)
    {
        return;
    }
    coopCampaignLoad();
    for (i = 0; i < sCampaignCount; i++)
    {
        if (strcmp(sCampaign[i].key, key) == 0 && sCampaign[i].mission == mission)
        {
            e = &sCampaign[i];
            break;
        }
    }
    if (e == NULL)
    {
        if (sCampaignCount >= COOP_CAMPAIGN_MAX)
        {
            return;
        }
        e = &sCampaign[sCampaignCount++];
        memset(e, 0, sizeof(*e));
        strncpy(e->key, key, sizeof(e->key) - 1);
        e->mission = (s8) mission;
        e->difficulty = (s8) diff;
    }
    else if (diff > e->difficulty && diff != DIFFICULTY_007)
    {
        e->difficulty = (s8) diff;
    }
    coopStatsCopyName(e->name, sizeof(e->name), coopLobbyPartnerName());
    coopCampaignSave();
    portConLogf("ge: coop: campaign: %s (%s) done with %s [%s]\n", sMissionNames[mission], sDiffNames[diff], e->name, key);
    {
        s32 stage, d;
        char label[48];

        if (coopCampaignNext(&stage, &d, label, sizeof(label)))
        {
            portConLogf("ge: coop: campaign: next %d/%d \"%s\"\n", stage, d, label);
        }
    }
}

/* The mission after the furthest one finished with the current partner, at
 * the difficulty it was finished on. 1 and stage (LEVELID_*), difficulty and
 * a label like "Continue: Facility (Agent)"; 0 with no partner, nothing
 * finished together, or the last mission done. */
s32 coopCampaignNext(s32 *stage, s32 *difficulty, char *label, s32 labelcap)
{
    char key[40];
    s32 i;
    s32 best = -1;
    s32 diff = DIFFICULTY_AGENT;
    s32 next;

    if (!coopCampaignKey(key, sizeof(key)))
    {
        return 0;
    }
    coopCampaignLoad();
    for (i = 0; i < sCampaignCount; i++)
    {
        if (strcmp(sCampaign[i].key, key) == 0 && sCampaign[i].mission > best)
        {
            best = sCampaign[i].mission;
            diff = sCampaign[i].difficulty;
        }
    }
    if (best < 0 || best + 1 >= COOP_MISSIONS)
    {
        return 0;
    }
    next = coopCampaignStageOf(best + 1);
    if (next < 0)
    {
        return 0;
    }
    if (stage != NULL)
    {
        *stage = next;
    }
    if (difficulty != NULL)
    {
        *difficulty = diff;
    }
    if (label != NULL && labelcap > 0)
    {
        snprintf(label, labelcap, "Continue: %s (%s)", sMissionNames[best + 1], sDiffNames[diff]);
    }
    return 1;
}
