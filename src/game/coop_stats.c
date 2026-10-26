/**
 * Co-op mission stats and the co-op campaign record (port only).
 *
 * Stats: both games keep a column per player (kills, shots, hits, head hits,
 * deaths, times revived, respawns, revives given, objectives credited to them,
 * damage taken in hundredths of a health bar). Each game counts its own shots and deaths.
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
#include "port_save.h"

/* ---- mission stats --------------------------------------------------------- */

/* COOPEV_STATS payload (at most 28 bytes, see CoopEvent in coop.c) */
enum { COOPSTATMSG_OWN, COOPSTATMSG_GUARDS };
typedef struct
{
    s16 stage;
    u8 kind;        /* COOPSTATMSG_OWN: the sender's column; _GUARDS: the host's count of `about`'s kills, hits, head hits */
    u8 about;       /* _GUARDS: whose */
    u16 v[COOPSTAT_COUNT];
} CoopStatsMsg;
typedef char CoopStatsMsgFits[(sizeof(CoopStatsMsg) <= 28) ? 1 : -1];

typedef struct
{
    s32 v[COOPSTAT_COUNT];
} CoopStatsCol;

static s32 sStage = -1;         /* the mission the table is for */
static s32 sCoop;               /* a partner was in it with us */
static s32 sLive;               /* in the mission; the final column is not sent yet */
static s32 sDown;               /* this death is counted */
static f32 sLastHp = -1.0f;     /* health + armour last tick, -1 not yet */
static f32 sDamage;             /* taken so far, in health bars */
static s32 sWas[COOP_MAX_PLAYERS];          /* by player number: was in the mission */
static CoopStatsCol sOwn[COOP_MAX_PLAYERS]; /* each player's own column (ours: deaths, revived, respawns while live) */
static s32 sOwnIn[COOP_MAX_PLAYERS];
static CoopStatsCol sGuards[COOP_MAX_PLAYERS]; /* the host's count of each joiner's kills, hits and head hits on guards */
static s32 sGuardsIn[COOP_MAX_PLAYERS];
static char sNames[COOP_MAX_PLAYERS][32];
static s32 sOrder[COOP_MAX_PLAYERS];        /* the debrief's columns: player numbers */
static s32 sNumCols;

/* host: a joiner's shot or blast is being applied */
static s32 sBlame;              /* its player number, 0 none */
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
    sLastHp = -1.0f;
    sDamage = 0.0f;
    sBlame = 0;
    memset(sWas, 0, sizeof(sWas));
    memset(sOwn, 0, sizeof(sOwn));
    memset(sOwnIn, 0, sizeof(sOwnIn));
    memset(sGuards, 0, sizeof(sGuards));
    memset(sGuardsIn, 0, sizeof(sGuardsIn));
    memset(sNames, 0, sizeof(sNames));
}

/* Host migration renumbered the players (coop.c coopRenumber): their columns
 * move with them. The old host's column goes; the kills and hits it counted
 * for the joiners stay theirs, and the new host counts from here on. */
void coopStatsRemap(const s32 *newof)
{
    s32 was[COOP_MAX_PLAYERS], ownin[COOP_MAX_PLAYERS], guardsin[COOP_MAX_PLAYERS];
    CoopStatsCol own[COOP_MAX_PLAYERS], guards[COOP_MAX_PLAYERS];
    char names[COOP_MAX_PLAYERS][32];
    s32 p;

    memcpy(was, sWas, sizeof(was));
    memcpy(ownin, sOwnIn, sizeof(ownin));
    memcpy(guardsin, sGuardsIn, sizeof(guardsin));
    memcpy(own, sOwn, sizeof(own));
    memcpy(guards, sGuards, sizeof(guards));
    memcpy(names, sNames, sizeof(names));
    memset(sWas, 0, sizeof(sWas));
    memset(sOwnIn, 0, sizeof(sOwnIn));
    memset(sGuardsIn, 0, sizeof(sGuardsIn));
    memset(sOwn, 0, sizeof(sOwn));
    memset(sGuards, 0, sizeof(sGuards));
    memset(sNames, 0, sizeof(sNames));
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        s32 q = newof[p];

        if (q < 0 || q >= COOP_MAX_PLAYERS)
        {
            continue;
        }
        sWas[q] = was[p];
        sOwnIn[q] = ownin[p];
        sGuardsIn[q] = guardsin[p];
        sOwn[q] = own[p];
        sGuards[q] = guards[p];
        memcpy(sNames[q], names[p], sizeof(sNames[q]));
    }
    sBlame = 0;
}

/* every mission tick (coopTick); partnerhere: a partner is in this stage */
void coopStatsMissionTick(s32 partnerhere)
{
    CoopHudPartner p;
    s32 i;

    if (!sLive)
    {
        return;
    }
    if (partnerhere)
    {
        const s32 self = coopLobbySelfPlayer();

        if (!sCoop)
        {
            sCoop = 1;
            sWas[self] = 1;
            coopStatsCopyName(sNames[self], sizeof(sNames[self]), coopLobbySelfName());
        }
        for (i = 0; coopHudPartnerAt(i, &p); i++)
        {
            if (!sWas[p.player])
            {
                sWas[p.player] = 1;
                coopStatsCopyName(sNames[p.player], sizeof(sNames[p.player]), p.name);
            }
        }
    }
    if (!g_CurrentPlayer->bonddead)
    {
        sDown = 0;
    }
    {   /* damage taken: every drop in health and armour (a respawn or a pickup only adds) */
        f32 hp = g_CurrentPlayer->bondhealth;

        hp = ((hp > 0.0f) ? hp : 0.0f) + ((g_CurrentPlayer->bondarmour > 0.0f) ? g_CurrentPlayer->bondarmour : 0.0f);

        if (sLastHp >= 0.0f && hp < sLastHp)
        {
            sDamage += sLastHp - hp;
        }
        sLastHp = hp;
        sOwn[coopLobbySelfPlayer()].v[COOPSTAT_DAMAGE] = (s32) (sDamage * 100.0f + 0.5f);
    }
}

/* our Bond is dead (coopDeadTick, every tick until the respawn or the end) */
void coopStatsDown(void)
{
    if (sLive && !sDown)
    {
        sDown = 1;
        sOwn[coopLobbySelfPlayer()].v[COOPSTAT_DEATHS]++;
    }
}

void coopStatsNote(s32 stat)
{
    if (sLive && stat >= 0 && stat < COOPSTAT_COUNT)
    {
        sOwn[coopLobbySelfPlayer()].v[stat]++;
    }
}

/* Host: what the code between Begin and End adds to our kills and hit
 * registers was that joiner's doing. Calls do not nest. */
void coopStatsBlameBegin(s32 player)
{
    if (player <= 0 || player >= COOP_MAX_PLAYERS || sBlame || !sLive || g_playerPerm == NULL)
    {
        return;
    }
    sBlame = player;
    sBlameKills = g_playerPerm->kill_count;
    memcpy(sBlameShots, g_playerPerm->shot_count, sizeof(sBlameShots));
}

void coopStatsBlameEnd(void)
{
    CoopStatsCol *g;
    s32 i;
    s32 hits = 0;

    if (!sBlame)
    {
        return;
    }
    g = &sGuards[sBlame];
    sBlame = 0;
    g->v[COOPSTAT_KILLS] += g_playerPerm->kill_count - sBlameKills;
    g_playerPerm->kill_count = sBlameKills;
    for (i = SHOT_REGISTER_HEAD; i <= SHOT_REGISTER_OBJECT; i++)
    {
        hits += g_playerPerm->shot_count[i] - sBlameShots[i];
    }
    g->v[COOPSTAT_HITS] += hits;
    g->v[COOPSTAT_HEAD] += g_playerPerm->shot_count[SHOT_REGISTER_HEAD] - sBlameShots[SHOT_REGISTER_HEAD];
    memcpy(g_playerPerm->shot_count, sBlameShots, sizeof(sBlameShots));
}

/* explosion.c: the blasts coopApplyExplosion sets off for a joiner */
#define COOP_STATS_BOOMS 8
static void *sPartnerBooms[COOP_STATS_BOOMS];
static s32 sPartnerBoomBy[COOP_STATS_BOOMS];

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
    for (i = 0; i < COOP_STATS_BOOMS && sPartnerBooms[i] != NULL; i++)
    {
    }
    if (i == COOP_STATS_BOOMS)
    {
        i = 0;
    }
    sPartnerBooms[i] = explosion;
    sPartnerBoomBy[i] = coopApplyingPartnerExplosion();
}

/* whose blast this is: a joiner's player number, 0 ours */
s32 coopStatsExplosionIsPartner(void *explosion)
{
    s32 i;

    for (i = 0; explosion != NULL && i < COOP_STATS_BOOMS; i++)
    {
        if (sPartnerBooms[i] == explosion)
        {
            return sPartnerBoomBy[i];
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

static void coopStatsSend(s32 kind, s32 about, CoopStatsCol *col)
{
    CoopStatsMsg m;
    s32 i;

    memset(&m, 0, sizeof(m));
    m.stage = (s16) sStage;
    m.kind = (u8) kind;
    m.about = (u8) about;
    for (i = 0; i < COOPSTAT_COUNT; i++)
    {
        m.v[i] = coopStatsU16(col->v[i]);
    }
    coopSendStatsEvent(&m, sizeof(m));
}

/* every frontend tick (coopFrontTick): the mission is over, send our column */
void coopStatsFrontTick(void)
{
    const s32 self = coopLobbySelfPlayer();
    CoopStatsCol *own = &sOwn[self];
    s32 p;

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
    own->v[COOPSTAT_SHOTS] = g_playerPerm->shot_count[SHOT_REGISTER_TOTAL];
    own->v[COOPSTAT_HITS] = coopStatsLocalHits();
    own->v[COOPSTAT_HEAD] = g_playerPerm->shot_count[SHOT_REGISTER_HEAD];
    own->v[COOPSTAT_KILLS] = coopNetIsHost() ? g_playerPerm->kill_count : 0;
    sOwnIn[self] = 1;
    coopStatsSend(COOPSTATMSG_OWN, self, own);
    portConLogf("ge: coop: stats sent: kills %d shots %d hits %d head %d deaths %d revived %d respawns %d revives %d objectives %d damage %d%s\n",
                own->v[COOPSTAT_KILLS], own->v[COOPSTAT_SHOTS], own->v[COOPSTAT_HITS], own->v[COOPSTAT_HEAD],
                own->v[COOPSTAT_DEATHS], own->v[COOPSTAT_REVIVED], own->v[COOPSTAT_RESPAWNS],
                own->v[COOPSTAT_REVIVES], own->v[COOPSTAT_OBJECTIVES], own->v[COOPSTAT_DAMAGE],
                coopNetIsHost() ? "" : " (kills from the host)");
    if (coopNetIsHost())
    {
        for (p = 1; p < COOP_MAX_PLAYERS; p++)
        {
            if (sWas[p])
            {
                sGuardsIn[p] = 1;
                coopStatsSend(COOPSTATMSG_GUARDS, p, &sGuards[p]);
                portConLogf("ge: coop: stats: player %d's kills %d hits %d head %d (from its shots here)\n", p + 1,
                            sGuards[p].v[COOPSTAT_KILLS], sGuards[p].v[COOPSTAT_HITS], sGuards[p].v[COOPSTAT_HEAD]);
            }
        }
    }
}

/* COOPEV_STATS from player `from` */
void coopStatsApply(const void *payload, s32 from)
{
    CoopStatsMsg m;
    s32 i;

    memcpy(&m, payload, sizeof(m));
    if (m.stage != sStage || !sCoop || from < 0 || from >= COOP_MAX_PLAYERS)
    {
        return;
    }
    if (m.kind == COOPSTATMSG_GUARDS)
    {
        if (from != 0 || m.about >= COOP_MAX_PLAYERS)
        {
            return;   /* only the host counts guards */
        }
        for (i = 0; i < COOPSTAT_COUNT; i++)
        {
            sGuards[m.about].v[i] = m.v[i];
        }
        sGuardsIn[m.about] = 1;
        return;
    }
    for (i = 0; i < COOPSTAT_COUNT; i++)
    {
        sOwn[from].v[i] = m.v[i];
    }
    sOwnIn[from] = 1;
    sWas[from] = 1;
    portConLogf("ge: coop: stats from player %d: kills %d shots %d hits %d head %d deaths %d revived %d respawns %d revives %d objectives %d damage %d\n",
                from + 1, m.v[COOPSTAT_KILLS], m.v[COOPSTAT_SHOTS], m.v[COOPSTAT_HITS], m.v[COOPSTAT_HEAD],
                m.v[COOPSTAT_DEATHS], m.v[COOPSTAT_REVIVED], m.v[COOPSTAT_RESPAWNS],
                m.v[COOPSTAT_REVIVES], m.v[COOPSTAT_OBJECTIVES], m.v[COOPSTAT_DAMAGE]);
}

/* 1: the debrief on screen is for a co-op mission (draw the columns) */
s32 coopStatsShown(void)
{
    return sCoop && !sLive;   /* the next mission's start clears sCoop */
}

/* the debrief's columns: every player who was in the mission, by number */
s32 coopStatsColumns(void)
{
    s32 p;

    sNumCols = 0;
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        if (sWas[p])
        {
            sOrder[sNumCols++] = p;
        }
    }
    return sNumCols;
}

/* column col (coopStatsColumns); -1 while that number has not come in */
s32 coopStatsGet(s32 col, s32 stat)
{
    s32 p;

    if (!coopStatsShown() || stat < 0 || stat >= COOPSTAT_COUNT || col < 0 || col >= sNumCols)
    {
        return -1;
    }
    p = sOrder[col];
    if (!sOwnIn[p])
    {
        return -1;
    }
    if (p == 0)
    {
        return sOwn[p].v[stat];   /* the host counted its own guards */
    }
    switch (stat)
    {   /* a joiner's hits on guards and kills were counted on the host */
        case COOPSTAT_KILLS: return sGuardsIn[p] ? sGuards[p].v[stat] : -1;
        case COOPSTAT_HITS:
        case COOPSTAT_HEAD:  return sGuardsIn[p] ? sOwn[p].v[stat] + sGuards[p].v[stat] : -1;
        default:             return sOwn[p].v[stat];
    }
}

s32 coopStatsSelf(s32 stat)
{
    const s32 self = coopLobbySelfPlayer();
    s32 col;

    for (col = 0; col < coopStatsColumns(); col++)
    {
        if (sOrder[col] == self)
        {
            return coopStatsGet(col, stat);
        }
    }
    return -1;
}

const char *coopStatsName(s32 col)
{
    s32 p = (col >= 0 && col < sNumCols) ? sOrder[col] : 0;

    return (sNames[p][0] != '\0') ? sNames[p] : "Player";
}


/* ---- the campaign record ----------------------------------------------------
 * coop_campaign.txt, one line per partner and mission, the best difficulty:
 *     <partner key> <mission> <difficulty> <partner name>
 * The key is steam:<SteamID> over Steam, else name:<name>. */

#define COOP_CAMPAIGN_FILE  "coop_campaign.txt"
#define COOP_CAMPAIGN_VERSION 2     /* 1: beside the game, no header (port_save.h) */
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
    int version;

    if (sCampaignCount >= 0)
    {
        return;
    }
    sCampaignCount = 0;
    /* v1 sat beside the game with no header; v2 is in Data/Saves. The lines
     * are the same in both. */
    f = portSaveOpenRead(COOP_CAMPAIGN_FILE, COOP_CAMPAIGN_FILE, &version);
    if (f == NULL)
    {
        return;
    }
    if (version > COOP_CAMPAIGN_VERSION)
    {
        portConLogf("ge: coop: campaign: %s is from a newer build (v%d); reading what this one knows\n",
                    COOP_CAMPAIGN_FILE, (int) version);
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

static u8 sCampaignSent[COOP_MAX_PLAYERS];  /* host: told this player our furthest (cross-save) */

static void coopCampaignSave(void)
{
    FILE *f = portSaveOpenWrite(COOP_CAMPAIGN_FILE, "coop-campaign", COOP_CAMPAIGN_VERSION);
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
    portSaveCommit(f);
}

/* partner p's key; 0 when p is not one */
static s32 coopCampaignKey(s32 p, char *key, s32 cap)
{
    unsigned long long id = coopPlayerId(p);
    s32 i;

    if (!coopSessionActive() || p == coopLobbySelfPlayer() || !coopLobbyPlayerHere(p))
    {
        return 0;
    }
    if (id != 0)
    {
        snprintf(key, cap, "steam:%llu", id);
        return 1;
    }
    snprintf(key, cap, "name:%s", coopPlayerName(p));
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

/* a solo mission's name ("Dam", "Surface II"), NULL for a stage that isn't one (rich presence) */
const char *coopMissionName(s32 stage)
{
    const s32 m = coopCampaignMissionOf(stage);

    return (m >= 0 && m < COOP_MISSIONS) ? sMissionNames[m] : NULL;
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

/* A co-op mission ended in a win (coopMissionComplete, every game): remember
 * it for each partner. GE_COOP_TESTWIN=1 counts any end as a win (harness only). */
void coopCampaignMissionEnd(void)
{
    char key[40];
    s32 mission = coopCampaignMissionOf(bossGetStageNum());
    s32 diff = lvlGetSelectedDifficulty();
    s32 i, p;
    s32 saved = 0;

    if (!sCoop || mission < 0 || mission >= COOP_MISSIONS || diff < DIFFICULTY_AGENT || diff >= DIFFICULTY_MAX)
    {
        return;
    }
    if (!objectiveIsAllComplete() && getenv("GE_COOP_TESTWIN") == NULL)
    {
        return;
    }
    coopCampaignLoad();
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        CoopCampaignEnt *e = NULL;

        if (!sWas[p] || !coopCampaignKey(p, key, sizeof(key)))
        {
            continue;
        }
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
                continue;
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
        coopStatsCopyName(e->name, sizeof(e->name), coopPlayerName(p));
        saved = 1;
        portConLogf("ge: coop: campaign: %s (%s) done with %s [%s]\n", sMissionNames[mission], sDiffNames[diff], e->name, key);
    }
    if (!saved)
    {
        return;
    }
    coopCampaignSave();
    memset(sCampaignSent, 0, sizeof(sCampaignSent));   /* host: the joiners hear the new furthest */
    {
        s32 stage, d;
        char label[48];

        if (coopCampaignNext(&stage, &d, label, sizeof(label)))
        {
            portConLogf("ge: coop: campaign: next %d/%d \"%s\"\n", stage, d, label);
        }
    }
}

/* Cross-save: the campaign is the host's. Our own progress is the furthest
 * mission in coop_campaign.txt with anyone (lines inherited from a host
 * count too). The host's Continue is the mission after its own furthest,
 * so a partner new to the campaign joins it where the host is; the host
 * sends its furthest to each joiner (COOPEV_CAMPAIGN), which shows it on its
 * lobby and keeps it as a line under the host's key, so it carries the
 * progress on when it hosts itself. */
typedef struct
{
    s8 mission;     /* the host's furthest, -1 none */
    s8 difficulty;
    u8 pad[2];
} CoopCampaignMsg;

static s32 sHostBest = -1;      /* joiner: the host's furthest, as it sent it */
static s32 sHostBestDiff;
static s32 sInheritPending;     /* joiner: sHostBest is not in our record yet */
static void coopCampaignInherit(void);

/* our furthest mission in the record, with anyone; -1 none */
static s32 coopCampaignBest(s32 *difficulty)
{
    s32 best = -1, diff = DIFFICULTY_AGENT;
    s32 i;

    coopCampaignLoad();
    for (i = 0; i < sCampaignCount; i++)
    {
        if (sCampaign[i].mission > best
            || (sCampaign[i].mission == best && sCampaign[i].difficulty > diff && sCampaign[i].difficulty != DIFFICULTY_007))
        {
            best = sCampaign[i].mission;
            diff = sCampaign[i].difficulty;
        }
    }
    if (difficulty != NULL)
    {
        *difficulty = diff;
    }
    return best;
}

/* The mission after the campaign's furthest (the host's: ours when we host,
 * the one it sent when we joined), at the difficulty it was finished on. 1
 * and stage (LEVELID_*), difficulty and a label like "Continue: Facility
 * (Agent)"; 0 with no partner, nothing finished, or the last mission done. */
s32 coopCampaignNext(s32 *stage, s32 *difficulty, char *label, s32 labelcap)
{
    s32 best, diff = DIFFICULTY_AGENT;
    s32 next;

    if (!coopLobbyHasPartner())
    {
        return 0;
    }
    if (coopLobbyIsHost())
    {
        best = coopCampaignBest(&diff);
    }
    else
    {
        best = sHostBest;
        diff = sHostBestDiff;
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

/* host: our furthest to each joiner once it is here, and again after a win */
void coopCampaignTick(void)
{
    s32 p;

    if (sInheritPending && !coopExtIsHost())
    {
        coopCampaignInherit();
    }
    if (!coopExtIsHost() || !coopSessionActive())
    {
        memset(sCampaignSent, 0, sizeof(sCampaignSent));
        return;
    }
    for (p = 1; p < COOP_MAX_PLAYERS; p++)
    {
        if (!coopLobbyPlayerHere(p))
        {
            sCampaignSent[p] = 0;
        }
        else if (!sCampaignSent[p])
        {
            CoopCampaignMsg m;
            s32 diff;

            memset(&m, 0, sizeof(m));
            m.mission = (s8) coopCampaignBest(&diff);
            m.difficulty = (s8) diff;
            coopExtSend(COOPEV_CAMPAIGN, p, &m, sizeof(m));
            sCampaignSent[p] = 1;
            portConLogf("ge: coop: campaign: told player %d we are at %d (%s)\n", p + 1, m.mission,
                        (m.mission >= 0) ? sMissionNames[(s32) m.mission] : "nothing yet");
        }
    }
}

/* joiner: keep the host's furthest as ours too, under the host's key */
static void coopCampaignInherit(void)
{
    char key[40];
    CoopCampaignEnt *e = NULL;
    s32 i;

    if (sHostBest < 0 || !coopCampaignKey(0, key, sizeof(key)))
    {
        return;     /* nothing, or the host's hello is not in yet: next tick */
    }
    sInheritPending = 0;
    coopCampaignLoad();
    for (i = 0; i < sCampaignCount; i++)
    {
        if (strcmp(sCampaign[i].key, key) == 0 && sCampaign[i].mission == sHostBest)
        {
            e = &sCampaign[i];
        }
    }
    if (e != NULL && e->difficulty >= sHostBestDiff)
    {
        return;     /* we have it */
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
        e->mission = (s8) sHostBest;
    }
    e->difficulty = (s8) sHostBestDiff;
    coopStatsCopyName(e->name, sizeof(e->name), coopPlayerName(0));
    coopCampaignSave();
    portConLogf("ge: coop: campaign: inherited %s (%s) from the host\n", sMissionNames[sHostBest], sDiffNames[sHostBestDiff]);
}

void coopCampaignApply(const u8 *raw, s32 from)
{
    CoopCampaignMsg m;

    memcpy(&m, raw, sizeof(m));
    if (from != 0 || coopExtIsHost() || m.mission >= COOP_MISSIONS || m.difficulty < DIFFICULTY_AGENT
        || m.difficulty >= DIFFICULTY_MAX)
    {
        return;
    }
    sHostBest = m.mission;
    sHostBestDiff = m.difficulty;
    portConLogf("ge: coop: campaign: the host is at %d (%s)\n", m.mission, (m.mission >= 0) ? sMissionNames[(s32) m.mission] : "nothing yet");
    sInheritPending = 1;
    coopCampaignInherit();
}

