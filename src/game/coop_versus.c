/**
 * Online versus: GoldenEye's own multiplayer (its arenas, scenarios, weapon
 * sets and characters) played over the co-op session, one player per PC,
 * each on a full screen instead of a quarter of one.
 *
 * Every game loads the same N-player match (front.c coopVsStartMatch) with
 * itself as the first player, local index 0, so the keyboard, mouse and first
 * pad stay where they always are. The other players' structs exist as they
 * do in split screen, but they are never moved here (boss.c skips their
 * lvlViewMoveTick) and get no view (lv.c): each one is filled from that
 * player's STATE, and Rare's own split-screen body code (bondview2.c
 * playerTick) poses and turns their body from it. The game's own multiplayer
 * rules run over all of them: scores, scenarios, limits, the radar, respawns
 * and the end screens.
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
 *   - the arena's guns, ammo boxes and armour: whoever picks one up says so
 *     (VSPICK) and it leaves the other worlds, to come back on Rare's timer.
 *   - guns a dead player drops, grenades and mines: coop_items.c, as in
 *     co-op (the dropper's or thrower's game says where they came to rest).
 *   - shots on glass, crates and other setup objects: the shooter's game
 *     sends each hit (OBJDMG) and every game applies it.
 *   - the end: any game whose limits end the match ends it for all (VSEND).
 *   - leaving: a joiner who picks Quit leaves alone (VSLEFT) and the others
 *     play on; so does one whose game stops answering. Their body leaves the
 *     arena and the radar, and once one player (or one team) is left the
 *     match ends. The host's Quit still ends it for everyone, since the
 *     others' packets go through the host.
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
#include "mpmenu.h"
#include "coop.h"
#include "coop_net.h"
#include "math_atan2f.h"
#include "initanitable.h"

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
extern void objFree(ObjectRecord *obj, s32 freeprop, s32 canregen);
extern s32 sizepropdef(PropDefHeaderRecord *pdef);
extern void portJoyInjectHeld(s8 contpadnum, u16 mask);

#define VS_DEAD_SETTLE  120     /* ticks a remote death plays before the game counts it as finished */
#define VS_PKT_EVERY    12      /* ticks between our VERSUS packets (5 a second) */
#define VS_GONE_AFTER   120     /* ticks without their STATE before a player counts as gone */
#ifdef VERSION_EU
#define CHROBJ_REGEN_TICKS 1000 /* chrprop.c propExecuteTickOperation: a taken pickup's wait */
#else
#define CHROBJ_REGEN_TICKS 1200
#endif

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
static s32 sVsGone[MAX_PLAYER_COUNT];       /* left the match: quit, or stopped answering */
static s32 sVsAbsent[MAX_PLAYER_COUNT];     /* ticks their STATE has been missing */
static s32 sVsQuit;                         /* we quit: our end is ours alone */
static char sVsName[MAX_PLAYER_COUNT][24];  /* their name as the match began (a renumbering loses it) */
static s32 sVsInBlast;                  /* explosion.c: damage from a blast is being dealt */
static s32 sVsApplying;                 /* a remote event is being applied */
static u32 sVsTick;
typedef struct
{   /* COOPEV_VSSTATS: one PC's own column of the end screen, what only it counts */
    u16 shots;          /* SHOT_REGISTER_TOTAL */
    u16 head;           /* SHOT_REGISTER_HEAD */
    u32 onscreen;       /* time_other_players_on_screen */
    f32 kmh;            /* its average speed, from its own clock */
    u16 armour;         /* body_armor_pickups x 100 */
    u16 longest;        /* longest_inning / 4, 0xFFFF for S32_MAX */
    u16 shortest;       /* shortest_inning / 4, the same */
    u8 multikill;       /* most_killed_one_time */
    u8 pad;
    u8 backside[MAX_PLAYER_COUNT];  /* hits it took from behind, by the shooter's session number */
    u8 pad2[2];
} VsStats;

static VsStats sVsCol[COOP_MAX_PLAYERS];    /* by session number, ours included */
static s32 sVsColHave[COOP_MAX_PLAYERS];
static s32 sVsBackLocal[MAX_PLAYER_COUNT];  /* hits from behind on our player, by local index, before the columns */
static s32 sVsColSent;
static u32 sVsAwardDice;
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

/* The host's versus rules (the lobby's Radar and Body Armour rows). Every PC
 * plays these, not its own Multiplayer Options sheet (port/src/video.c), so
 * nobody's game has rules the others' don't. */
s32 coopVersusRule(s32 bit)
{
    return (coopSetting(COOPSET_VSRULES) & bit) != 0;
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
        sVsGone[i] = 0;
        sVsAbsent[i] = 0;
    }
    sVsEnded = 0;
    sVsQuit = 0;
    sVsTick = 0;
    sVsColSent = 0;
    memset(sVsColHave, 0, sizeof(sVsColHave));
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

static s32 coopVsScenarioItem(void);
static void coopVsDropAt(s32 li, s32 item);
static void coopVsSend(s32 kind, s32 to, const void *payload, s32 len);
static void coopVsApplyStats(const VsStats *c, s32 from);

/* one player (or one team) is all that is left: the match is over */
static void coopVsLastStanding(void)
{
    s32 li, n = 0, teams = 0;

    if (sVsEnded || g_gameOverFlag)
    {
        return;
    }
    for (li = 0; li < sVsCount; li++)
    {
        if (!sVsGone[li])
        {
            n++;
            teams |= 1 << (portMpTeamScenario() ? portMpTeamOf(li) : li);
        }
    }
    if (n >= 2 && (teams & (teams - 1)) != 0)
    {
        return;     /* two sides still play */
    }
    portConLogf("ge: versus: only %s left, the match is over\n", (n >= 2) ? "one team is" : "we are");
    mpCalculateAwards(FALSE);   /* (coopVersusMatchOver tells any team-mates) */
}

/* local player li left the match: their body leaves the arena and the radar */
static void coopVsGone(s32 li)
{
    struct player *pl = g_playerPointers[li];
    const s32 p = sVsNet[li];
    const s32 item = coopVsScenarioItem();
    char msg[64];

    if (li <= 0 || li >= sVsCount || sVsGone[li])
    {
        return;
    }
    sVsGone[li] = 1;
    if (pl != NULL && pl->prop != NULL && pl->prop->chr != NULL)
    {
        if (item >= 0 && g_playerPlayerData[li].have_token_or_goldengun && !pl->bonddead)
        {
            coopVsDropAt(li, item);     /* the flag or the Golden Gun stays in play */
        }
        pl->prop->chr->chrflags |= CHRFLAG_HIDDEN;  /* not drawn, not in anyone's way */
        pl->bonddead = TRUE;                        /* off the radar; the end checks count them finished */
        pl->redbloodfinished = TRUE;
        pl->deathanimfinished = TRUE;
        pl->colourfadetimemax60 = -1.0f;
    }
    g_playerPlayerData[li].have_token_or_goldengun = 0;
    if (sVsName[li][0] != '\0')
    {
        snprintf(msg, sizeof(msg), "%s|LEFT THE MATCH", sVsName[li]);
    }
    else
    {
        snprintf(msg, sizeof(msg), "A PLAYER|LEFT THE MATCH");
    }
    coopFeedToast(msg, (p < COOP_MAX_PLAYERS) ? p : -1);
    portConLogf("ge: versus: local player %d (player %d) left the match\n", li + 1, p + 1);
    coopVsLastStanding();
}

/* mpmenu.c: our Quit leaves the match to the others, unless we host it */
s32 coopVersusQuitAlone(void)
{
    return coopVersus() && !coopNetIsHost();
}

/* mpmenu.c, Quit on the pause menu: a joiner tells the others it is going,
 * and its own end (coopVersusMatchOver) stays its own */
void coopVersusQuit(void)
{
    if (!coopVersusQuitAlone())
    {
        return;
    }
    sVsQuit = 1;
    coopVsSend(COOPEV_VSLEFT, COOP_ALL, NULL, 0);
    portConLogf("ge: versus: we quit the match\n");
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
        coord3d off;

        /* x and z only, as Rare's own MP bodies and co-op's partners do
         * (coop.c coopPuppetPlace): the model's height is the animation's
         * root over the ground, and a loop of the idle seeds the root from
         * it, so the player's eye height here made the body hop */
        getsuboffset(chr->model, &off);
        off.f[0] = pos.f[0];
        off.f[2] = pos.f[2];
        setsuboffset(chr->model, &off);
        setsubroty(chr->model, info->yaw);
        sub_GAME_7F01FC10(chr->model, &prop->pos, &prop->pos, &chr->ground);
    }
    chrDetectRooms(chr);
    pl->bondprevpos = pos;
    pl->field_488.collision_position = pos;     /* where the multiplayer body code puts the body back */
    pl->field_488.current_tile_ptr = stan;
}

/* Rare's split-screen code (bondview2.c playerTick, run as the props tick
 * after coopTick) animates and turns each other player's body from that
 * player's struct, which nothing here ticks: so the struct gets what their
 * STATE says, and the body moves as it would on a split screen. Left stale,
 * it turned every body back to where it first spawned and put it back in
 * the idle pose each frame, over the running and strafing we set. */
static void coopVsFeedPlayer(struct player *pl, const CoopRemoteInfo *info)
{
    f32 theta = 360.0f - info->yaw * (180.0f / M_PI_F);
    s32 h;

    while (theta >= 360.0f)
    {
        theta -= 360.0f;
    }
    while (theta < 0.0f)
    {
        theta += 360.0f;
    }
    pl->vv_theta = theta;
    pl->vv_costheta = cosf(theta * (M_PI_F / 180.0f));
    pl->vv_sintheta = sinf(theta * (M_PI_F / 180.0f));
    pl->speedforwards = info->look.speedforwards;
    pl->speedsideways = info->look.speedsideways;
    pl->speedtheta = info->look.speedtheta;
    pl->field_2A08 = info->look.aimpitch;      /* the body's aim up and down */
    pl->field_2A0C = info->look.aimyaw;        /* and across */
    pl->headanim = info->look.headanim;
    pl->crouchpos = info->look.crouch;
    pl->autocrouchpos = info->look.crouch;
    for (h = GUNRIGHT; h <= GUNLEFT; h++)
    {
        pl->hands[h].field_87D = !info->dead && (info->look.flash & (1 << h)) != 0;   /* the muzzle flash */
    }
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
    if (sVsGone[li])
    {
        return;
    }
    pl->cameramode = 0;     /* normal first-person play: the body is drawn for us (chr.c), as another player's */
    pl->prop->chr->chrnum = COOP_PUPPET_CHRNUM + p;
    if (!coopRemoteInfo(p, &info))
    {   /* left the session or the match, or a renumbering after the host left
         * gave them no number: gone once it lasts (a lost line still counts as
         * here until coop.c gives up on it) */
        if ((sVsHeard[li] && ++sVsAbsent[li] > VS_GONE_AFTER) || p >= COOP_MAX_PLAYERS)
        {
            coopVsGone(li);
        }
        return;
    }
    sVsAbsent[li] = 0;
    if (!sVsHeard[li])
    {
        sVsHeard[li] = 1;
        snprintf(sVsName[li], sizeof(sVsName[li]), "%s", (coopPlayerName(p) != NULL) ? coopPlayerName(p) : "");
        sVsName[li][strcspn(sVsName[li], "\n")] = '\0';
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
    coopVsFeedPlayer(pl, &info);
    coopLookBodyTick(&sVsLook[li], pl->prop->chr, &info.look, info.dead, COOP_PUPPET_CHRNUM + sVsNet[0]);
}

/* coop.c coopSubFieldTick: a tick between two 60ths (tickhz above 60). The
 * game moves our own Bond on every tick, but the others' bodies were placed
 * only on the 60ths, so above 60 fps they stepped. Their spots between
 * samples come from the clock, so each tick places them again; the rest of
 * their state stays on the 60ths. */
void coopVersusSubFieldTick(void)
{
    s32 li;

    if (!coopVersus())
    {
        return;
    }
    for (li = 1; li < sVsCount; li++)
    {
        struct player *pl = g_playerPointers[li];
        CoopRemoteInfo info;

        if (pl != NULL && pl->prop != NULL && pl->prop->chr != NULL && sVsHeard[li]
            && coopRemoteInfo(sVsNet[li], &info) && !info.dead)
        {
            coopVsMoveBody(li, &info);
            coopVsFeedPlayer(pl, &info);
        }
    }
}

/* ---- events -------------------------------------------------------------------- */

typedef struct { f32 damage; f32 vx; f32 vz; u8 armour; u8 pad[3]; } VsHit;
typedef struct { u8 victim; u8 killer; u8 gg; u8 held; f32 pos[3]; } VsKill;
typedef struct { u8 what; u8 item; u8 pad[2]; } VsItem;
typedef struct { f32 pos[3]; s16 type; u8 rooms[8]; u8 pad[2]; } VsBoom;
typedef struct { s32 objofs; s16 type; s16 item; } VsPick;

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

    if (!coopVersus() || sVsApplying || source != NULL || type == EXPLOSION_DEF_PLAYER || coopItemsExploding())
    {   /* (a thrown grenade or mine: each arena sets off its own copy, coop_items.c) */
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
    if (b->rooms[0] == 0xFF || owner < 0)
    {
        return;
    }
    stan = coopBoomStan(&pos);   /* a blast on a ledge or in mid-air, as in co-op */
    if (stan == NULL)
    {
        portConLogf("ge: versus: player %d's blast at %.0f %.0f %.0f has no floor here, dropped\n", from + 1, pos.f[0], pos.f[1], pos.f[2]);
        return;
    }
    memcpy(rooms, b->rooms, sizeof(rooms));
    explosionCreate(NULL, &pos, stan, b->type, 0, owner, rooms, 0);
    portConLogf("ge: versus: player %d's blast (type %d) at %.0f %.0f %.0f set off here\n", from + 1, b->type, pos.f[0], pos.f[1], pos.f[2]);
}

/* coop.c coopPickedUp: something was picked up here. The scenario's item
 * says so with VSITEM; a gun, ammo box or armour the arena placed leaves the
 * other arenas too (VSPICK) and comes back there on the same timer; a gun a
 * player dropped or something thrown is coop_items.c's. */
void coopVersusPickedUp(ObjectRecord *obj)
{
    VsItem it;
    VsPick pk;
    s32 item = (obj->type == PROPDEF_COLLECTABLE) ? ((WeaponObjRecord *) obj)->weaponnum : -1;

    if (!coopVersus() || sVsApplying)
    {
        return;
    }
    if (item >= 0 && item == coopVsScenarioItem())
    {
        memset(&it, 0, sizeof(it));
        it.what = VSITEM_PICKED;
        it.item = (u8) item;
        coopVsSend(COOPEV_VSITEM, COOP_ALL, &it, sizeof(it));
        portConLogf("ge: versus: we picked up item %d\n", item);
        return;
    }
    memset(&pk, 0, sizeof(pk));
    pk.objofs = coopExtSetupOfs(obj);
    if (pk.objofs < 0)
    {
        coopItemsPickedUp(obj);
        return;
    }
    pk.type = (s16) obj->type;
    pk.item = (s16) item;
    coopVsSend(COOPEV_VSPICK, COOP_ALL, &pk, sizeof(pk));
    portConLogf("ge: versus: we picked up obj %d (type %d, item %d)\n", pk.objofs, pk.type, item);
}

/* the setup object at that offset, if it is the kind they say */
static ObjectRecord *coopVsSetupObj(s32 ofs, s32 type)
{
    ObjectRecord *obj;

    if (g_CurrentSetup.propDefs == NULL || ofs < 0)
    {
        return NULL;
    }
    obj = (ObjectRecord *) ((u8 *) g_CurrentSetup.propDefs + ofs);
    if (coopExtSetupOfs(obj) != ofs || obj->type != type)
    {
        return NULL;
    }
    return obj;
}

/* it lies in this arena now (not taken, not waiting to come back) */
static s32 coopVsPickupHere(ObjectRecord *obj)
{
    PropRecord *prop = obj->prop;

    return prop != NULL && prop->obj == obj && (prop->flags & PROPFLAG_ENABLED) && prop->timetoregen == 0 && prop->parent == NULL;
}

/* someone else took a setup pickup: the same two steps as a pickup here
 * (propPickupByPlayer's objFree, then propsTick's TICKOP_FREE), without
 * the gun, so it waits to come back on Rare's timer as theirs does */
static void coopVsApplyPick(const VsPick *pk, s32 from)
{
    ObjectRecord *obj = coopVsSetupObj(pk->objofs, pk->type);
    PropRecord *prop;

    if (obj == NULL || (prop = obj->prop) == NULL)
    {
        return;
    }
    if (!coopVsPickupHere(obj))
    {
        if (prop->timetoregen > 0 && (((PropDefHeaderRecord *) obj)->state & PROPSTATE_RESPAWN))
        {   /* it came back there before here and they took it again: start our wait again too */
            prop->timetoregen = CHROBJ_REGEN_TICKS;
            portConLogf("ge: versus: player %d picked up obj %d, still coming back here: wait again\n", from + 1, pk->objofs);
        }
        else
        {
            portConLogf("ge: versus: player %d picked up obj %d, already gone here\n", from + 1, pk->objofs);
        }
        return;
    }
    objFree(obj, 0, ((PropDefHeaderRecord *) obj)->state & RUNTIMEBITFLAG_REMOVE);
    propExecuteTickOperation(prop, TICKOP_FREE);
    portConLogf("ge: versus: player %d picked up obj %d: gone here too\n", from + 1, pk->objofs);
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

            if (lk > 0 && get_cur_playernum() == 0 && !g_playerPointers[0]->bonddead && !g_gameOverFlag)
            {
                portConLogf("ge: versus: hit by player %d for %.3f\n", from + 1, h->damage);
                record_damage_kills(h->damage, h->vx, h->vz, lk, h->armour);
            }
            break;
        }
        case COOPEV_VSKILL:
            coopVsApplyKill((const VsKill *) raw);
            if (g_gameOverFlag)
            {   /* it came after the end here: the table it changes is on the end screen */
                extern void mpVersusRechooseAwards(void);
                mpVersusRechooseAwards();
            }
            break;
        case COOPEV_VSLEFT:
            coopVsGone(coopVsLocalOfNet(from));
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
        case COOPEV_VSPICK:
            coopVsApplyPick((const VsPick *) raw, from);
            break;
        case COOPEV_VSSTATS:
        {
            VsStats c;

            memcpy(&c, raw, sizeof(c));
            coopVsApplyStats(&c, from);
            break;
        }
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
    if (sVsQuit)
    {
        portConLogf("ge: versus: we left; the others play on\n");
        return;
    }
    coopVsSend(COOPEV_VSEND, COOP_ALL, NULL, 0);
    portConLogf("ge: versus: the match is over\n");
}

/* ---- the end screen's stats -------------------------------------------------------
 * Each PC counts only some of the stats the awards are picked from: its own
 * player's shots, speed, armour, time alive and multi-kills, and the hits from
 * behind its own player took. Left as they were, the awards and accuracy came
 * out differently on every screen. At the end each PC sends its column, every
 * PC fills the table from them (its own through the same packing, so all agree)
 * and picks again, with dice seeded by the match. */

static u16 coopVsPackInning(s32 v)
{
    return (v == S32_MAX || v / 4 >= 0xFFFF) ? 0xFFFF : (u16) ((v < 0 ? 0 : v) / 4);
}

static s32 coopVsUnpackInning(u16 v)
{
    return (v == 0xFFFF) ? S32_MAX : (s32) v * 4;
}

static void coopVsOwnColumn(VsStats *c, s32 duration)
{
    struct player_data *pd = &g_playerPlayerData[0];
    s32 li, b;

    memset(c, 0, sizeof(*c));
    c->shots = (u16) ((pd->shot_count[SHOT_REGISTER_TOTAL] > 0xFFFF) ? 0xFFFF : pd->shot_count[SHOT_REGISTER_TOTAL]);
    c->head = (u16) ((pd->shot_count[SHOT_REGISTER_HEAD] > 0xFFFF) ? 0xFFFF : pd->shot_count[SHOT_REGISTER_HEAD]);
    c->onscreen = (u32) pd->time_other_players_on_screen;
    c->kmh = pd->distance_traveled / 100000.0f / ((duration + 1) / (3600.0f * 60.0f));
    c->armour = (u16) ((pd->body_armor_pickups * 100.0f > 65535.0f) ? 65535.0f : pd->body_armor_pickups * 100.0f + 0.5f);
    c->longest = coopVsPackInning(pd->longest_inning);
    c->shortest = coopVsPackInning(pd->shortest_inning);
    c->multikill = (u8) pd->most_killed_one_time;
    for (li = 0; li < sVsCount; li++)
    {
        if (sVsNet[li] < MAX_PLAYER_COUNT)
        {
            b = sVsBackLocal[li];
            c->backside[sVsNet[li]] = (u8) ((b > 255) ? 255 : b);
        }
    }
}

/* mpmenu.c mpChooseAwards, before it reads the table: our column goes out
 * once, and every column we have goes into the table */
void coopVersusAwardStats(s32 duration)
{
    s32 li, p;

    if (!coopVersus())
    {
        return;
    }
    if (!sVsColSent)
    {   /* the hits from behind counted here, before the columns are added to them */
        for (li = 0; li < MAX_PLAYER_COUNT; li++)
        {
            sVsBackLocal[li] = (li < sVsCount) ? g_playerPlayerData[li].damage_to_backside : 0;
        }
        coopVsOwnColumn(&sVsCol[sVsNet[0]], duration);
        sVsColHave[sVsNet[0]] = 1;
        sVsColSent = 1;
        coopVsSend(COOPEV_VSSTATS, COOP_ALL, &sVsCol[sVsNet[0]], sizeof(VsStats));
        portConLogf("ge: versus: our end-screen stats sent (%d shots, %d head)\n", sVsCol[sVsNet[0]].shots, sVsCol[sVsNet[0]].head);
    }
    for (li = 0; li < sVsCount; li++)
    {
        struct player_data *pd = &g_playerPlayerData[li];
        const s32 net = sVsNet[li];
        s32 back = sVsBackLocal[li];

        for (p = 0; p < COOP_MAX_PLAYERS; p++)
        {
            if (sVsColHave[p] && p != sVsNet[0] && net < MAX_PLAYER_COUNT)
            {
                back += sVsCol[p].backside[net];
            }
        }
        pd->damage_to_backside = back;
        if (net >= COOP_MAX_PLAYERS || !sVsColHave[net])
        {
            continue;   /* (theirs has not come; ours is always here) */
        }
        pd->shot_count[SHOT_REGISTER_TOTAL] = sVsCol[net].shots;
        pd->shot_count[SHOT_REGISTER_HEAD] = sVsCol[net].head;
        pd->time_other_players_on_screen = (s32) sVsCol[net].onscreen;
        pd->distance_traveled = sVsCol[net].kmh * 100000.0f * ((duration + 1) / (3600.0f * 60.0f));
        pd->body_armor_pickups = sVsCol[net].armour / 100.0f;
        pd->longest_inning = coopVsUnpackInning(sVsCol[net].longest);
        pd->shortest_inning = coopVsUnpackInning(sVsCol[net].shortest);
        pd->most_killed_one_time = sVsCol[net].multikill;
    }
}

/* mpmenu.c mpChooseAwards, the table read: each PC's own row index is 0, so
 * the rows go into session order (seat[k] the player of row k) for the ties
 * and the dice to fall alike; and the speed is the one its PC sent, as is */
void coopVersusAwardMetrics(s32 count, struct AwardMetrics *metrics, s32 *seat)
{
    struct AwardMetrics local[MAX_PLAYER_COUNT];
    s32 k, li, p;

    for (k = 0; k < MAX_PLAYER_COUNT; k++)
    {
        seat[k] = k;
    }
    if (!coopVersus() || count != sVsCount)
    {
        return;
    }
    memcpy(local, metrics, sizeof(local[0]) * count);
    k = 0;
    for (p = 0; p <= COOP_MAX_PLAYERS && k < count; p++)
    {   /* (a player renumbered away after a host change sits at COOP_MAX_PLAYERS, last) */
        for (li = 0; li < count; li++)
        {
            if (sVsNet[li] == p)
            {
                seat[k] = li;
                metrics[k] = local[li];
                if (p < COOP_MAX_PLAYERS && sVsColHave[p])
                {
                    metrics[k].avg_km_per_hour = sVsCol[p].kmh;
                }
                k++;
            }
        }
    }
    if (k != count)
    {   /* (cannot happen: every player has a number) */
        memcpy(metrics, local, sizeof(local[0]) * count);
        for (k = 0; k < MAX_PLAYER_COUNT; k++)
        {
            seat[k] = k;
        }
    }
}

/* mpmenu.c: the awards' dice, seeded by the match each time they are picked */
u32 coopVersusAwardRandom(s32 reset)
{
    if (reset)
    {
        sVsAwardDice = sVsSerial * 2654435761u + 0x5EEDu;
        return 0;
    }
    sVsAwardDice = sVsAwardDice * 1103515245u + 12345u;
    return sVsAwardDice >> 8;
}

static void coopVsApplyStats(const VsStats *c, s32 from)
{
    extern void mpVersusRechooseAwards(void);

    if (from < 0 || from >= COOP_MAX_PLAYERS || from == sVsNet[0] || coopVsLocalOfNet(from) <= 0)
    {
        return;
    }
    memcpy(&sVsCol[from], c, sizeof(*c));
    sVsColHave[from] = 1;
    portConLogf("ge: versus: player %d's end-screen stats came (%d shots, %d head)\n", from + 1, c->shots, c->head);
    mpVersusRechooseAwards();   /* (once the match is over here; else the end picks with it) */
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
 * GE_VS_RESPAWN=1: press Start when dead, and at the end screen.
 * GE_VS_TESTBOOM=<s>[:<dy>]: a blast on the first opponent (see below). */
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
    if (getenv("GE_VS_QUIT") != NULL && sVsTick == (u32) atoi(getenv("GE_VS_QUIT")) * 60 && !sVsEnded)
    {   /* GE_VS_QUIT=<s>: Quit on the pause menu that far in */
        coopVersusQuit();
        mpCalculateAwards(FALSE);
        return;
    }
    if (getenv("GE_VS_TESTBOOM") != NULL && sVsTick == (u32) atoi(getenv("GE_VS_TESTBOOM")) * 60 && them != NULL)
    {   /* GE_VS_TESTBOOM=<s>[:<dy>]: a grenade-sized blast on the first opponent that far
         * in, or <dy> above them (below their floor when negative: no floor under it) */
        const char *e = getenv("GE_VS_TESTBOOM");
        coord3d at = them->pos;

        at.f[1] += (strchr(e, ':') != NULL) ? (f32) atof(strchr(e, ':') + 1) : 0.0f;
        portConLogf("ge: versus: harness: blast on player %d at %.0f %.0f %.0f\n", sVsNet[1] + 1, at.f[0], at.f[1], at.f[2]);
        explosionCreate(NULL, &at, them->stan, 2, 0, 0, them->rooms, 0);
    }
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

/* GE_VS_WANDER=<s>: from <s> seconds walk about, 3 s each of running with a
 * turn, strafing, walking back and standing still (so the others see every
 * body animation). GE_VS_AUDIT=1: every 2 s, for each other player, how
 * their body is drawn here against what their STATE says. */
static s32 sVsWanderAt = -2;
static s32 sVsAudit;

static s8 sVsWanderX, sVsWanderY;
static u16 sVsWanderButtons;

static void coopVsWander(void)
{
    s32 phase;

    if (sVsWanderAt == -2)
    {
        const char *w = getenv("GE_VS_WANDER");

        sVsWanderAt = (w != NULL) ? atoi(w) * 60 : -1;
        sVsAudit = getenv("GE_VS_AUDIT") != NULL;
    }
    sVsWanderX = sVsWanderY = 0;
    sVsWanderButtons = 0;
    if (sVsWanderAt < 0 || (s32) sVsTick < sVsWanderAt || g_playerPointers[0]->bonddead)
    {
        return;
    }
    phase = (((s32) sVsTick - sVsWanderAt) / 180) % 5;
    switch (phase)
    {
        case 0: sVsWanderX = 30; sVsWanderY = 80; sVsWanderButtons = ((sVsTick % 30) < 4) ? B_BUTTON : 0; break;  /* run, pressing Use on doors */
        case 1: sVsWanderButtons = R_CBUTTONS; break;       /* strafe */
        case 2: sVsWanderY = -70; break;                    /* back */
        default: break;                                     /* stand (turning on the spot in 3) */
    }
    if (phase == 0 || phase == 3)
    {   /* turn as a mouse does (the stick's x strafes in the PC layouts) */
        struct player *pl = g_playerPointers[0];

        pl->vv_theta += 1.5f;
        if (pl->vv_theta >= 360.0f)
        {
            pl->vv_theta -= 360.0f;
        }
    }
}

void coopVersusTestInput(s8 *stick_x, s8 *stick_y, u16 *buttons)
{
    if (!coopVersus() || sVsWanderAt < 0 || (s32) sVsTick < sVsWanderAt)
    {
        return;
    }
    *stick_x = sVsWanderX;
    *stick_y = sVsWanderY;
    *buttons |= sVsWanderButtons;
}

/* the arena's pickups: guns, ammo boxes and armour placed by the setup */
static s32 coopVsIsPickup(PropDefHeaderRecord *def)
{
    return def->type == PROPDEF_COLLECTABLE || def->type == PROPDEF_AMMO || def->type == PROPDEF_ARMOUR
        || def->type == PROPDEF_MAGAZINE;
}

/* GE_VS_AUDIT: which of them lie in this arena now, as a hash to set beside
 * the other games' lines, and the guns and thrown things made since */
static void coopVsItemsAudit(void)
{
    PropDefHeaderRecord *def = (PropDefHeaderRecord *) g_CurrentSetup.propDefs;
    PropRecord *prop;
    u32 hash = 0, broke = 0, doors = 0;
    s32 n = 0, here = 0, regen = 0, loose = 0, damaged = 0, open = 0;

    if (def == NULL)
    {
        return;
    }
    for (; def->type != PROPDEF_END; def = (PropDefHeaderRecord *) ((u32 *) def + sizepropdef(def)))
    {
        if (def->type == PROPDEF_DOOR && ((DoorRecord *) def)->openPosition > 0.0f)
        {
            open++;
            doors = doors * 31u + (u32) ((u8 *) def - (u8 *) g_CurrentSetup.propDefs);
        }
        if (!coopVsIsPickup(def))
        {   /* glass, crates, barrels: shot or blown up */
            ObjectRecord *obj = (ObjectRecord *) def;

            if ((def->type == PROPDEF_PROP || def->type == PROPDEF_GLASS || def->type == PROPDEF_TINTED_GLASS
                 || def->type == PROPDEF_MONITOR || def->type == PROPDEF_MULTI_MONITOR || def->type == PROPDEF_RACK
                 || def->type == PROPDEF_ALARM || def->type == PROPDEF_CCTV || def->type == PROPDEF_SWITCH) && obj->prop != NULL
                && obj->prop->obj == obj && (obj->damage > 0.0f || (obj->state & PROPSTATE_DESTROYED)))
            {
                damaged++;
                broke = broke * 31u + (u32) ((u8 *) def - (u8 *) g_CurrentSetup.propDefs) + (u32) (obj->damage * 8.0f);
            }
            continue;
        }
        n++;
        if (coopVsPickupHere((ObjectRecord *) def))
        {
            here++;
            hash = hash * 31u + (u32) ((u8 *) def - (u8 *) g_CurrentSetup.propDefs);
        }
        else if (((ObjectRecord *) def)->prop != NULL && ((ObjectRecord *) def)->prop->timetoregen > 0)
        {
            regen++;
        }
    }
    for (prop = g_ActivePropsHead; prop != NULL; prop = prop->next)
    {
        if (prop->type == PROP_TYPE_WEAPON && prop->weapon != NULL && prop->parent == NULL && coopExtSetupOfs(prop->obj) < 0)
        {
            loose++;
        }
    }
    portConLogf("ge: vsitems: t %u: %d of %d pickups here (hash %08x), %d coming back, %d dropped or thrown; %d objects damaged (hash %08x); %d doors open (hash %08x)\n",
                sVsTick / 60, here, n, hash, regen, loose, damaged, broke, open, doors);
}

/* GE_VS_PICKUPS=<s>: from <s> seconds, every 4 s step onto the nearest
 * pickup lying here that we have not stood on lately */
static s32 sVsPickAt = -2;
static s32 sVsPicked[4];

static void coopVsPickupsHarness(void)
{
    PropDefHeaderRecord *def = (PropDefHeaderRecord *) g_CurrentSetup.propDefs;
    PropRecord *bond = g_playerPointers[0]->prop;
    ObjectRecord *best = NULL;
    f32 bestd = 0.0f;
    StandTile *stan;
    coord3d to;
    s32 k;

    if (sVsPickAt == -2)
    {
        const char *e = getenv("GE_VS_PICKUPS");

        sVsPickAt = (e != NULL) ? atoi(e) * 60 : -1;
        memset(sVsPicked, -1, sizeof(sVsPicked));
    }
    if (sVsPickAt < 0 || (s32) sVsTick < sVsPickAt || ((s32) sVsTick - sVsPickAt) % 240 != 0 || def == NULL
        || bond == NULL || g_playerPointers[0]->bonddead)
    {
        return;
    }
    for (; def->type != PROPDEF_END; def = (PropDefHeaderRecord *) ((u32 *) def + sizepropdef(def)))
    {
        ObjectRecord *obj = (ObjectRecord *) def;
        s32 ofs = (s32) ((u8 *) def - (u8 *) g_CurrentSetup.propDefs);
        f32 dx, dz, d;

        if (!coopVsIsPickup(def) || !coopVsPickupHere(obj)
            || (def->type == PROPDEF_COLLECTABLE && ((WeaponObjRecord *) obj)->weaponnum == coopVsScenarioItem()))
        {
            continue;
        }
        for (k = 0; k < 4 && sVsPicked[k] != ofs; k++)
        {
        }
        if (k < 4)
        {
            continue;
        }
        dx = obj->prop->pos.f[0] - bond->pos.f[0];
        dz = obj->prop->pos.f[2] - bond->pos.f[2];
        d = dx * dx + dz * dz;
        if (best == NULL || d < bestd)
        {
            best = obj;
            bestd = d;
        }
    }
    if (best == NULL)
    {
        return;
    }
    to = best->prop->pos;
    to.f[1] += 30.0f;
    stan = stanFindTileBelowPos(&to, NULL, NULL);
    if (stan == NULL)
    {
        return;
    }
    memmove(&sVsPicked[1], &sVsPicked[0], sizeof(sVsPicked[0]) * 3);
    sVsPicked[0] = (s32) ((u8 *) best - (u8 *) g_CurrentSetup.propDefs);
    coopRespawnPlayerAt(&to, stan, 0.0f, getCurrentPlayerWeaponId(GUNRIGHT));
    portConLogf("ge: versus: harness: onto obj %d (type %d)\n", sVsPicked[0], best->type);
}

static s32 coopVsFiring(ChrRecord *chr, s32 hand)
{
    PropRecord *gun = chrGetEquippedWeaponProp(chr, hand);

    return gun != NULL && weaponIsGunfireVisible(gun);
}

static void coopVsAudit(void)
{
    s32 li;

    if (!sVsAudit || (sVsTick % 120) != 0)
    {
        return;
    }
    if ((sVsTick % 300) == 0)
    {
        coopVsItemsAudit();
    }
    for (li = 1; li < sVsCount; li++)
    {
        struct player *pl = g_playerPointers[li];
        CoopRemoteInfo info;
        Model *model;
        f32 drawn, diff;

        if (pl == NULL || pl->prop == NULL || pl->prop->chr == NULL || (model = pl->prop->chr->model) == NULL
            || !coopRemoteInfo(sVsNet[li], &info))
        {
            continue;
        }
        drawn = getsubroty(model);
        diff = (drawn - info.yaw) * (180.0f / M_PI_F) - pl->field_1280;    /* (Rare's lean into a sidestep) */
        while (diff > 180.0f)
        {
            diff -= 360.0f;
        }
        while (diff < -180.0f)
        {
            diff += 360.0f;
        }
        portConLogf("ge: vsaudit: player %d facing %.0f, off by %.0f deg, anim %d rate %.2f (moving %.2f fwd %.2f side, crouch %d), "
                    "aim %.1f/%.1f drawn %.1f/%.1f, flash %d drawn %d%d, dead %d\n",
                    sVsNet[li] + 1, info.yaw * (180.0f / M_PI_F), diff, (s32) model->anim - (s32) ptr_animation_table, modelGetAnimSpeed(model),
                    info.look.speedforwards, info.look.speedsideways, info.look.crouch,
                    info.look.aimpitch, info.look.aimyaw, pl->prop->chr->aimendback, pl->prop->chr->aimendsideback,
                    info.look.flash, coopVsFiring(pl->prop->chr, GUNRIGHT), coopVsFiring(pl->prop->chr, GUNLEFT),
                    info.dead);
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
    coopVsAudit();
    coopVsWander();
    coopVsPickupsHarness();
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
