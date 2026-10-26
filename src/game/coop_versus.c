/**
 * Online versus: GoldenEye's own multiplayer (its arenas, scenarios, weapon
 * sets and characters) played over the co-op session, one player per PC,
 * each on a full screen instead of a quarter of one.
 *
 * Every game loads the same N-player match (front.c coopVsStartMatch) with
 * itself as the first player, local index 0, so the keyboard, mouse and first
 * pad stay where they always are. The other players' structs exist as they
 * do in split screen, but they are never ticked and get no view (lv.c): their
 * bodies are moved from each player's STATE with the co-op puppet code
 * (coop_look.c), and the game's own multiplayer rules run over all of them:
 * scores, scenarios, limits, the radar, respawns and the end screens.
 *
 * Who decides what:
 *   - a bullet: the shooter's game. A hit on another player's body goes to
 *     that player (VSHIT), whose game applies it with record_damage_kills as
 *     if the shooter were sitting beside them.
 *   - a blast: each game for its own player. Grenades, mines and rockets
 *     are set off on every game with their owner (VSBOOM).
 *   - a death: the victim's game, which counts it the game's way and tells
 *     everyone (VSKILL), so every game keeps the same kill table.
 *   - Flag Tag's flag and the Golden Gun: whoever picks one up says so
 *     (VSITEM) and it leaves the other worlds; a holder's death drops it
 *     where they fell on every game.
 *   - the end: any game whose limits end the match ends it for all (VSEND).
 *
 * Player numbers on the wire are the session's (0 the host); g_playerPointers
 * indices are local, and differ per game. sVsNet / sVsLocal map between them.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include <boss.h>
#include <joy.h>
#include "bondview.h"
#include "bondinv.h"
#include "gun.h"
#include "chr.h"
#include "chraction.h"
#include "model.h"
#include "stan.h"
#include "player.h"
#include "propobj.h"
#include "explosion.h"
#include "front.h"
#include "coop.h"
#include "coop_net.h"
#include "math_atan2f.h"

extern void portConLogf(const char *fmt, ...);
extern float sinf(float);
extern float cosf(float);
extern float sqrtf(float);
extern void hudmsgBottomShow(char *mess);
extern s32 g_stopPlayFlag;
extern s32 g_gameOverFlag;
extern void increment_num_deaths(void);
extern void increment_num_suicides_display_MP(void);
extern void increment_num_kills_display_text_in_MP(void);
extern void increment_num_times_killed_MwtGC(void);
extern void mpCalculateAwards(s32 arg);
extern s32 bondinvHasGoldenGun(void);
extern s32 bondinvIsAliveWithFlag(void);
extern PROP getPropForHeldItem(ITEM_IDS item);
extern void objFreePermanently(ObjectRecord *obj, bool freeprop);
extern void chrDetectRooms(ChrRecord *chr);
extern s32 walkTilesBetweenPoints_NoCallback(StandTile **tile, f32 x1, f32 z1, f32 x2, f32 z2);
extern void bondinvRemoveItemByID(ITEM_IDS item);
extern int bondinvHasInvItem(ITEM_IDS item);
extern PropRecord *g_ActivePropsHead;

#define VS_DEAD_SETTLE  120     /* ticks a remote death plays before the game counts it as finished */
#define VS_PKT_EVERY    12      /* ticks between our VERSUS packets (5 a second) */

/* a VERSUS packet: what the STATE does not carry, both ways five times a second */
typedef struct
{
    u32 magic;
    u16 version;
    u16 type;           /* COOPPKT_VERSUS */
    u8 player;          /* the sender's (the host stamps a joiner's before relaying) */
    u8 holds;           /* Flag Tag's flag or the Golden Gun is in their hands */
    u8 pad[2];
    u32 serial;         /* the match */
    s32 flagticks;      /* Flag Tag: the time they have held the flag (flag_counter) */
} CoopVsPkt;

static s32 sVs;                         /* a versus match is set up, or being played */
static u32 sVsSerial;                   /* the match's number, the same on every game */
static s32 sVsCount;                    /* players in it */
static s32 sVsNet[MAX_PLAYER_COUNT];    /* local index -> session player number */
static s32 sVsLocal[COOP_MAX_PLAYERS];  /* session player number -> local index, -1 not playing */
static CoopLookPuppet sVsLook[MAX_PLAYER_COUNT];
static s32 sVsDeadTicks[MAX_PLAYER_COUNT];  /* a remote's death, ticks so far */
static s32 sVsSpawned[MAX_PLAYER_COUNT];    /* the first start pad was handed out */
static s32 sVsHeard[MAX_PLAYER_COUNT];      /* their STATE has come in this match */
static s32 sVsEnded;
static s32 sVsInBlast;                  /* explosion.c: damage from a blast is being dealt */
static s32 sVsApplying;                 /* a remote event is being applied */
static u32 sVsTick;
static s32 sVsHuntAt = -2;    /* harness (coopVsHarness) */
static s32 sVsHuntHits;     /* harness: hits we dealt since its last look round */

/* ---- who is who ------------------------------------------------------------ */

s32 coopVersus(void)
{
    return sVs && sVsCount >= 2 && bossGetStageNum() != LEVELID_TITLE && getPlayerCount() == sVsCount;
}

s32 coopVsViewCount(void)
{
    return coopVersus() ? 1 : getPlayerCount();
}

s32 coopVsRemote(s32 li)
{
    return coopVersus() && li > 0 && li < sVsCount;
}

s32 coopVsNetOfLocal(s32 li)
{
    return (li >= 0 && li < sVsCount) ? sVsNet[li] : -1;
}

s32 coopVsLocalOfNet(s32 p)
{
    return (p >= 0 && p < COOP_MAX_PLAYERS) ? sVsLocal[p] : -1;
}

s32 coopVsPlayers(void)
{
    return sVsCount;
}

/* Set up who plays: the session's players in `mask` (bit p = player p), we
 * first, then the rest in session order after us, wrapping round, so the
 * order is the same circle on every game. */
s32 coopVersusRoster(u32 mask, u32 serial)
{
    s32 self = coopLobbySelfPlayer();
    s32 k, p;

    memset(sVsLocal, -1, sizeof(sVsLocal));
    sVsCount = 0;
    for (k = 0; k < COOP_MAX_PLAYERS; k++)
    {
        p = (self + k) % COOP_MAX_PLAYERS;
        if ((mask & (1u << p)) && sVsCount < MAX_PLAYER_COUNT)
        {
            sVsLocal[p] = sVsCount;
            sVsNet[sVsCount++] = p;
        }
    }
    if (sVsLocal[self] != 0)
    {
        sVsCount = 0;   /* we are not in it */
    }
    sVsSerial = serial;
    sVs = sVsCount >= 2;
    memset(sVsSpawned, 0, sizeof(sVsSpawned));  /* (the stage's own spawns come before coopVersusStageStart) */
    portConLogf("ge: versus: match %u, %d players, mask %x, we are player %d\n", serial, sVsCount, mask, self + 1);
    return sVs;
}

/* host migration renumbered the session (coop.c coopRenumber): our map
 * follows; a player who left keeps their slot here and their body stays still */
void coopVersusRenumber(const s32 *newof)
{
    s32 li;

    if (!sVs)
    {
        return;
    }
    memset(sVsLocal, -1, sizeof(sVsLocal));
    for (li = 0; li < sVsCount; li++)
    {
        const s32 p = sVsNet[li];

        if (p >= 0 && p < COOP_MAX_PLAYERS && newof[p] >= 0)
        {
            sVsNet[li] = newof[p];
            sVsLocal[newof[p]] = li;
        }
        else
        {
            sVsNet[li] = COOP_MAX_PLAYERS;  /* gone: no session number answers to it */
        }
    }
    portConLogf("ge: versus: players renumbered after the host left\n");
}

void coopVersusClear(void)
{
    sVs = 0;
    sVsCount = 0;
}

u32 coopVersusSerial(void)
{
    return sVsSerial;
}

/* ---- the stage ----------------------------------------------------------------- */

void coopVersusStageStart(void)
{
    s32 i;

    if (!sVs || bossGetStageNum() == LEVELID_TITLE)
    {
        return;
    }
    for (i = 0; i < MAX_PLAYER_COUNT; i++)
    {
        coopLookReset(&sVsLook[i]);
        sVsDeadTicks[i] = 0;
        sVsHeard[i] = 0;
    }
    sVsEnded = 0;
    sVsTick = 0;
    sVsHuntAt = -2;     /* (the harness reads its settings again) */
}

/* bondview.c: the first start pad of each player, the same on every game
 * (Rare's walk would put everyone's own player on the same pad) */
s32 coopVersusStartPad(s32 li, s32 count)
{
    if (!coopVersus() || li < 0 || li >= sVsCount || sVsSpawned[li] || count <= 0)
    {
        return -1;
    }
    sVsSpawned[li] = 1;
    return (s32) ((sVsSerial * 5u + (u32) sVsNet[li] * ((count >= 8) ? (u32) (count / 4) : 1u)) % (u32) count);
}

/* the body of local player li moves to where their game says it is */
static void coopVsMoveBody(s32 li, const CoopRemoteInfo *info)
{
    struct player *pl = g_playerPointers[li];
    PropRecord *prop = pl->prop;
    ChrRecord *chr = prop->chr;
    coord3d pos;
    StandTile *stan = prop->stan;

    pos.f[0] = info->pos[0];
    pos.f[1] = info->pos[1];
    pos.f[2] = info->pos[2];
    if (stan == NULL
        || (pos.f[0] - prop->pos.f[0]) * (pos.f[0] - prop->pos.f[0]) + (pos.f[2] - prop->pos.f[2]) * (pos.f[2] - prop->pos.f[2]) > 300.0f * 300.0f
        || !walkTilesBetweenPoints_NoCallback(&stan, prop->pos.f[0], prop->pos.f[2], pos.f[0], pos.f[2]) || stan == NULL)
    {   /* a respawn (a walk along the old floor could end on another storey) */
        stan = stanFindTileBelowPos(&pos, NULL, NULL);
    }
    if (stan == NULL)
    {
        return;     /* nowhere to stand here yet */
    }
    prop->stan = stan;
    prop->pos = pos;
    if (chr->model != NULL)
    {
        setsuboffset(chr->model, &pos);
        setsubroty(chr->model, info->yaw);
        sub_GAME_7F01FC10(chr->model, &prop->pos, &prop->pos, &chr->ground);
    }
    chrDetectRooms(chr);
    pl->bondprevpos = pos;
    pl->field_488.collision_position = pos;     /* where the multiplayer body code puts the body back */
    pl->field_488.current_tile_ptr = stan;
}

static void coopVsRemoteTick(s32 li)
{
    struct player *pl = g_playerPointers[li];
    CoopRemoteInfo info;
    s32 p = sVsNet[li];

    if (pl == NULL || pl->prop == NULL || pl->prop->chr == NULL)
    {
        return;
    }
    pl->cameramode = 0;     /* normal first-person play: the body is drawn for us (chr.c), as another player's */
    pl->prop->chr->chrnum = COOP_PUPPET_CHRNUM + p;
    if (!coopRemoteInfo(p, &info))
    {
        return;
    }
    if (!sVsHeard[li])
    {
        sVsHeard[li] = 1;
        portConLogf("ge: versus: player %d (local %d) is in the arena\n", p + 1, li + 1);
    }
    coopVsMoveBody(li, &info);
    pl->bondhealth = info.health;
    pl->bondarmour = info.armour;
    if (info.dead)
    {
        if (!pl->bonddead)
        {
            sVsDeadTicks[li] = 0;
        }
        pl->bonddead = TRUE;
        if (++sVsDeadTicks[li] > VS_DEAD_SETTLE)
        {   /* the end-of-match checks wait for these (lv.c) */
            pl->redbloodfinished = TRUE;
            pl->deathanimfinished = TRUE;
            pl->colourfadetimemax60 = -1.0f;
        }
    }
    else if (pl->bonddead)
    {
        pl->bonddead = FALSE;
        pl->redbloodfinished = FALSE;
        pl->deathanimfinished = FALSE;
        pl->colourfadetimemax60 = -1.0f;
        coopLookReset(&sVsLook[li]);
    }
    coopLookPuppetTick(&sVsLook[li], pl->prop->chr, &info.look, info.dead, COOP_PUPPET_CHRNUM + sVsNet[0]);
}

/* ---- events -------------------------------------------------------------------- */

typedef struct { f32 damage; f32 vx; f32 vz; u8 armour; u8 pad[3]; } VsHit;
typedef struct { u8 victim; u8 killer; u8 gg; u8 held; f32 pos[3]; } VsKill;
typedef struct { u8 what; u8 item; u8 pad[2]; } VsItem;
typedef struct { f32 pos[3]; s16 type; u8 rooms[8]; u8 pad[2]; } VsBoom;

enum { VSITEM_PICKED = 1 };

static void coopVsSend(s32 kind, s32 to, const void *payload, s32 len)
{
    coopSendVersusEvent(kind, to, payload, len);
}

/* record_damage_kills, before anything: damage to another player's body is
 * not ours to deal. A bullet goes to them; a blast they get from their own
 * copy of it. Returns 1 to stop there. */
s32 coopVersusDamage(f32 damage, f32 vx, f32 vz, s32 attacker, s32 armour)
{
    s32 victim = get_cur_playernum();
    VsHit h;

    if (!coopVersus() || sVsApplying || victim == 0)
    {
        return 0;
    }
    if (sVsInBlast || attacker != 0 || g_playerPointers[victim]->bonddead || g_stopPlayFlag || g_gameOverFlag
        || sVsNet[victim] >= COOP_MAX_PLAYERS)
    {
        return 1;
    }
    memset(&h, 0, sizeof(h));
    h.damage = damage;
    h.vx = vx;
    h.vz = vz;
    h.armour = (u8) (armour != 0);
    coopVsSend(COOPEV_VSHIT, sVsNet[victim], &h, sizeof(h));
    portConLogf("ge: versus: hit player %d for %.3f\n", sVsNet[victim] + 1, damage);
    sVsHuntHits++;
    return 1;
}

void coopVersusBlast(s32 on)
{
    sVsInBlast = on;
}

/* record_damage_kills, our player just died at `killer`'s hand (local
 * indices): the kill table is already counted here, now everywhere */
void coopVersusKilled(s32 victim, s32 killer, s32 gg)
{
    VsKill k;
    PropRecord *bond;

    if (!coopVersus() || victim != 0)   /* (also while applying their hit: the death is still ours to tell) */
    {
        return;
    }
    memset(&k, 0, sizeof(k));
    k.victim = (u8) sVsNet[0];
    k.killer = (u8) ((killer >= 0 && killer < sVsCount) ? sVsNet[killer] : 0xFF);
    k.gg = (u8) gg;
    k.held = (u8) ((get_scenario() == SCENARIO_TLD && bondinvIsAliveWithFlag()) || (get_scenario() == SCENARIO_MWTGG && bondinvHasGoldenGun()));
    bond = g_playerPointers[0]->prop;
    k.pos[0] = bond->pos.f[0];
    k.pos[1] = bond->pos.f[1];
    k.pos[2] = bond->pos.f[2];
    coopVsSend(COOPEV_VSKILL, COOP_ALL, &k, sizeof(k));
    portConLogf("ge: versus: killed by player %d\n", (k.killer == 0xFF) ? 0 : k.killer + 1);
}

/* the scenario's item (Flag Tag's flag, the Golden Gun) */
static s32 coopVsScenarioItem(void)
{
    switch (get_scenario())
    {
        case SCENARIO_TLD:   return ITEM_TOKEN;
        case SCENARIO_MWTGG: return ITEM_GOLDENGUN;
        default:             return -1;
    }
}

/* take every loose copy of `item` out of this world (it is in someone's hands) */
/* the scenario item lying loose in the arena, if it is */
static PropRecord *coopVsFindLoose(s32 item)
{
    PropRecord *prop;

    for (prop = g_ActivePropsHead; prop != NULL; prop = prop->next)
    {
        if (prop->type == PROP_TYPE_WEAPON && prop->weapon != NULL && prop->weapon->weaponnum == item
            && prop->parent == NULL)
        {
            return prop;
        }
    }
    return NULL;
}

static void coopVsRemoveLoose(s32 item)
{
    PropRecord *prop;
    s32 removed = 0;

    for (prop = g_ActivePropsHead; prop != NULL; prop = prop->next)
    {
        if (prop->type == PROP_TYPE_WEAPON && prop->weapon != NULL && prop->weapon->weaponnum == item
            && prop->parent == NULL)
        {
            objFreePermanently((ObjectRecord *) prop->weapon, TRUE);
            removed++;
            break;  /* the list changed under us; one at a time */
        }
    }
    if (removed)
    {
        coopVsRemoveLoose(item);
    }
}

/* a dead holder's item falls where they died, from their body */
static void coopVsDropAt(s32 li, s32 item)
{
    ChrRecord *chr = g_playerPointers[li]->prop->chr;
    PropRecord *prop;
    s32 propid = (s32) getPropForHeldItem(item);

    if (chr == NULL || propid < 0)
    {
        return;
    }
    prop = something_with_generating_object(chr, (PROP) propid, item, 0x20000000, NULL, NULL);
    if (prop != NULL)
    {
        propobjSetDropped(prop, DROPTYPE_DEFAULT);
        objDrop(prop);
        portConLogf("ge: versus: player %d dropped item %d\n", sVsNet[li] + 1, item);
    }
}

static void coopVsApplyKill(const VsKill *k)
{
    s32 lv = coopVsLocalOfNet(k->victim);
    s32 lk = (k->killer == 0xFF) ? lv : coopVsLocalOfNet(k->killer);
    s32 cur = get_cur_playernum();

    if (lv <= 0 || lk < 0)
    {
        return;
    }
    if (lk != lv)
    {
        set_cur_player(lv);
        increment_num_deaths();
    }
    set_cur_player(lk);
    if (lk == lv)
    {
        increment_num_suicides_display_MP();
    }
    else
    {
        increment_num_kills_display_text_in_MP();
        if (k->gg)
        {
            increment_num_times_killed_MwtGC();
        }
    }
    set_cur_player(cur);
    g_playerPlayerData[lk].kill_counts[lv]++;
    if (k->held && coopVsScenarioItem() >= 0)
    {
        coopVsDropAt(lv, coopVsScenarioItem());
        g_playerPlayerData[lv].have_token_or_goldengun = 0;
    }
    portConLogf("ge: versus: player %d killed player %d\n", ((k->killer == 0xFF) ? k->victim : k->killer) + 1, k->victim + 1);
}

/* a blast set off on someone else's game, again here with its owner */
void coopVersusExplosion(PropRecord *source, coord3d *pos, s32 type, u8 *rooms)
{
    VsBoom b;
    s32 i;

    if (!coopVersus() || sVsApplying || source != NULL || type == EXPLOSION_DEF_PLAYER)
    {
        return;
    }
    memset(&b, 0, sizeof(b));
    b.pos[0] = pos->f[0];
    b.pos[1] = pos->f[1];
    b.pos[2] = pos->f[2];
    b.type = (s16) type;
    memset(b.rooms, 0xFF, sizeof(b.rooms));
    for (i = 0; rooms != NULL && i < 7 && rooms[i] != 0xFF; i++)
    {
        b.rooms[i] = rooms[i];
    }
    coopVsSend(COOPEV_VSBOOM, COOP_ALL, &b, sizeof(b));
}

static void coopVsApplyBoom(const VsBoom *b, s32 from)
{
    coord3d pos;
    StandTile *stan;
    u8 rooms[8];
    s32 owner = coopVsLocalOfNet(from);

    pos.f[0] = b->pos[0];
    pos.f[1] = b->pos[1];
    pos.f[2] = b->pos[2];
    stan = stanFindTileBelowPos(&pos, NULL, NULL);
    if (stan == NULL || b->rooms[0] == 0xFF || owner < 0)
    {
        return;
    }
    memcpy(rooms, b->rooms, sizeof(rooms));
    explosionCreate(NULL, &pos, stan, b->type, 0, owner, rooms, 0);
}

/* propobj.c (coopPickedUp): something was picked up here */
void coopVersusPickedUp(s32 item)
{
    VsItem it;

    if (!coopVersus() || item != coopVsScenarioItem())
    {
        return;
    }
    memset(&it, 0, sizeof(it));
    it.what = VSITEM_PICKED;
    it.item = (u8) item;
    coopVsSend(COOPEV_VSITEM, COOP_ALL, &it, sizeof(it));
    portConLogf("ge: versus: we picked up item %d\n", item);
}

static void coopVsApplyItem(const VsItem *it, s32 from)
{
    s32 li = coopVsLocalOfNet(from);

    if (li <= 0 || it->what != VSITEM_PICKED)
    {
        return;
    }
    if (bondinvHasInvItem(it->item))
    {   /* both grabbed it at once: the lower player number keeps it */
        if (from > sVsNet[0])
        {
            return;
        }
        bondinvRemoveItemByID(it->item);
        hudmsgBottomShow("Someone else got there first.\n");
    }
    coopVsRemoveLoose(it->item);
    g_playerPlayerData[li].have_token_or_goldengun = 1;
    portConLogf("ge: versus: player %d picked up item %d\n", from + 1, it->item);
}

void coopVersusApplyEvent(s32 kind, s32 from, const void *raw)
{
    if (!coopVersus())
    {
        return;
    }
    sVsApplying = 1;
    switch (kind)
    {
        case COOPEV_VSHIT:
        {
            const VsHit *h = (const VsHit *) raw;
            s32 lk = coopVsLocalOfNet(from);

            if (lk > 0 && get_cur_playernum() == 0 && !g_playerPointers[0]->bonddead)
            {
                portConLogf("ge: versus: hit by player %d for %.3f\n", from + 1, h->damage);
                record_damage_kills(h->damage, h->vx, h->vz, lk, h->armour);
            }
            break;
        }
        case COOPEV_VSKILL:
            coopVsApplyKill((const VsKill *) raw);
            break;
        case COOPEV_VSEND:
            if (!sVsEnded && !g_gameOverFlag)
            {
                sVsEnded = 1;
                portConLogf("ge: versus: player %d's game ended the match\n", from + 1);
                mpCalculateAwards(FALSE);
            }
            break;
        case COOPEV_VSITEM:
            coopVsApplyItem((const VsItem *) raw, from);
            break;
        case COOPEV_VSBOOM:
            coopVsApplyBoom((const VsBoom *) raw, from);
            break;
    }
    sVsApplying = 0;
}

/* mpmenu.c mpCalculateAwards: the match is over here; over everywhere */
void coopVersusMatchOver(void)
{
    if (!coopVersus() || sVsApplying || sVsEnded)
    {
        return;
    }
    sVsEnded = 1;
    coopVsSend(COOPEV_VSEND, COOP_ALL, NULL, 0);
    portConLogf("ge: versus: the match is over\n");
}

/* ---- the VERSUS packet ----------------------------------------------------------- */

static void coopVsSendPkt(void)
{
    CoopVsPkt p;

    memset(&p, 0, sizeof(p));
    p.type = COOPPKT_VERSUS;
    p.player = (u8) sVsNet[0];
    p.holds = (u8) g_playerPlayerData[0].have_token_or_goldengun;
    p.serial = sVsSerial;
    p.flagticks = g_playerPlayerData[0].flag_counter;
    coopSendVersusPkt(&p, sizeof(p));
}

s32 coopVersusPktSize(void)
{
    return (s32) sizeof(CoopVsPkt);
}

void coopVersusRecvPkt(void *buf, s32 player)
{
    CoopVsPkt *p = (CoopVsPkt *) buf;
    s32 li = coopVsLocalOfNet(player);

    if (!coopVersus() || li <= 0 || p->serial != sVsSerial)
    {
        return;
    }
    g_playerPlayerData[li].have_token_or_goldengun = p->holds;
    g_playerPlayerData[li].flag_counter = p->flagticks;
}

/* ---- test harness (coop/docker/versus.sh; see coop/NOTES.md) ------------------------
 * GE_VS_HUNT=<s>: at <s> seconds into the match step 4 m in front of the first
 * opponent and from then on keep facing them (GE_COOP_TESTFIRE fires).
 * GE_VS_GOTO=<s>: at <s> seconds walk onto the first opponent's spot (pickups).
 * GE_VS_GRAB=<s>: at <s> seconds walk onto the scenario's item (Flag Tag, Golden Gun).
 * GE_VS_RESPAWN=1: press Start when dead, and at the end screen. */
static s32 sVsHuntDone;
static s32 sVsHuntAlive;
static s32 sVsHuntDir;
static s32 sVsGotoAt = -2;
static s32 sVsGrabAt = -2;
static s32 sVsRespawn;

static void coopVsHarness(void)
{
    struct player *me = g_playerPointers[0];
    PropRecord *bond = me->prop;
    PropRecord *them = (sVsCount >= 2 && g_playerPointers[1] != NULL) ? g_playerPointers[1]->prop : NULL;
    const s32 s = (s32) (sVsTick / 60);

    if (sVsHuntAt == -2)
    {
        const char *e = getenv("GE_VS_HUNT");
        const char *g = getenv("GE_VS_GOTO");

        sVsHuntAt = (e != NULL) ? atoi(e) : -1;
        sVsGotoAt = (g != NULL) ? atoi(g) : -1;
        sVsGrabAt = (getenv("GE_VS_GRAB") != NULL) ? atoi(getenv("GE_VS_GRAB")) : -1;
        sVsRespawn = (getenv("GE_VS_RESPAWN") != NULL);
        sVsHuntDone = 0;
    }
    coopLookTestTick((s32) sVsTick);    /* GE_COOP_TESTFIRE: draw a gun, hold fire */
    if (sVsRespawn && me->bonddead && (sVsTick % 30) == 0)
    {   /* GE_VS_RESPAWN: press Start to come back */
        portJoyInjectHeld(PLAYER_1, START_BUTTON);
    }
    if (them == NULL || bond == NULL || me->bonddead || !sVsHeard[1])
    {
        return;
    }
    if (sVsGrabAt >= 0 && s == sVsGrabAt && (sVsTick % 60) == 0 && coopVsScenarioItem() >= 0)
    {
        PropRecord *it = coopVsFindLoose(coopVsScenarioItem());

        StandTile *st = (it != NULL) ? stanFindTileBelowPos(&it->pos, NULL, NULL) : NULL;

        if (st != NULL)
        {
            coopRespawnPlayerAt(&it->pos, st, 0.0f, getCurrentPlayerWeaponId(GUNRIGHT));
            portConLogf("ge: versus: harness: went to item %d\n", coopVsScenarioItem());
        }
    }
    if (sVsGotoAt >= 0 && s == sVsGotoAt && (sVsTick % 60) == 0)
    {
        coopRespawnPlayerAt(&them->pos, them->stan, 0.0f, getCurrentPlayerWeaponId(GUNRIGHT));
        portConLogf("ge: versus: harness: went to player %d's spot\n", sVsNet[1] + 1);
    }
    if (sVsHuntAt < 0 || s < sVsHuntAt)
    {
        return;
    }
    if (!g_playerPointers[1]->bonddead && (sVsTick % 300) == 0)
    {   /* every 5 s: up to them again if they got away, and a full gun */
        const char *f = getenv("GE_COOP_TESTFIRE");
        f32 dx = them->pos.f[0] - bond->pos.f[0];
        f32 dz = them->pos.f[2] - bond->pos.f[2];

        if (sVsHuntDone == 1 && (dx * dx + dz * dz > 600.0f * 600.0f || (sVsHuntHits == 0 && s >= sVsHuntAt + 10)))
        {   /* they got away, or a door stands between: try another side */
            sVsHuntDone = 2;
            sVsHuntDir++;
        }
        sVsHuntHits = 0;
        if (f != NULL && atoi(f) > 0 && sVsHuntDone > 0
            && g_CurrentPlayer->ammoheldarr[get_ammo_type_for_weapon(atoi(f))] < 50)
        {
            coopGiveTestDraw(atoi(f));
        }
    }
    sVsHuntAlive = g_playerPointers[1]->bonddead ? 0 : sVsHuntAlive + 1;
    if (sVsHuntDone < 1 || (sVsHuntAlive > 60 && sVsHuntDone == 2))   /* (their respawn has settled) */
    {   /* in front of them (again after they respawn) */
        coord3d to;
        StandTile *stan = NULL;
        f32 yaw = 6.2831855f - g_playerPointers[1]->vv_theta * 6.2831855f / 360.0f;
        s32 k;

        for (k = 0; k < 8 && stan == NULL; k++)
        {
            f32 a = (f32) ((k + sVsHuntDir) % 8) * 0.785f;

            to.f[0] = them->pos.f[0] + sinf(a) * 400.0f;
            to.f[1] = them->pos.f[1] + 30.0f;
            to.f[2] = them->pos.f[2] + cosf(a) * 400.0f;
            stan = stanFindTileBelowPos(&to, NULL, NULL);
            if (stan != NULL)
            {   /* a straight walk to them, so no wall stands between (from here:
                 * this tile is surely under this spot) */
                StandTile *from = stan;

                if (!walkTilesBetweenPoints_NoCallback(&from, to.f[0], to.f[2], them->pos.f[0], them->pos.f[2]))
                {
                    stan = NULL;
                }
            }
        }
        (void) yaw;
        if (stan != NULL)
        {
            coopRespawnPlayerAt(&to, stan, 0.0f, getCurrentPlayerWeaponId(GUNRIGHT));
            portConLogf("ge: versus: harness: 4 m from player %d at %.0f %.0f %.0f\n", sVsNet[1] + 1, to.f[0], to.f[1], to.f[2]);
        }
        sVsHuntDone = 1;
    }
    if (g_playerPointers[1]->bonddead)
    {
        sVsHuntDone = 2;    /* move up again once they are back */
    }
    {
        f32 dx = them->pos.f[0] - bond->pos.f[0];
        f32 dz = them->pos.f[2] - bond->pos.f[2];
        f32 dy = (them->pos.f[1] - 10.0f) - bond->pos.f[1];
        f32 d = sqrtf(dx * dx + dz * dz);

        me->vv_theta = -(atan2f(dx, dz) * 180.0f) / M_PI_F;
        if (me->vv_theta < 0.0f)
        {
            me->vv_theta += 360.0f;
        }
        me->vv_verta = (atan2f(dy, (d > 1.0f) ? d : 1.0f) * 180.0f) / M_PI_F;
    }
}

/* lv.c, just before the watch menu's tick: GE_VS_RESPAWN also leaves the end screen */
void coopVersusHarnessInput(void)
{
    static s32 n;

    if (coopVersus() && sVsRespawn && g_gameOverFlag && get_cur_playernum() == 0 && (++n % 120) == 0)
    {
        portJoyInjectPressed(PLAYER_1, START_BUTTON);
    }
}

/* ---- every tick (coop.c coopTick) ------------------------------------------------- */

void coopVersusTick(void)
{
    s32 li;

    if (!coopVersus())
    {
        return;
    }
    sVsTick++;
    for (li = 1; li < sVsCount; li++)
    {
        coopVsRemoteTick(li);
    }
    coopVsHarness();
    if ((sVsTick % VS_PKT_EVERY) == 0)
    {
        coopVsSendPkt();
    }
}
