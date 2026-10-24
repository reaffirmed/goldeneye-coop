/**
 * Story co-op: the other player's Bond as a puppet character.
 *
 * Each field tick this sends the local Bond's position and facing to the peer,
 * and moves a puppet chr to the peer's last reported state. The puppet runs the
 * dead AI list and an action type the tick dispatcher ignores, so only this file
 * moves it.
 *
 * Guard targeting (host): each guard keeps a target, the local Bond or the
 * puppet. While a guard targeting the puppet runs its action and fire ticks,
 * getCurrentPlayerProp() returns the puppet, so the retail "Bond" AI sees,
 * chases and shoots the remote player; damage it deals goes to the peer as a
 * hit packet. The acquire/commit/release rules are ported from GoldenEye 007
 * Plus (src/game/chr.c chrCoopGetTargetPlayer).
 *
 * Up to four players, in a star: the host (player 0) talks to each joiner
 * (players 1-3, by the host's transport slot), a joiner only to the host. The
 * host relays each joiner's STATE to the others, and forwards events that
 * name another player or everyone (CoopEvent.to). Every game has a puppet for
 * each of the others.
 */
#include <ultra64.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <bondaicommands.h>
#include <bondgame.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <boss.h>
#include "bondview.h"
#include "chr.h"
#include "chrai.h"
#include "chraction.h"
#include "initanitable.h"
#include "model.h"
#include "stan.h"
#include "player.h"
#include "lv.h"
#include "coop.h"
#include "coop_net.h"
#include "propobj.h"
#include "objecthandler.h"
#include "objective_status.h"
#include "bondinv.h"
#include "front.h"
#include "file.h"

void doorSetOpenState(DoorRecord *door, s32 newstate);
bool doorIsClosed(DoorRecord *door);
s32 chrGetNumFree(void);
s32 sizepropdef(PropDefHeaderRecord *pdef);
bool objIsMortal(ObjectRecord *obj);
void alarmActivate(void);
void ai(PropDefHeaderRecord *Entityp, PROP_TYPE EntityType);
void *explosionCreate(PropRecord *arg0, coord3d *target_pos, StandTile *target_stan, s16 explosion_type, s32 arg4, s32 player, u8 *rooms, s32 arg7);
ITEM_IDS getCurrentPlayerWeaponId(GUNHAND hand);
void hudmsgBottomShow(char *mess);
TICKOP propPickupByPlayer(PropRecord *prop, bool showstring);
void objFreePermanently(ObjectRecord *obj, bool freeprop);
void alarmDeactivate(void);
bool alarmIsActive(void);
s32 objGetDestroyedLevel(ObjectRecord *obj);
void objApplyDamage(ObjectRecord *obj, f32 damage, coord3d *pos, ITEM_IDS itemnum, s32 playernum);
void chrSetFiring(ChrRecord *self, s32 hand, s32 firing);
void sub_GAME_7F02BFE4(ChrRecord *self, s32 arg1, s32 arg2);
PropRecord *chrSpawnAtCoord(s32 bodynum, s32 headnum, coord3d *pos, StandTile *stan, f32 angle, AIListRecord *ailist, s32 spawnflags);

extern void portConLogf(const char *fmt, ...);

#define COOP_MAGIC   0x50434547u /* "GECP" */
#define COOP_VERSION 4 /* 2: lobby fields in HELLO, health in STATE; 3: session ids; 4: up to four players */

enum { COOPPKT_STATE = 1, COOPPKT_GUARDS = 3, COOPPKT_EVENTS = 5, COOPPKT_WORLD = 6, COOPPKT_HELLO = 7 };

typedef struct
{
    u32 magic;
    u16 version;
    u16 type;
    u32 tick;
    s32 stage;
    f32 pos[3];
    f32 yaw;        /* radians, model convention */
    s32 bodynum;
    s32 headnum;
    s32 dead;
    f32 health;     /* 0..1 */
    f32 armour;     /* 0..1 */
    s32 respawn;    /* dead and waiting: ticks to the respawn (bleed-out under revive only), else -1 */
    s32 downrule;   /* the rule the sender plays by */
    u8 player;      /* whose Bond (the host stamps a joiner's before relaying it) */
    u8 pad;
    u16 calm;       /* ticks since the sender was last in a fight (capped) */
} CoopStatePkt;

#define COOP_ALL 0xFF   /* CoopEvent.to: everyone */

/* the host's roster in each hello: who is in the session */
typedef struct
{
    unsigned long long id;  /* SteamID, or the address; 0 when not known */
    char name[24];
    u8 present;
    u8 ready;
    u8 inlobby;
    u8 pad;
} CoopRosterEnt;

/* Where each game is, both ways once a second, menus included: the joiner
 * follows the host into each mission. */
typedef struct
{
    u32 magic;
    u16 version;
    u16 type;
    s32 stage;          /* LEVELID_TITLE in the menus */
    s32 difficulty;
    u32 mission;        /* host: counts missions started */
    u8 ready;           /* the lobby screen's Ready */
    u8 inlobby;         /* on the lobby screen */
    u8 downrule;        /* host: the lobby's down rule */
    u8 pad;
    u32 session;        /* as in CoopEventsPkt */
    u8 settings[COOPSET_MAX];   /* host: the lobby's other settings (coopSetting) */
    u32 missionticks;   /* how long the sender has been in this mission */
    u8 youare;          /* host: the player number of the game it is sent to */
    u8 pad2[3];
    CoopRosterEnt roster[COOP_MAX_PLAYERS];     /* host: every player, by number */
} CoopHelloPkt;

/* Mission state, both ways twice a second. Everything in it is a level
 * (not an edge), so a lost packet costs nothing. */
typedef struct
{
    u32 magic;
    u16 version;
    u16 type;
    u32 stageflags;     /* host's objectiveregisters1 (set by level scripts) */
    u32 roombits;       /* objective criteria met, one bit per list entry */
    u32 depositbits;
    u32 photobits;
    u8 keycopied;
    u8 alarm;
    u16 calm;           /* ticks since the sender was last in a fight (capped) */
} CoopWorldPkt;

/* Reliable, in-order events (hits, shots, doors, ...). Every EVENTS packet
 * carries all unacked events plus an ack of what we have received. */
enum { COOPEV_HIT = 1, COOPEV_SHOTCHR = 2, COOPEV_DOOR = 3, COOPEV_OBJDMG = 4, COOPEV_PICKUP = 5, COOPEV_MISSIONEND = 6, COOPEV_EXPLODE = 7, COOPEV_AICMD = 8,
       COOPEV_STATS = 9, COOPEV_GIVE = 10, COOPEV_GIVEBACK = 11 };

typedef struct
{
    u8 kind;
    u8 from;        /* the player that queued it */
    u8 to;          /* a player, or COOP_ALL */
    u8 pad;
    union
    {
        struct { f32 damage; f32 angle; } hit;                                  /* host -> client */
        struct { s16 chrnum; s16 hitpart; s32 weaponid; f32 vector[3]; } shot;  /* client -> host */
        struct { s32 doorofs; s32 state; } door;                                /* both ways */
        struct { s32 objofs; f32 damage; f32 pos[3]; s16 item; s16 player; } obj; /* both ways */
        struct { s32 objofs; s32 kept; } pick;                                  /* both ways */
        struct { s32 stage; u32 stageflags; } end;                              /* both ways */
        struct { f32 pos[3]; s16 type; u8 rooms[8]; } boom;                     /* client -> host */
        struct { s16 chrnum; u8 len; u8 cmd[25]; } ai;                          /* host -> client */
        /* COOPEV_STATS: a CoopStatsMsg (coop_stats.c) in raw, both ways */
        CoopGive give;                                                          /* both ways (coop_give.c) */
        u8 raw[28];
    } u;
} CoopEvent;

#define COOP_EVENTS_PER_PKT 32
#define COOP_PUPPET_CHRNUM  30000
#define COOP_EVENT_RING     256

typedef struct
{
    u32 magic;
    u16 version;
    u16 type;
    u32 ack;        /* highest in-order seq received from the peer */
    u32 firstseq;   /* seq of ev[0] */
    u32 count;
    u32 session;    /* the sender's session: a new one means it restarted */
    CoopEvent ev[COOP_EVENTS_PER_PKT];
} CoopEventsPkt;

/* host -> client guard snapshot, one entry per chr */
enum { GSF_HIDDEN = 1, GSF_FLIP = 2, GSF_LOOP = 4, GSF_FIRE_R = 8, GSF_FIRE_L = 16 };

typedef struct
{
    s16 chrnum;
    u8 flags;
    u8 body;
    u8 head;
    s8 gunitem;     /* right hand ITEM_IDS, 0 none */
    s16 gunmodel;   /* PROP_ model of that gun */
    f32 pos[3];
    f32 yaw;
    s32 animoff;    /* byte offset into ptr_animation_table, -1 none */
    f32 frame;
    f32 speed;
} CoopGuardEnt;

#define COOP_GUARDS_PER_PKT 32

typedef struct
{
    u32 magic;
    u16 version;
    u16 type;
    u32 snap;       /* snapshot id; one snapshot spans several packets */
    u16 first;      /* index of the first entry in the snapshot */
    u16 count;
    CoopGuardEnt ent[COOP_GUARDS_PER_PKT];
} CoopGuardsPkt;


static s32 sEnabled = -1;
#define COOP_MAX_PEERHELD 32
static s32 sPeerHeld[COOP_MAX_PEERHELD];
static s32 sNumPeerHeld;
static CoopHelloPkt sHostHello;  /* joiner: the host's last hello */
static u32 sMissionSerial;      /* host: missions started */
static u32 sFollowedMission;    /* joiner: last host mission we launched into */
static s32 sLobbyReady;         /* our Ready on the lobby screen */
static s32 sDownRule = COOPDOWN_BOTH; /* the host's down rule (lobby): COOPDOWN_BOTH, _TIMER or _REVIVE */
static u8 sSettings[COOPSET_MAX];     /* the host's other lobby settings; the joiner copies them from HELLO */
static u32 sSelfSession;        /* ours, new each run and each lobby session */
static s32 sSelfPlayer;         /* our player number: 0 the host, a joiner's from the host's hello */
static u32 sMissionTicks;       /* ticks in this mission */
static s32 sLateJoin;           /* joiner: followed the host into a mission under way */
static s32 sOnLobbyScreen;      /* front.c: the lobby screen is up */
static u32 sLastHurtTick;   /* local Bond lost health */
static u32 sLastFightTick;  /* host: a guard was attacking someone */
static f32 sLastHealth;
static s32 sLastWeapon = ITEM_UNARMED;
static u32 sDeadSince;      /* sTick our Bond's death fade finished, 0 alive */
static s32 sDefsBytes;  /* size of the stage's setup prop defs */
static void *sDefsBase;
static u32 sTick;

/* the other players, by player number (ours is unused) */
typedef struct
{
    CoopStatePkt st;    /* the newest STATE */
    s32 valid;
    u32 seenTick;
    u32 calmTick;       /* sTick when st.calm came */
    PropRecord *puppet;
    coord3d puppetPrev;
    s32 puppetAnim;
    s32 puppetGhost;
    s32 resync;         /* host: it restarted; resend doors when it is back in */
} CoopRemote;
static CoopRemote sR[COOP_MAX_PLAYERS];

/* a reliable event channel per transport peer (host: a joiner each; joiner: the host) */
typedef struct
{
    CoopEvent out[COOP_EVENT_RING];
    u32 outNext;        /* seq the next queued event gets */
    u32 outAcked;       /* the peer has everything up to here */
    u32 inDone;         /* we have applied everything up to here */
    u32 outSentUpTo;
    u32 evLastSend;
    s32 ackDirty;
    u32 session;        /* the peer's session, 0 until heard */
    CoopHelloPkt hello; /* its last hello */
    u32 helloTick;
    s32 helloSeen;
} CoopChan;
static CoopChan sChan[COOP_NET_MAX_PEERS];

/* host guard targeting: a target is a player number, the host's own Bond 0 */
enum { COOPTGT_LOCAL = 0, COOPTGT_COUNT = COOP_MAX_PLAYERS };

typedef struct
{
    s32 lastupdate60;
    s32 laststimulus60;
    s8 target;
} CoopChrTarget;

#define COOP_MAX_CHRS 256
static CoopChrTarget sChrTarget[COOP_MAX_CHRS];
static ChrRecord *sTickChr;         /* guard whose tick has the puppet as Bond */
PropRecord *g_CoopPropOverride;     /* read by getCurrentPlayerProp() */

static s32 sEvForwarding;   /* host: passing a joiner's event on, keep its sender */

/* client guard mirroring */
static u32 sGuardSnap;
static u32 sChrLastSync[COOP_MAX_CHRS]; /* sTick of the last snapshot entry */
static u8 sChrFireSeen[COOP_MAX_CHRS][2]; /* host: firecount at the last snapshot */
static u8 sChrFiring[COOP_MAX_CHRS];      /* client: GSF_FIRE_* from the last snapshot */
static u32 sLastSnapTick;

enum { PANIM_IDLE, PANIM_WALK, PANIM_RUN };

static s32 coopInMission(void);
static void coopApplyGuard(CoopGuardEnt *e);
static void coopApplyEvent(CoopEvent *ev);
static void coopQueueEvent(CoopEvent *ev);
static void coopFlushEvents(void);
static void coopApplyDoor(s32 doorofs, s32 state);
static void coopApplyObjDamage(CoopEvent *ev);
static void coopSendWorld(void);
static void coopApplyPickup(CoopEvent *ev);
static void coopApplyMissionEnd(CoopEvent *ev);
static void coopSendHello(void);
static void coopApplyExplosion(CoopEvent *ev);
static void coopApplyAiCommand(CoopEvent *ev);
static s32 coopLocalCalm(void);
static void coopTrackCombat(void);
static void coopApplyWorld(CoopWorldPkt *p);
static s32 coopSetupOfs(void *rec);
static StandTile *coopFindStan(coord3d *pos, StandTile *from, coord3d *frompos);
static s32 coopIsMirroringClient(void);
static void coopSendGuards(void);
static void coopReapGuards(void);
static void coopClientGunfire(void);
static void coopHarnessTick(void);
static void coopPuppetRemove(CoopRemote *r);

static s32 coopLocalBody(void)
{
    switch (g_CurrentPlayer->bondtype)
    {
        case CUFF_BOILER: return BODY_Special_Operations_Uniform;
        case CUFF_JUNGLE: return BODY_Jungle_Fatigues;
        case CUFF_SNOW:   return BODY_Parka;
        case CUFF_BLUE:   return BODY_Formal_Wear;
        default:          return BODY_Brosnan_Tuxedo;
    }
}

static s32 coopInMission(void)
{
    PropRecord *bond = getCurrentPlayerProp();

    return bond != NULL && bond->stan != NULL && g_CameraMode == CAMERAMODE_FP;
}

static s32 coopRespawnEta(void);

/* ---- players ------------------------------------------------------------- */

/* our player number: the host is 0, a joiner learns its own from the host */
static s32 coopSelf(void)
{
    if (coopNetIsHost())
    {
        return 0;
    }
    return (sHostHello.youare >= 1 && sHostHello.youare < COOP_MAX_PLAYERS) ? sHostHello.youare : 1;
}

static s32 coopPlayerOfChan(s32 c)
{
    return coopNetIsHost() ? c + 1 : 0;
}

/* the channel that reaches player p, -1 if none does directly */
static s32 coopChanOfPlayer(s32 p)
{
    if (coopNetIsHost())
    {
        return (p >= 1 && p <= COOP_NET_MAX_PEERS) ? p - 1 : -1;
    }
    return (p == 0) ? 0 : -1;
}

/* player p (another one) is in this mission with us */
static s32 coopRemoteHere(s32 p)
{
    return p >= 0 && p < COOP_MAX_PLAYERS && p != coopSelf() && sR[p].valid && sR[p].st.stage == bossGetStageNum();
}

static s32 coopRemoteAliveHere(s32 p)
{
    return coopRemoteHere(p) && !sR[p].st.dead && sR[p].puppet != NULL && sR[p].puppet->chr != NULL
        && sR[p].puppet->stan != NULL;
}

/* somebody else is in this mission */
static s32 coopPartnerHere(void)
{
    s32 p;

    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        if (coopRemoteHere(p))
        {
            return 1;
        }
    }
    return 0;
}

/* the player whose puppet this is, -1 if it is none */
static s32 coopPlayerOfProp(PropRecord *prop)
{
    s32 p;

    for (p = 0; prop != NULL && p < COOP_MAX_PLAYERS; p++)
    {
        if (sR[p].puppet == prop)
        {
            return p;
        }
    }
    return -1;
}

static s32 coopIsPuppet(PropRecord *prop)
{
    return coopPlayerOfProp(prop) >= 0;
}

static s32 coopRespawnEta(void);
static s32 coopLocalCalm(void);
static void coopRemotesTick(void);
static void coopQueueEventTo(CoopEvent *ev, s32 to);

static void coopSendState(void)
{
    CoopStatePkt p;
    PropRecord *bond = getCurrentPlayerProp();

    memset(&p, 0, sizeof(p));
    p.magic = COOP_MAGIC;
    p.version = COOP_VERSION;
    p.type = COOPPKT_STATE;
    p.tick = sTick;
    p.stage = bossGetStageNum();
    p.pos[0] = bond->pos.f[0];
    p.pos[1] = bond->pos.f[1];
    p.pos[2] = bond->pos.f[2];
    p.yaw = bondviewGetPlayerYawRadians();
    p.bodynum = coopLocalBody();
    p.headnum = HEAD_Male_Brosnan_Default;
    p.dead = g_CurrentPlayer->bonddead;
    p.health = g_CurrentPlayer->bondhealth;
    p.armour = g_CurrentPlayer->bondarmour;
    p.respawn = coopRespawnEta();
    p.downrule = sDownRule;
    p.player = (u8) coopSelf();
    p.calm = (u16) coopLocalCalm();
    coopNetSend(&p, sizeof(p));

    if ((sTick % 300) == 1)
    {
        portConLogf("ge: coop: local pos %.0f %.0f %.0f yaw %.2f\n", p.pos[0], p.pos[1], p.pos[2], p.yaw);
    }
}

static u32 coopSelfSession(void)
{
    while (sSelfSession == 0)
    {   /* any value that differs from the last run's */
        sSelfSession = (u32) osGetCount() ^ ((u32) osGetTime() << 7) ^ (u32) (size_t) &sSelfSession;
    }
    return sSelfSession;
}

/* Doors a joiner's fresh copy of the level has shut: send it every open one */
static void coopResyncDoors(s32 player)
{
    PropDefHeaderRecord *it = (PropDefHeaderRecord *) g_CurrentSetup.propDefs;
    s32 n = 0;

    if (it == NULL || !coopNetIsHost())
    {
        return;
    }
    for (; it->type != PROPDEF_END; it = (PropDefHeaderRecord *) ((u32 *) it + sizepropdef(it)))
    {
        if (it->type == PROPDEF_DOOR && ((DoorRecord *) it)->prop != NULL && !doorIsClosed((DoorRecord *) it))
        {
            CoopEvent ev;

            memset(&ev, 0, sizeof(ev));
            ev.kind = COOPEV_DOOR;
            ev.u.door.doorofs = coopSetupOfs(it);
            ev.u.door.state = DOORSTATE_OPENING;
            if (ev.u.door.doorofs >= 0)
            {
                coopQueueEventTo(&ev, player);
                n++;
            }
        }
    }
    portConLogf("ge: coop: resent %d open doors to player %d\n", n, player + 1);
}

static void coopChanReset(CoopChan *ch)
{
    ch->outNext = 1;
    ch->outAcked = 0;
    ch->outSentUpTo = 0;
    ch->inDone = 0;
    ch->ackDirty = 0;
    ch->evLastSend = 0;
}

/* A player that restarted (a crash, or rejoining from the lobby) starts its
 * event numbering again: start ours to it again too, so neither side waits
 * for or acks numbers the other never sent. */
static void coopChanSession(s32 c, u32 session)
{
    CoopChan *ch = &sChan[c];

    if (session == 0 || session == ch->session)
    {
        return;
    }
    if (ch->session != 0)
    {
        portConLogf("ge: coop: player %d rejoined (new session), restarting its event channel\n", coopPlayerOfChan(c) + 1);
        coopChanReset(ch);
        sNumPeerHeld = 0;   /* its pickups start again too */
        if (coopNetIsHost())
        {
            sR[coopPlayerOfChan(c)].resync = 1;   /* once it is back in this mission */
        }
    }
    ch->session = session;
}

static void coopApplyEvent(CoopEvent *ev);

/* an EVENTS packet on channel c */
static void coopRecvEvents(s32 c, CoopEventsPkt *e)
{
    CoopChan *ch = &sChan[c];
    u32 k;

    coopChanSession(c, e->session);
    if ((s32) (e->ack - ch->outAcked) > 0)
    {
        ch->outAcked = e->ack;
    }
    for (k = 0; k < e->count; k++)
    {
        u32 seq = e->firstseq + k;
        CoopEvent ev;

        if (seq != ch->inDone + 1)
        {
            continue;
        }
        ch->inDone = seq;
        ev = e->ev[k];
        if (coopNetIsHost())
        {
            ev.from = (u8) coopPlayerOfChan(c);   /* a joiner can only speak for itself */
            if (ev.to != 0)
            {   /* for another joiner, or everyone: pass it on */
                sEvForwarding = 1;
                coopQueueEventTo(&ev, ev.to);
                sEvForwarding = 0;
            }
            if (ev.to != 0 && ev.to != COOP_ALL)
            {
                continue;
            }
        }
        else if (ev.to != COOP_ALL && ev.to != coopSelf())
        {
            continue;
        }
        coopApplyEvent(&ev);
    }
    ch->ackDirty = 1;
}

/* a STATE packet on channel c */
static void coopRecvState(s32 c, CoopStatePkt *p)
{
    CoopRemote *r;
    s32 q;

    if (coopNetIsHost())
    {
        p->player = (u8) coopPlayerOfChan(c);
        for (q = 0; q < COOP_NET_MAX_PEERS; q++)
        {   /* every joiner sees every other one */
            if (q != c)
            {
                coopNetSendTo(q, p, sizeof(*p));
            }
        }
    }
    if (p->player >= COOP_MAX_PLAYERS || p->player == coopSelf())
    {
        return;
    }
    r = &sR[p->player];
    /* datagrams can arrive out of order; keep the newest */
    if (r->valid && (s32) (p->tick - r->st.tick) <= 0 && r->st.stage == p->stage)
    {
        return;
    }
    if (!r->valid)
    {
        portConLogf("ge: coop: first state from player %d (stage %d)\n", p->player + 1, p->stage);
    }
    r->st = *p;
    r->valid = 1;
    r->seenTick = sTick;
    r->calmTick = sTick;
}

static void coopPollNet(void)
{
    u8 buf[1500];
    s32 n;
    s32 c;

    while ((n = coopNetRecvFrom(buf, sizeof(buf), &c)) > 0)
    {
        CoopStatePkt *p = (CoopStatePkt *) buf;

        if (n < (s32) sizeof(u32) * 2 || p->magic != COOP_MAGIC || p->version != COOP_VERSION
            || c < 0 || c >= COOP_NET_MAX_PEERS)
        {
            continue;
        }
        if (p->type == COOPPKT_EVENTS)
        {
            CoopEventsPkt *e = (CoopEventsPkt *) buf;
            s32 hdr = (s32) ((u8 *) &e->ev[0] - (u8 *) e);

            if (n < hdr || e->count > COOP_EVENTS_PER_PKT || n < hdr + (s32) (e->count * sizeof(CoopEvent)))
            {
                continue;
            }
            coopRecvEvents(c, e);
            continue;
        }
        if (p->type == COOPPKT_GUARDS && !coopNetIsHost())
        {
            CoopGuardsPkt *g = (CoopGuardsPkt *) buf;
            s32 hdr = (s32) ((u8 *) &g->ent[0] - (u8 *) g);
            s32 k;

            if (n < hdr || g->count > COOP_GUARDS_PER_PKT || n < hdr + g->count * (s32) sizeof(CoopGuardEnt))
            {
                continue;
            }
            if (coopInMission() && coopRemoteHere(0))
            {
                for (k = 0; k < g->count; k++)
                {
                    coopApplyGuard(&g->ent[k]);
                }
                sLastSnapTick = sTick;
            }
            continue;
        }
        if (p->type == COOPPKT_HELLO && n >= (s32) sizeof(CoopHelloPkt))
        {
            CoopChan *ch = &sChan[c];

            coopChanSession(c, ((CoopHelloPkt *) buf)->session);
            ch->hello = *(CoopHelloPkt *) buf;
            if (!coopNetIsHost())
            {
                sHostHello = ch->hello;
                if (sHostHello.downrule <= COOPDOWN_REVIVE)
                {
                    sDownRule = sHostHello.downrule;   /* the host's lobby decides */
                    memcpy(sSettings, sHostHello.settings, sizeof(sSettings));
                }
            }
            ch->helloTick = sTick;
            if (!ch->helloSeen)
            {
                ch->helloSeen = 1;
                portConLogf("ge: coop: connected to player %d\n", coopPlayerOfChan(c) + 1);
            }
            continue;
        }
        if (p->type == COOPPKT_WORLD && n >= (s32) sizeof(CoopWorldPkt))
        {
            if (coopInMission() && coopRemoteHere(coopPlayerOfChan(c)))
            {
                coopApplyWorld((CoopWorldPkt *) buf);
            }
            continue;
        }
        if (p->type == COOPPKT_STATE && n >= (s32) sizeof(CoopStatePkt))
        {
            coopRecvState(c, p);
        }
    }
}

static StandTile *coopFindStan(coord3d *pos, StandTile *from, coord3d *frompos)
{
    StandTile *tile = from;

    if (tile != NULL && walkTilesBetweenPoints_NoCallback(&tile, frompos->f[0], frompos->f[2], pos->f[0], pos->f[2]) && tile != NULL)
    {
        return tile;
    }
    return stanFindTileBelowPos(pos, NULL, NULL);
}

static void coopPuppetSetAnim(CoopRemote *r, ChrRecord *chr, s32 which, f32 speed)
{
    struct ModelAnimation *anim;
    f32 rate;

    switch (which)
    {
        case PANIM_RUN:
            anim = (struct ModelAnimation *) &ptr_animation_table->data[PTR_ANIM_running];
            rate = speed / D_80030988;
            break;
        case PANIM_WALK:
            anim = (struct ModelAnimation *) &ptr_animation_table->data[PTR_ANIM_walking_unarmed];
            rate = speed / D_80030990;
            break;
        default:
            anim = (struct ModelAnimation *) &ptr_animation_table->data[PTR_ANIM_idle_unarmed];
            rate = 0.25f;
            break;
    }
    if (rate > 1.0f)
    {
        rate = 1.0f;
    }

    if (which != r->puppetAnim)
    {
        modelSetAnimation(chr->model, anim, 0, 0.0f, rate, 16.0f);
        if (which == PANIM_IDLE)
        {
            modelSetAnimLooping(chr->model, 0, 16.0f);
        }
        r->puppetAnim = which;
    }
    else
    {
        modelSetAnimSpeed(chr->model, rate, 0.0f);
    }
}

static void coopPuppetSpawn(CoopRemote *r)
{
    coord3d pos;
    StandTile *stan;
    Model *header;
    PropRecord *prop;
    s32 head = r->st.headnum;

    pos.f[0] = r->st.pos[0];
    pos.f[1] = r->st.pos[1];
    pos.f[2] = r->st.pos[2];
    stan = stanFindTileBelowPos(&pos, NULL, NULL);
    if (stan == NULL)
    {
        return; /* peer is somewhere this stage has no floor; retry next tick */
    }

    header = retrieve_header_for_body_and_head(r->st.bodynum, head, 0);
    if (header == NULL)
    {
        return;
    }
    prop = chrAllocate(header, &pos, r->st.yaw, stan, (s32) ailistFindById(GAILIST_DEAD_AI));
    if (prop == NULL)
    {
        return;
    }
    chrpropActivateThisFrame(prop);
    chrpropEnable(prop);
    prop->chr->bodynum = r->st.bodynum;
    prop->chr->headnum = head;
    prop->chr->actiontype = ACT_BONDMULTI; /* not dispatched by chrlvActionTick */
    prop->chr->chrnum = COOP_PUPPET_CHRNUM + r->st.player; /* out of the way of the spawn numbers */
    prop->chr->sleep = 0;

    r->puppet = prop;
    r->puppetPrev = pos;
    r->puppetAnim = -1;
    r->puppetGhost = 1;
    coopPuppetSetAnim(r, prop->chr, PANIM_IDLE, 0.0f);
    portConLogf("ge: coop: puppet for player %d spawned at %.0f %.0f %.0f\n", r->st.player + 1, pos.f[0], pos.f[1], pos.f[2]);
    hudmsgBottomShow("A partner is here.\n");
}

static void coopPuppetUpdate(CoopRemote *r)
{
    ChrRecord *chr = r->puppet->chr;
    coord3d pos;
    StandTile *stan;
    f32 dx, dz, speed;

    pos.f[0] = r->st.pos[0];
    pos.f[1] = r->st.pos[1];
    pos.f[2] = r->st.pos[2];

    stan = coopFindStan(&pos, r->puppet->stan, &r->puppet->pos);
    if (stan == NULL)
    {
        return;
    }

    dx = pos.f[0] - r->puppetPrev.f[0];
    dz = pos.f[2] - r->puppetPrev.f[2];
    speed = sqrtf(dx * dx + dz * dz) / (g_ClockTimer > 0 ? (f32) g_ClockTimer : 1.0f);
    r->puppetPrev = pos;

    r->puppet->stan = stan;
    r->puppet->pos = pos;
    setsuboffset(chr->model, &pos);
    setsubroty(chr->model, r->st.yaw);
    sub_GAME_7F01FC10(chr->model, &r->puppet->pos, &r->puppet->pos, &chr->ground);
    chrDetectRooms(chr);

    if (speed > 4.0f)
    {
        coopPuppetSetAnim(r, chr, PANIM_RUN, speed);
    }
    else if (speed > 0.5f)
    {
        coopPuppetSetAnim(r, chr, PANIM_WALK, speed);
    }
    else
    {
        coopPuppetSetAnim(r, chr, PANIM_IDLE, 0.0f);
    }
}

/* Both Bonds start on the same pad, so the puppet stays non-solid until the
 * players have moved apart; after that they block each other like MP players. */
static void coopPuppetUpdateGhost(CoopRemote *r)
{
    PropRecord *bond = getCurrentPlayerProp();
    f32 dx = r->puppet->pos.f[0] - bond->pos.f[0];
    f32 dz = r->puppet->pos.f[2] - bond->pos.f[2];
    f32 d2 = dx * dx + dz * dz;

    if (d2 < 60.0f * 60.0f)
    {
        r->puppetGhost = 1;
    }
    else if (d2 > 90.0f * 90.0f)
    {
        r->puppetGhost = 0;
    }
}

static void coopPuppetRemove(CoopRemote *r)
{
    if (r->puppet->chr != NULL)
    {
        r->puppet->chr->hidden |= CHRHIDDEN_REMOVE;
    }
    r->puppet = NULL;
    portConLogf("ge: coop: puppet for player %d removed\n", r->st.player + 1);
}

s32 coopPropIsGhost(PropRecord *prop)
{
    s32 p = coopPlayerOfProp(prop);

    return p >= 0 && sR[p].puppetGhost;
}

void coopStageStart(void)
{
    s32 i;

    if (bossGetStageNum() != LEVELID_TITLE)
    {
        sMissionSerial++;
        sLobbyReady = 0; /* ready again in the lobby for the next one */
        coopStatsMissionStart();
    }

    for (i = 0; i < COOP_MAX_CHRS; i++)
    {
        sChrTarget[i].lastupdate60 = -1;
        sChrTarget[i].laststimulus60 = -1;
        sChrTarget[i].target = -1;
    }
    for (i = 0; i < COOP_MAX_CHRS; i++)
    {
        sChrLastSync[i] = 0;
        sChrFireSeen[i][0] = sChrFireSeen[i][1] = 0;
        sChrFiring[i] = 0;
    }
    sLastSnapTick = 0;
    sMissionTicks = 0;
    sNumPeerHeld = 0;
    sLastHurtTick = sLastFightTick = sDeadSince = 0;
    sLastHealth = 1.0f;
    sLastWeapon = ITEM_UNARMED;
    sTickChr = NULL;
    g_CoopPropOverride = NULL;
    for (i = 0; i < COOP_MAX_PLAYERS; i++)
    {
        sR[i].puppet = NULL;
        sR[i].puppetGhost = 0;
        sR[i].puppetAnim = -1;
        sR[i].valid = 0;
    }
    coopDifficultyStageStart();
}

void coopTick(void)
{
    if (sEnabled < 0)
    {
        sEnabled = coopNetInit();
    }
    if (!sEnabled || g_ClockTimer <= 0)
    {
        return;
    }

    sTick++;
    coopPollNet();
    coopFlushEvents();
    coopSendHello();

    if (!coopInMission())
    {
        return;
    }

    sMissionTicks++;
    coopStatsMissionTick(coopPartnerHere());
    coopSendState();
    coopTrackCombat();
    if ((sTick % 30) == 0 && coopPartnerHere())
    {
        coopSendWorld();
    }

    {   /* GE_COOP_TRACE=<chrnum>: log one chr on both sides to compare */
        static s32 trace = -2;
        if (trace == -2)
        {
            const char *e = getenv("GE_COOP_TRACE");
            trace = (e != NULL) ? atoi(e) : -1;
        }
        if (trace >= 0 && (sTick % 60) == 0)
        {
            ChrRecord *c = chrFindByLiteralId(trace);
            if (c != NULL && c->prop != NULL)
            {
                portConLogf("ge: coop: trace chr %d pos %.0f %.0f %.0f act %d anim %d frame %.1f\n", trace,
                            c->prop->pos.f[0], c->prop->pos.f[1], c->prop->pos.f[2], c->actiontype,
                            (s32) ((u8 *) objecthandlerGetModelAnim(c->model) - (u8 *) ptr_animation_table), c->model->animframe1);
            }
        }
    }

    if (coopNetIsHost() && coopNetHasPeer() && coopPartnerHere() && (sTick % 3) == 0)
    {
        coopSendGuards();
    }
    if (coopIsMirroringClient())
    {
        coopReapGuards();
        coopClientGunfire();
    }

    coopHarnessTick();

    coopRemotesTick();
}

/* the puppets: one per other player in this mission */
static void coopRemotesTick(void)
{
    s32 p;

    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        CoopRemote *r = &sR[p];

        if (p == coopSelf())
        {
            continue;
        }
        if (r->valid && sTick - r->seenTick > 300)
        {
            portConLogf("ge: coop: nothing from player %d for 5 s, dropping its Bond\n", p + 1);
            if (coopInMission())
            {
                hudmsgBottomShow("Lost contact with a partner.\n");
            }
            r->valid = 0;
        }
        if (r->puppet != NULL && !coopRemoteHere(p))
        {
            coopPuppetRemove(r);
        }
        if (!coopRemoteHere(p))
        {
            continue;
        }
        if (r->puppet == NULL)
        {
            coopPuppetSpawn(r);
        }
        else if (r->puppet->chr == NULL)
        {
            r->puppet = NULL; /* freed under us (stage cleanup); respawn */
        }
        else
        {
            coopPuppetUpdate(r);
            coopPuppetUpdateGhost(r);
            if ((sTick % 60) == 0)
            {
                ChrRecord *c = r->puppet->chr;
                portConLogf("ge: coop: puppet %d pos %.0f %.0f %.0f stan %p chrflags %x hidden %x propflags %x anim %d ghost %d\n",
                            p + 1, r->puppet->pos.f[0], r->puppet->pos.f[1], r->puppet->pos.f[2], (void *) r->puppet->stan,
                            c->chrflags, c->hidden, r->puppet->flags, r->puppetAnim, r->puppetGhost);
            }
        }
        if (r->resync && coopNetIsHost() && r->puppet != NULL)
        {
            r->resync = 0;
            coopResyncDoors(p);
            hudmsgBottomShow("A partner rejoined.\n");
        }
    }
    if (sLateJoin && coopRemoteAliveHere(0) && sMissionTicks > 30 && !g_CurrentPlayer->bonddead)
    {   /* joined a mission under way: go to the host, not the level's start */
        sLateJoin = 0;
        sR[0].puppetGhost = 1;
        coopRespawnPlayerAt(&sR[0].puppet->pos, sR[0].puppet->stan, 6.2831855f - sR[0].st.yaw, sLastWeapon);
        hudmsgBottomShow("Joined your partners.\n");
        portConLogf("ge: coop: late join, moved beside the host\n");
    }
}


/* ---- test harness hooks (env vars, see coop/NOTES.md) ------------------ */

static void coopHarnessTick(void)
{
    if (coopNetIsHost() && coopPartnerHere())
    {   /* GE_COOP_TESTFADE=1: the host's "script" fades to black at 15 s and back at 18 s (harness only) */
        static s32 testfade = -1;
        if (testfade == -1)
        {
            testfade = getenv("GE_COOP_TESTFADE") != NULL;
        }
        if (testfade && (sTick == 900 || sTick == 1080))
        {
            static u8 list[2];
            ChrRecord *c = chrFindByLiteralId(6);
            if (c != NULL)
            {
                AIRecord *sl = c->ailist;
                s32 so = c->aioffset;
                list[0] = (sTick == 900) ? AI_ScreenFadeToBlack : AI_ScreenFadeFromBlack;
                list[1] = AI_EndList;
                c->ailist = (AIRecord *) list;
                c->aioffset = 0;
                ai((PropDefHeaderRecord *) c, PROP_TYPE_CHR);
                c->ailist = sl;
                c->aioffset = so;
                portConLogf("ge: coop: testfade %d\n", list[0]);
            }
        }
    }
    if (coopIsMirroringClient())
    {   /* GE_COOP_TESTBOOM=<chrnum>: at 20 s, a grenade-sized blast on that chr (harness only) */
        static s32 testboom = -2;
        if (testboom == -2)
        {
            const char *e = getenv("GE_COOP_TESTBOOM");
            testboom = (e != NULL) ? atoi(e) : -1;
        }
        if (testboom >= 0 && sTick >= 1200)
        {
            ChrRecord *c = chrFindByLiteralId(testboom);
            testboom = -1;
            if (c != NULL && c->prop != NULL)
            {
                portConLogf("ge: coop: testboom on chr %d\n", c->chrnum);
                explosionCreate(NULL, &c->prop->pos, c->prop->stan, 2, 0, 0, c->prop->rooms, 0);
            }
        }
    }
    {   /* GE_COOP_TESTDIE=<ticks>: kill our Bond that far into the mission, 900 if not given (harness only) */
        static s32 testdie = -1;
        if (testdie == -1)
        {
            const char *e = getenv("GE_COOP_TESTDIE");
            testdie = (e == NULL) ? 0 : (atoi(e) > 1) ? atoi(e) : 900;
        }
        if (testdie && sMissionTicks >= (u32) testdie && coopPartnerHere() && !g_CurrentPlayer->bonddead)
        {
            testdie = 0;
            portConLogf("ge: coop: testdie\n");
            bondviewCallRecordDamageKills(10.0f, 0.0f, -1, 1);
        }
    }
    {   /* GE_COOP_TESTEND=<ticks>: complete the mission that far in, 1200 if not given (harness only) */
        static s32 testend = -1;
        if (testend == -1)
        {
            const char *e = getenv("GE_COOP_TESTEND");
            testend = (e == NULL) ? 0 : (atoi(e) > 1) ? atoi(e) : 1200;
        }
        if (testend && sMissionTicks >= (u32) testend && coopPartnerHere())
        {
            testend = 0;
            portConLogf("ge: coop: testend\n");
            bossReturnTitleStage();
        }
    }
    {   /* GE_COOP_TESTWORLD=1: host raises the alarm and a stage flag at 15 s (harness only) */
        static s32 testworld = -1;
        if (testworld == -1)
        {
            testworld = getenv("GE_COOP_TESTWORLD") != NULL;
        }
        if (testworld && coopNetIsHost() && sTick >= 900)
        {
            testworld = 0;
            alarmActivate();
            objectiveregisters1 |= 0x40000000;
            portConLogf("ge: coop: testworld: alarm + flag 0x40000000\n");
        }
    }
    if (coopIsMirroringClient())
    {   /* GE_COOP_TESTSHOT=<chrnum>: a chest shot every 2 s (harness only) */
        static s32 testchr = -2;
        if (testchr == -2)
        {
            const char *e = getenv("GE_COOP_TESTSHOT");
            testchr = (e != NULL) ? atoi(e) : -1;
        }
        if (testchr >= 0 && (sTick % 120) == 0)
        {
            ChrRecord *c = chrFindByLiteralId(testchr);
            coord3d v = { 0.0f, 0.0f, 1.0f };
            if (c != NULL && c->prop != NULL)
            {
                coopClientForwardShot(c, HIT_CHEST, &v, ITEM_WPPK);
            }
        }
    }

    if (coopPartnerHere() && coopInMission())
    {   /* GE_COOP_TESTGOTO=<chrnum>: once, 5 s into the mission, stand where that chr stands (harness only) */
        static s32 testgoto = -2;
        static s32 inmission;
        if (testgoto == -2)
        {
            const char *e = getenv("GE_COOP_TESTGOTO");
            testgoto = (e != NULL) ? atoi(e) : -1;
        }
        if (testgoto >= 0 && ++inmission == 300)
        {
            ChrRecord *c = chrFindByLiteralId(testgoto);
            if (c != NULL && c->prop != NULL && c->prop->stan != NULL)
            {
                coopRespawnPlayerAt(&c->prop->pos, c->prop->stan, 0.0f, sLastWeapon);
                portConLogf("ge: coop: testgoto -> chr %d at %.0f %.0f %.0f\n", testgoto,
                            c->prop->pos.f[0], c->prop->pos.f[1], c->prop->pos.f[2]);
            }
        }
    }
    if (coopPartnerHere() && coopInMission())
    {   /* GE_COOP_TESTGIVE=<s>: <s> seconds into the mission, face the partner and
         * press give_partner; GE_COOP_TESTGIVEWEAPON=<item> draws that gun 3 s
         * before, with 100 rounds (harness only) */
        static s32 testgive = -2;
        static s32 testweapon = -1;
        if (testgive == -2)
        {
            const char *e = getenv("GE_COOP_TESTGIVE");
            const char *w = getenv("GE_COOP_TESTGIVEWEAPON");
            testgive = (e != NULL) ? atoi(e) * 60 : -1;
            testweapon = (w != NULL) ? atoi(w) : -1;
        }
        if (testweapon > 0 && testgive > 180 && (s32) sMissionTicks == testgive - 180)
        {
            coopGiveTestDraw(testweapon);
        }
        if (testgive > 0 && (s32) sMissionTicks == testgive)
        {
            coopGiveTestPress();
        }
    }
    if (coopNetIsHost() && coopPartnerHere())
    {   /* GE_COOP_TESTSPAWN=<chrnum>: once, at 15 s, spawn a copy of that chr next to Bond (harness only) */
        static s32 testspawn = -2;
        if (testspawn == -2)
        {
            const char *e = getenv("GE_COOP_TESTSPAWN");
            testspawn = (e != NULL) ? atoi(e) : -1;
        }
        if (testspawn >= 0 && sTick >= 900 && coopInMission())
        {
            static s32 done;
            if (done++) { testspawn = -1; }
        }
        if (testspawn >= 0 && sTick >= 900 && coopInMission())
        {
            ChrRecord *src = chrFindByLiteralId(testspawn);
            PropRecord *bond = getCurrentPlayerProp();
            if (src != NULL && bond != NULL)
            {
                PropRecord *np = chrSpawnAtCoord(src->bodynum, src->headnum, &src->prop->pos, src->prop->stan, 0.0f, src->ailist, 0x10);
                if (np != NULL)
                {
                    chrGiveWeapon(np->chr, PROP_CHRKALASH, ITEM_AK47, 0);
                }
                portConLogf("ge: coop: testspawn -> chr %d (free %d)\n", np ? np->chr->chrnum : -1, chrGetNumFree());
            }
        }
    }
    if (coopPartnerHere())
    {   /* GE_COOP_TESTOBJ=1: every 5 s, a heavy "shot" on the nearest breakable setup object (harness only) */
        static s32 testobj = -1;
        if (testobj == -1)
        {
            testobj = getenv("GE_COOP_TESTOBJ") != NULL;
        }
        if (testobj && (sTick % 300) == 150 && coopInMission())
        {
            PropRecord *bond = getCurrentPlayerProp();
            PropRecord *it, *best = NULL;
            f32 bestd = 1e30f;

            for (it = g_ActivePropsHead; it != NULL; it = it->next)
            {
                if (it->type == PROP_TYPE_OBJ && it->obj != NULL && coopSetupOfs(it->obj) >= 0
                    && objIsMortal(it->obj) && !objGetDestroyedLevel(it->obj))
                {
                    f32 dx = it->pos.f[0] - bond->pos.f[0];
                    f32 dz = it->pos.f[2] - bond->pos.f[2];
                    if (dx * dx + dz * dz < bestd)
                    {
                        bestd = dx * dx + dz * dz;
                        best = it;
                    }
                }
            }
            if (best != NULL)
            {
                portConLogf("ge: coop: testobj %d model %d (dist %.0f)\n", coopSetupOfs(best->obj), best->obj->obj, sqrtf(bestd));
                objApplyDamage(best->obj, 3.0f, &best->pos, ITEM_AK47, 0);
            }
        }
    }

    if (coopPartnerHere())
    {   /* GE_COOP_TESTPICK=1: every 10 s, grab the nearest pickup in the setup (harness only) */
        static s32 testpick = -1;
        if (testpick == -1)
        {
            testpick = getenv("GE_COOP_TESTPICK") != NULL;
        }
        if (testpick && (sTick % 600) == 450 && coopInMission())
        {
            PropRecord *bond = getCurrentPlayerProp();
            PropRecord *it, *best = NULL;
            f32 bestd = 1e30f;

            for (it = g_ActivePropsHead; it != NULL; it = it->next)
            {
                if ((it->type == PROP_TYPE_WEAPON || it->type == PROP_TYPE_OBJ) && it->obj != NULL
                    && coopSetupOfs(it->obj) >= 0 && (it->obj->type == PROPDEF_COLLECTABLE || it->obj->type == PROPDEF_AMMO
                        || it->obj->type == PROPDEF_KEY || it->obj->type == PROPDEF_ARMOUR))
                {
                    f32 dx = it->pos.f[0] - bond->pos.f[0];
                    f32 dz = it->pos.f[2] - bond->pos.f[2];
                    if (dx * dx + dz * dz < bestd)
                    {
                        bestd = dx * dx + dz * dz;
                        best = it;
                    }
                }
            }
            if (best != NULL)
            {
                TICKOP op;
                portConLogf("ge: coop: testpick %d type %d (dist %.0f)\n", coopSetupOfs(best->obj), best->obj->type, sqrtf(bestd));
                op = propPickupByPlayer(best, TRUE);
                propExecuteTickOperation(best, op);
            }
        }
    }

    if (coopPartnerHere())
    {   /* GE_COOP_TESTDOOR=1: toggle the door nearest Bond every 10 s (harness only) */
        static s32 testdoor = -1;
        if (testdoor == -1)
        {
            testdoor = getenv("GE_COOP_TESTDOOR") != NULL;
        }
        if (testdoor && (sTick % 600) == 300)
        {
            PropRecord *bond = getCurrentPlayerProp();
            PropRecord *it, *best = NULL;
            f32 bestd = 1e30f;

            for (it = g_ActivePropsHead; it != NULL; it = it->next)
            {
                if (it->type == PROP_TYPE_DOOR && it->door != NULL)
                {
                    f32 dx = it->pos.f[0] - bond->pos.f[0];
                    f32 dz = it->pos.f[2] - bond->pos.f[2];
                    if (dx * dx + dz * dz < bestd)
                    {
                        bestd = dx * dx + dz * dz;
                        best = it;
                    }
                }
            }
            if (best != NULL)
            {
                s32 st = doorIsClosed(best->door) ? DOORSTATE_OPENING : DOORSTATE_CLOSING;
                portConLogf("ge: coop: testdoor %d -> state %d (dist %.0f)\n",
                    (s32) ((u8 *) best->door - (u8 *) g_CurrentSetup.propDefs), st, sqrtf(bestd));
                doorActivate(best->door, st);
            }
        }
    }
}

/* ---- host guard targeting ---------------------------------------------- */

static s32 coopTargetable(s32 t)
{
    if (t == COOPTGT_LOCAL)
    {
        return !g_CurrentPlayer->bonddead;
    }
    return coopRemoteAliveHere(t);
}

static PropRecord *coopTargetProp(s32 t)
{
    return (t == COOPTGT_LOCAL) ? g_CurrentPlayer->prop : sR[t].puppet;
}

static s32 coopAnyRemoteTargetable(void)
{
    s32 t;

    for (t = 1; t < COOPTGT_COUNT; t++)
    {
        if (coopTargetable(t))
        {
            return 1;
        }
    }
    return 0;
}

static CoopChrTarget *coopChrState(ChrRecord *chr)
{
    s32 slot;

    if (g_ChrSlots == NULL || chr == NULL)
    {
        return NULL;
    }
    slot = chr - g_ChrSlots;
    if (slot < 0 || slot >= g_NumChrSlots || slot >= COOP_MAX_CHRS)
    {
        return NULL;
    }
    return &sChrTarget[slot];
}

static s32 coopNearestTarget(ChrRecord *chr)
{
    s32 best = -1;
    f32 bestd2 = 1.0e30f;
    s32 t;

    for (t = 0; t < COOPTGT_COUNT; t++)
    {
        PropRecord *p;
        f32 dx, dy, dz, d2;

        if (!coopTargetable(t))
        {
            continue;
        }
        p = coopTargetProp(t);
        dx = p->pos.f[0] - chr->prop->pos.f[0];
        dy = p->pos.f[1] - chr->prop->pos.f[1];
        dz = p->pos.f[2] - chr->prop->pos.f[2];
        d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestd2)
        {
            bestd2 = d2;
            best = t;
        }
    }
    return best;
}

/* Passive guards may rotate candidates; once a player-relative action runs,
 * changing Bond under it makes the retail AI list contradict itself. */
static s32 coopActionAllowsScan(ChrRecord *chr)
{
    switch (chr->actiontype)
    {
        case ACT_INIT:
        case ACT_STAND:
        case ACT_PATROL:
            return TRUE;
        default:
            return FALSE;
    }
}

/* Acquire -> commit -> release (from GE+): a passive guard scans players a
 * few times a second, a real stimulus commits it, and it lets go only after
 * ten seconds without one and back in a passive action. A dead target is
 * dropped at once. */
static s32 coopChrTarget(ChrRecord *chr)
{
    CoopChrTarget *st = coopChrState(chr);
    s32 next;

    if (st == NULL || !coopAnyRemoteTargetable())
    {
        return COOPTGT_LOCAL;
    }

    if (st->target >= 0 && coopTargetable(st->target))
    {
        if (st->laststimulus60 >= 0)
        {
            if (g_GlobalTimer - st->laststimulus60 < CHRLV_10_SEC_TIMER || !coopActionAllowsScan(chr))
            {
                return st->target;
            }
            st->laststimulus60 = -1;
            st->lastupdate60 = g_GlobalTimer;
        }
        else if (!coopActionAllowsScan(chr))
        {
            return st->target;
        }

        if (g_GlobalTimer - st->lastupdate60 < 12)
        {
            return st->target;
        }

        for (next = (st->target + 1) % COOPTGT_COUNT; next != st->target; next = (next + 1) % COOPTGT_COUNT)
        {   /* the next player round that can be targeted */
            if (coopTargetable(next))
            {
                st->target = next;
                st->lastupdate60 = g_GlobalTimer;
                break;
            }
        }
        return st->target;
    }

    next = coopNearestTarget(chr);
    if (next < 0)
    {
        next = COOPTGT_LOCAL;
    }
    st->target = next;
    st->lastupdate60 = g_GlobalTimer;
    st->laststimulus60 = -1;
    return next;
}

static s32 coopIsMirroringClient(void)
{
    return sEnabled && !coopNetIsHost() && sLastSnapTick != 0 && coopRemoteHere(0);
}

s32 coopChrTickBegin(ChrRecord *chr)
{
    s32 t;

    sTickChr = NULL;
    g_CoopPropOverride = NULL;

    if (!sEnabled || coopIsPuppet(chr->prop))
    {
        return 1;
    }
    if (!coopNetIsHost())
    {
        return !coopIsMirroringClient();
    }
    if (!coopPartnerHere())
    {
        return 1;
    }
    t = coopChrTarget(chr);
    if (t != COOPTGT_LOCAL)
    {
        static ChrRecord *logged[8];
        s32 k;

        for (k = 0; k < 8 && logged[k] != NULL && logged[k] != chr; k++)
        {
        }
        if (k < 8 && logged[k] == NULL)
        {
            logged[k] = chr;
            portConLogf("ge: coop: guard %d now targets player %d\n", chr->chrnum, t + 1);
        }
        sTickChr = chr;
        g_CoopPropOverride = sR[t].puppet;
    }
    return 1;
}

void coopChrTickEnd(void)
{
    sTickChr = NULL;
    g_CoopPropOverride = NULL;
}

/* A real stimulus (seen, heard, shot by) commits the guard to whichever
 * player is "Bond" right now. Ownership doesn't ping-pong while the current
 * target is still fresh. */
void coopChrStimulus(ChrRecord *chr)
{
    CoopChrTarget *st;
    s32 t = (g_CoopPropOverride != NULL) ? coopPlayerOfProp(g_CoopPropOverride) : COOPTGT_LOCAL;

    if (!sEnabled || !coopNetIsHost() || chr == NULL || coopIsPuppet(chr->prop))
    {
        return;
    }
    st = coopChrState(chr);
    if (st == NULL || t < 0)
    {
        return;
    }
    if (st->target != t && st->target >= 0 && coopTargetable(st->target)
        && st->laststimulus60 >= 0 && g_GlobalTimer - st->laststimulus60 < CHRLV_SEEN_RECENT_CHECK)
    {
        return;
    }
    st->target = t;
    st->lastupdate60 = g_GlobalTimer;
    st->laststimulus60 = g_GlobalTimer;
}

s32 coopTargetIsRemote(void)
{
    return g_CoopPropOverride != NULL;
}

/* bondviewUpdateGuardTankFlagsRelated for whichever Bond the guard is on:
 * takes the target out of the line-of-sight test against itself. */
void coopGuardTankFlags(s32 flag)
{
    if (g_CoopPropOverride != NULL)
    {
        chrSetMoving(g_CoopPropOverride->chr, flag);
        return;
    }
    bondviewUpdateGuardTankFlagsRelated(g_CurrentPlayer->prop, flag);
}

f32 coopTargetDuckingHeight(void)
{
    if (g_CoopPropOverride != NULL)
    {
        return g_CoopPropOverride->chr->chrheight * 0.5f;
    }
    return bondviewGetPlayerDuckingHeightRelated(g_CurrentPlayer);
}

s32 coopTargetDamageShowTime(void)
{
    if (g_CoopPropOverride != NULL)
    {
        return 0;
    }
    return bondviewGetIfCurrentPlayerDamageShowTime();
}

s32 coopTargetIsDead(void)
{
    if (g_CoopPropOverride != NULL)
    {
        s32 p = coopPlayerOfProp(g_CoopPropOverride);

        return p < 0 || sR[p].st.dead;
    }
    return g_CurrentPlayer->bonddead;
}

/* Damage a guard dealt to "Bond". Returns 1 when it belonged to the peer. */
s32 coopTargetRecordDamage(f32 damage, f32 angle)
{
    CoopEvent ev;

    if (g_CoopPropOverride == NULL)
    {
        return 0;
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_HIT;
    ev.u.hit.damage = damage;
    ev.u.hit.angle = angle;
    coopQueueEventTo(&ev, coopPlayerOfProp(g_CoopPropOverride));
    portConLogf("ge: coop: guard %d hit player %d: %.3f\n", sTickChr ? sTickChr->chrnum : -1,
                coopPlayerOfProp(g_CoopPropOverride) + 1, damage);
    return 1;
}


/* ---- guard mirroring (host sends, client applies) ----------------------- */

static void coopSendGuards(void)
{
    CoopGuardsPkt pkt;
    s32 i;
    s32 index = 0;
    s32 hdr = (s32) ((u8 *) &pkt.ent[0] - (u8 *) &pkt);

    sGuardSnap++;
    memset(&pkt, 0, hdr);
    pkt.magic = COOP_MAGIC;
    pkt.version = COOP_VERSION;
    pkt.type = COOPPKT_GUARDS;
    pkt.snap = sGuardSnap;

    for (i = 0; i < g_NumChrSlots; i++)
    {
        ChrRecord *chr = &g_ChrSlots[i];
        CoopGuardEnt *e;
        ModelAnimation *anim;

        if (chr->model == NULL || chr->prop == NULL || coopIsPuppet(chr->prop))
        {
            continue;
        }

        if (pkt.count == 0)
        {
            pkt.first = (u16) index;
        }
        e = &pkt.ent[pkt.count++];
        index++;

        e->chrnum = chr->chrnum;
        e->flags = 0;
        if (i < COOP_MAX_CHRS)
        {   /* firecount ticks on every fire tick, shot or not */
            if (chr->firecount[GUNRIGHT] != sChrFireSeen[i][GUNRIGHT])
            {
                e->flags |= GSF_FIRE_R;
            }
            if (chr->firecount[GUNLEFT] != sChrFireSeen[i][GUNLEFT])
            {
                e->flags |= GSF_FIRE_L;
            }
            sChrFireSeen[i][GUNRIGHT] = chr->firecount[GUNRIGHT];
            sChrFireSeen[i][GUNLEFT] = chr->firecount[GUNLEFT];
        }
        e->body = (u8) chr->bodynum;
        e->head = (u8) chr->headnum;
        {
            PropRecord *gun = chrGetEquippedWeaponProp(chr, GUNRIGHT);
            if (gun != NULL && gun->weapon != NULL)
            {
                e->gunitem = gun->weapon->weaponnum;
                e->gunmodel = gun->weapon->obj;
            }
        }
        if (chr->chrflags & CHRFLAG_HIDDEN)
        {
            e->flags |= GSF_HIDDEN;
        }
        if (objecthandlerGetModelGunhand(chr->model))
        {
            e->flags |= GSF_FLIP;
        }
        if (chr->model->animlooping)
        {
            e->flags |= GSF_LOOP;
        }
        e->pos[0] = chr->prop->pos.f[0];
        e->pos[1] = chr->prop->pos.f[1];
        e->pos[2] = chr->prop->pos.f[2];
        e->yaw = getsubroty(chr->model);
        anim = objecthandlerGetModelAnim(chr->model);
        e->animoff = (anim != NULL) ? (s32) ((u8 *) anim - (u8 *) ptr_animation_table) : -1;
        e->frame = chr->model->animframe1;
        e->speed = chr->model->speed;

        if (pkt.count == COOP_GUARDS_PER_PKT)
        {
            coopNetSend(&pkt, hdr + pkt.count * (s32) sizeof(CoopGuardEnt));
            pkt.count = 0;
        }
    }
    if (pkt.count > 0)
    {
        coopNetSend(&pkt, hdr + pkt.count * (s32) sizeof(CoopGuardEnt));
    }
}

/* Client: a chr the host has and we don't (spawned mid-mission, a clone,
 * a reinforcement). Make a local stand-in with the same chrnum; the host's
 * snapshots drive it from here on. */
static s32 sMirrorSpawnsThisTick;
static u32 sMirrorSpawnTick;

static ChrRecord *coopMirrorSpawn(CoopGuardEnt *e)
{
    coord3d pos;
    StandTile *stan;
    Model *header;
    PropRecord *prop;
    ChrRecord *chr;

    if (e->flags & GSF_HIDDEN)
    {
        return NULL; /* script chrs that never show; nothing to see */
    }
    if (sMirrorSpawnTick != sTick)
    {
        sMirrorSpawnTick = sTick;
        sMirrorSpawnsThisTick = 0;
    }
    if (sMirrorSpawnsThisTick >= 4 || chrGetNumFree() < 3)
    {
        return NULL; /* try again on the next snapshot */
    }
    pos.f[0] = e->pos[0];
    pos.f[1] = e->pos[1];
    pos.f[2] = e->pos[2];
    stan = stanFindTileBelowPos(&pos, NULL, NULL);
    if (stan == NULL)
    {
        return NULL;
    }
    header = retrieve_header_for_body_and_head(e->body, e->head, 0);
    if (header == NULL)
    {
        return NULL;
    }
    prop = chrAllocate(header, &pos, e->yaw, stan, (s32) ailistFindById(GAILIST_DEAD_AI));
    if (prop == NULL)
    {
        return NULL;
    }
    sMirrorSpawnsThisTick++;
    chrpropActivateThisFrame(prop);
    chrpropEnable(prop);
    chr = prop->chr;
    chr->chrnum = e->chrnum;
    chr->bodynum = e->body;
    chr->headnum = e->head;
    if (e->gunitem > 0)
    {
        chrGiveWeapon(chr, e->gunmodel, e->gunitem, 0);
    }
    portConLogf("ge: coop: mirrored host chr %d (body %d, gun %d) at %.0f %.0f %.0f\n",
        e->chrnum, e->body, e->gunitem, pos.f[0], pos.f[1], pos.f[2]);
    return chr;
}

static void coopApplyGuard(CoopGuardEnt *e)
{
    ChrRecord *chr = chrFindByLiteralId(e->chrnum);
    PropRecord *prop;
    coord3d pos;
    StandTile *stan;
    s32 slot;

    if (chr == NULL)
    {
        chr = coopMirrorSpawn(e);
    }
    if (chr == NULL || chr->prop == NULL || coopIsPuppet(chr->prop))
    {
        return;
    }
    prop = chr->prop;
    slot = chr - g_ChrSlots;
    if (slot >= 0 && slot < COOP_MAX_CHRS)
    {
        sChrLastSync[slot] = sTick;
        sChrFiring[slot] = e->flags & (GSF_FIRE_R | GSF_FIRE_L);
    }

    if (e->flags & GSF_HIDDEN)
    {
        chr->chrflags |= CHRFLAG_HIDDEN;
    }
    else
    {
        chr->chrflags &= ~CHRFLAG_HIDDEN;
    }

    pos.f[0] = e->pos[0];
    pos.f[1] = e->pos[1];
    pos.f[2] = e->pos[2];
    stan = coopFindStan(&pos, prop->stan, &prop->pos);
    if (stan != NULL)
    {
        prop->stan = stan;
        prop->pos = pos;
        setsuboffset(chr->model, &pos);
        chrDetectRooms(chr);
    }
    setsubroty(chr->model, e->yaw);
    chr->chrflags |= CHRFLAG_INIT;

    if (e->animoff >= 0)
    {
        ModelAnimation *anim = (ModelAnimation *) ((u8 *) ptr_animation_table + e->animoff);
        s32 flip = (e->flags & GSF_FLIP) != 0;

        if (objecthandlerGetModelAnim(chr->model) != anim || (objecthandlerGetModelGunhand(chr->model) != 0) != flip)
        {
            modelSetAnimation(chr->model, anim, flip, e->frame, e->speed, 8.0f);
            if (e->flags & GSF_LOOP)
            {
                modelSetAnimLooping(chr->model, 0, 8.0f);
            }
        }
        else
        {
            f32 d = chr->model->animframe1 - e->frame;

            if (d > 6.0f || d < -6.0f)
            {
                modelSetAnimation(chr->model, anim, flip, e->frame, e->speed, 0.0f);
                if (e->flags & GSF_LOOP)
                {
                    modelSetAnimLooping(chr->model, 0, 0.0f);
                }
            }
            else if (chr->model->speed != e->speed)
            {
                modelSetAnimSpeed(chr->model, e->speed, 0.0f);
            }
        }
    }
}

/* Client: muzzle flash and gun sound for guards the host says are firing.
 * Damage is the host's business; this is only what the player sees and hears. */
static void coopClientGunfire(void)
{
    s32 i, h;

    for (i = 0; i < g_NumChrSlots && i < COOP_MAX_CHRS; i++)
    {
        ChrRecord *chr = &g_ChrSlots[i];

        if (chr->model == NULL || chr->prop == NULL || coopIsPuppet(chr->prop))
        {
            continue;
        }
        chr->hidden &= ~CHRHIDDEN_FIRE_TRACER;
        for (h = GUNRIGHT; h <= GUNLEFT; h++)
        {
            s32 firing = (sChrFiring[i] & (h == GUNRIGHT ? GSF_FIRE_R : GSF_FIRE_L)) != 0;

            if (chrGetEquippedWeaponProp(chr, h) == NULL)
            {
                continue;
            }
            chrSetFiring(chr, h, firing);
            if (firing)
            {
                if ((sTick % 60) == 0)
                {
                    portConLogf("ge: coop: chr %d firing (mirrored)\n", chr->chrnum);
                }
                sub_GAME_7F02BFE4(chr, h, 1);
            }
        }
    }
}

/* Chrs the host freed stop showing up in snapshots; free ours after 2 s. */
static void coopReapGuards(void)
{
    s32 i;

    if (sTick - sLastSnapTick > 30)
    {
        return; /* snapshots stalled: don't read silence as removal */
    }
    for (i = 0; i < g_NumChrSlots && i < COOP_MAX_CHRS; i++)
    {
        ChrRecord *chr = &g_ChrSlots[i];

        if (chr->model == NULL || chr->prop == NULL || coopIsPuppet(chr->prop))
        {
            continue;
        }
        if (sChrLastSync[i] == 0)
        {
            sChrLastSync[i] = sTick; /* start the clock when we first see it */
        }
        else if (sTick - sChrLastSync[i] > 120 && !(chr->hidden & CHRHIDDEN_REMOVE))
        {
            portConLogf("ge: coop: chr %d gone on the host, removing\n", chr->chrnum);
            chr->hidden |= CHRHIDDEN_REMOVE;
        }
    }
}

s32 coopClientForwardShot(ChrRecord *chr, s32 hitpart, coord3d *vector, s32 weaponid)
{
    CoopEvent ev;

    if (!coopIsMirroringClient() || chr == NULL || coopIsPuppet(chr->prop))
    {
        return 0;
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_SHOTCHR;
    ev.u.shot.chrnum = chr->chrnum;
    ev.u.shot.hitpart = (s16) hitpart;
    ev.u.shot.weaponid = weaponid;
    ev.u.shot.vector[0] = vector->f[0];
    ev.u.shot.vector[1] = vector->f[1];
    ev.u.shot.vector[2] = vector->f[2];
    coopQueueEventTo(&ev, 0);
    portConLogf("ge: coop: shot guard %d (part %d, weapon %d), sent to host\n", chr->chrnum, hitpart, weaponid);
    return 1;
}

/* Host: apply a client's shot as if the remote player fired it. */
static void coopHostApplyShot(CoopEvent *ev)
{
    ChrRecord *chr = chrFindByLiteralId(ev->u.shot.chrnum);
    coord3d vec;

    if (chr == NULL || chr->prop == NULL || !coopRemoteHere(ev->from) || sR[ev->from].puppet == NULL)
    {
        return;
    }
    vec.f[0] = ev->u.shot.vector[0];
    vec.f[1] = ev->u.shot.vector[1];
    vec.f[2] = ev->u.shot.vector[2];
    g_CoopPropOverride = sR[ev->from].puppet; /* the stimulus belongs to that player */
    coopStatsBlameBegin(ev->from);            /* and so do the hit and the kill */
    handles_shot_actors(chr, ev->u.shot.hitpart, &vec, ev->u.shot.weaponid, TRUE);
    coopStatsBlameEnd();
    g_CoopPropOverride = NULL;
    portConLogf("ge: coop: player %d shot guard %d (part %d)\n", ev->from + 1, ev->u.shot.chrnum, ev->u.shot.hitpart);
}

/* ---- reliable events -------------------------------------------------- */

static void coopChanQueue(s32 c, CoopEvent *ev)
{
    CoopChan *ch = &sChan[c];

    if (!coopNetPeerConnected(c))
    {
        return;
    }
    if (ch->outNext - ch->outAcked > COOP_EVENT_RING)
    {
        portConLogf("ge: coop: event queue to player %d full, dropping kind %d\n", coopPlayerOfChan(c) + 1, ev->kind);
        return;
    }
    ch->out[ch->outNext % COOP_EVENT_RING] = *ev;
    ch->outNext++;
}

/* To player `to`, or COOP_ALL. A joiner sends everything to the host, which
 * passes on what is not only for itself (coopRecvEvents). */
static void coopQueueEventTo(CoopEvent *ev, s32 to)
{
    s32 c;

    if (!sEvForwarding)
    {
        ev->from = (u8) coopSelf();
    }
    ev->to = (u8) to;
    if (!coopNetIsHost())
    {
        if (to != coopSelf())
        {
            coopChanQueue(0, ev);
        }
        return;
    }
    for (c = 0; c < COOP_NET_MAX_PEERS; c++)
    {
        s32 p = coopPlayerOfChan(c);

        if ((to == COOP_ALL || to == p) && p != ev->from)
        {
            coopChanQueue(c, ev);
        }
    }
}

static void coopQueueEvent(CoopEvent *ev)
{
    coopQueueEventTo(ev, COOP_ALL);
}

/* Send every unacked event at most every 4 ticks (at once when there is
 * something new or an ack owed). Repeats cover loss; seq numbers drop the
 * duplicates on the other side. */

static void coopFlushChan(s32 c)
{
    CoopChan *ch = &sChan[c];
    CoopEventsPkt pkt;
    s32 hdr = (s32) ((u8 *) &pkt.ev[0] - (u8 *) &pkt);
    u32 seq;
    s32 pending = (s32) (ch->outNext - 1 - ch->outAcked);
    s32 fresh = (s32) (ch->outNext - 1 - ch->outSentUpTo) > 0;

    if (!coopNetPeerConnected(c) || (!ch->ackDirty && (pending <= 0 || (!fresh && sTick - ch->evLastSend < 4))))
    {
        return;
    }

    memset(&pkt, 0, hdr);
    pkt.magic = COOP_MAGIC;
    pkt.version = COOP_VERSION;
    pkt.type = COOPPKT_EVENTS;
    pkt.ack = ch->inDone;
    pkt.session = coopSelfSession();
    pkt.firstseq = ch->outAcked + 1;
    for (seq = ch->outAcked + 1; seq < ch->outNext && pkt.count < COOP_EVENTS_PER_PKT; seq++)
    {
        pkt.ev[pkt.count++] = ch->out[seq % COOP_EVENT_RING];
    }
    coopNetSendTo(c, &pkt, hdr + (s32) (pkt.count * sizeof(CoopEvent)));
    ch->outSentUpTo = seq - 1;
    ch->evLastSend = sTick;
    ch->ackDirty = 0;
}

static void coopFlushEvents(void)
{
    s32 c;

    for (c = 0; c < COOP_NET_MAX_PEERS; c++)
    {
        coopFlushChan(c);
    }
}

static void coopApplyEvent(CoopEvent *ev)
{
    switch (ev->kind)
    {
        case COOPEV_HIT:
            if (coopInMission() && !g_CurrentPlayer->bonddead)
            {
                portConLogf("ge: coop: hit from a host guard: %.3f\n", ev->u.hit.damage);
                bondviewCallRecordDamageKills(ev->u.hit.damage, ev->u.hit.angle, -1, 1);
            }
            break;
        case COOPEV_SHOTCHR:
            if (coopNetIsHost() && coopInMission())
            {
                coopHostApplyShot(ev);
            }
            break;
        case COOPEV_DOOR:
            coopApplyDoor(ev->u.door.doorofs, ev->u.door.state);
            break;
        case COOPEV_OBJDMG:
            coopApplyObjDamage(ev);
            break;
        case COOPEV_PICKUP:
            coopApplyPickup(ev);
            break;
        case COOPEV_MISSIONEND:
            coopApplyMissionEnd(ev);
            break;
        case COOPEV_EXPLODE:
            coopApplyExplosion(ev);
            break;
        case COOPEV_AICMD:
            coopApplyAiCommand(ev);
            break;
        case COOPEV_STATS:
            coopStatsApply(ev->u.raw, ev->from);
            break;
        case COOPEV_GIVE:
            coopGiveReceive(&ev->u.give, ev->from);
            break;
        case COOPEV_GIVEBACK:
            coopGiveReturned(&ev->u.give, ev->from);
            break;
    }
}

/* coop_stats.c: our end-of-mission column for the partner */
void coopSendStatsEvent(const void *payload, s32 len)
{
    CoopEvent ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_STATS;
    memcpy(ev.u.raw, payload, (len < (s32) sizeof(ev.u.raw)) ? len : (s32) sizeof(ev.u.raw));
    coopQueueEvent(&ev);
}

/* coop_give.c: a give to the partner, or our answer to theirs */
void coopGiveSend(s32 answer, s32 to, CoopGive *give)
{
    CoopEvent ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = answer ? COOPEV_GIVEBACK : COOPEV_GIVE;
    ev.u.give = *give;
    coopQueueEventTo(&ev, to);
}


/* ---- doors ---------------------------------------------------------------
 * Both sides run door motion; only state changes cross the wire. A door is
 * named by its byte offset in the stage's prop defs, which both sides load
 * from the same setup file. */

static s32 sApplyingRemoteDoor;

void coopDoorStateChanged(DoorRecord *door, s32 state)
{
    CoopEvent ev;

    if (!sEnabled || sApplyingRemoteDoor || !coopNetHasPeer() || !coopPartnerHere() || g_CurrentSetup.propDefs == NULL)
    {
        return;
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_DOOR;
    ev.u.door.doorofs = coopSetupOfs(door);
    if (ev.u.door.doorofs < 0)
    {
        return;
    }
    ev.u.door.state = state;
    coopQueueEvent(&ev);
}

static void coopApplyDoor(s32 doorofs, s32 state)
{
    DoorRecord *door;

    if (coopSetupOfs((u8 *) g_CurrentSetup.propDefs + doorofs) < 0)
    {
        return;
    }
    door = (DoorRecord *) ((u8 *) g_CurrentSetup.propDefs + doorofs);
    if (door->type != PROPDEF_DOOR)
    {
        portConLogf("ge: coop: door event for offset %d is not a door here\n", doorofs);
        return;
    }
    sApplyingRemoteDoor = 1;
    doorSetOpenState(door, state);
    sApplyingRemoteDoor = 0;
    portConLogf("ge: coop: peer door %d -> state %d\n", doorofs, state);
}


/* Byte offset of a record in the stage's setup prop defs, or -1 for things
 * made at run time (thrown grenades, dropped guns). Both sides load the same
 * setup file, so the offset names the same object on each. */

static s32 coopSetupOfs(void *rec)
{
    u8 *base = (u8 *) g_CurrentSetup.propDefs;
    s32 ofs;

    if (base == NULL)
    {
        return -1;
    }
    if ((void *) base != sDefsBase)
    {
        PropDefHeaderRecord *it = (PropDefHeaderRecord *) base;

        while (it->type != PROPDEF_END)
        {
            it = (PropDefHeaderRecord *) ((u32 *) it + sizepropdef(it));
        }
        sDefsBase = base;
        sDefsBytes = (s32) ((u8 *) it - base);
    }
    ofs = (s32) ((u8 *) rec - base);
    return (ofs >= 0 && ofs < sDefsBytes) ? ofs : -1;
}

/* ---- objects ---------------------------------------------------------------
 * The host owns object damage. The joiner forwards its own shots on setup
 * objects to the host and drops the rest (its explosions are replays of the
 * host's); the host applies everything and sends each result back, which the
 * joiner replays with the same numbers so destruction and its explosions
 * happen on both sides. */

static s32 sApplyingRemoteObj;

s32 coopObjDamage(ObjectRecord *obj, f32 damage, coord3d *pos, s32 item, s32 player)
{
    CoopEvent ev;
    s32 ofs;

    if (sEnabled <= 0 || sApplyingRemoteObj || !coopNetHasPeer() || !coopPartnerHere())
    {
        return 0;
    }
    ofs = coopSetupOfs(obj);
    if (ofs < 0)
    {
        return 0;
    }
    if (!coopNetIsHost())
    {
        if (item == ITEM_UNARMED || item == 0x1D) /* 0x1D: explosion */
        {
            return 1;
        }
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_OBJDMG;
    ev.u.obj.objofs = ofs;
    ev.u.obj.damage = damage;
    ev.u.obj.pos[0] = pos->f[0];
    ev.u.obj.pos[1] = pos->f[1];
    ev.u.obj.pos[2] = pos->f[2];
    ev.u.obj.item = (s16) item;
    ev.u.obj.player = (s16) player;
    coopQueueEventTo(&ev, coopNetIsHost() ? COOP_ALL : 0);   /* a joiner's shot goes to the host only; the verdict to all */
    return !coopNetIsHost(); /* the joiner waits for the host's verdict */
}

static void coopApplyObjDamage(CoopEvent *ev)
{
    ObjectRecord *obj;
    coord3d pos;

    if (g_CurrentSetup.propDefs == NULL || coopSetupOfs((u8 *) g_CurrentSetup.propDefs + ev->u.obj.objofs) < 0)
    {
        return;
    }
    obj = (ObjectRecord *) ((u8 *) g_CurrentSetup.propDefs + ev->u.obj.objofs);
    if (obj->prop == NULL)
    {
        return; /* already gone here */
    }
    pos.f[0] = ev->u.obj.pos[0];
    pos.f[1] = ev->u.obj.pos[1];
    pos.f[2] = ev->u.obj.pos[2];
    if (coopNetIsHost())
    {
        /* the joiner's shot: apply as ours; the hook sends the result back */
        objApplyDamage(obj, ev->u.obj.damage, &pos, ev->u.obj.item, ev->u.obj.player);
    }
    else
    {
        sApplyingRemoteObj = 1;
        objApplyDamage(obj, ev->u.obj.damage, &pos, ev->u.obj.item, ev->u.obj.player);
        sApplyingRemoteObj = 0;
    }
    portConLogf("ge: coop: peer obj %d damage %.2f (item %d)\n", ev->u.obj.objofs, ev->u.obj.damage, ev->u.obj.item);
}

/* Explosions hurt chrs on the host only; the joiner's copies of the host's
 * explosions would count twice, and neither side's blast may hurt the
 * puppet (each player's own game hurts its own Bond). */
s32 coopSkipExplosionDamage(ChrRecord *chr)
{
    if (sEnabled <= 0 || !coopNetHasPeer())
    {
        return 0;
    }
    if (chr->prop != NULL && coopIsPuppet(chr->prop))
    {
        return 1;
    }
    return coopIsMirroringClient();
}


/* ---- objectives and alarm ------------------------------------------------
 * Level scripts run on the host only, so its stage flags are the truth. The
 * criteria either player can meet (entering a room, a deposit, a photo,
 * copying the key) are OR'd together, so the team shares one objective list. */

static void coopSendWorld(void)
{
    CoopWorldPkt p;
    struct criteria_roomentered *r;
    struct criteria_deposit *d;
    struct criteria_picture *ph;
    s32 i;

    memset(&p, 0, sizeof(p));
    p.magic = COOP_MAGIC;
    p.version = COOP_VERSION;
    p.type = COOPPKT_WORLD;
    p.stageflags = (u32) objectiveregisters1;
    for (r = ptr_last_enter_room_subobject_entry_type20, i = 0; r != NULL && i < 32; r = r->next, i++)
    {
        if (r->status)
        {
            p.roombits |= 1u << i;
        }
    }
    for (d = ptr_last_deposit_in_room_subobject_entry_type21, i = 0; d != NULL && i < 32; d = d->next, i++)
    {
        if (d->flag)
        {
            p.depositbits |= 1u << i;
        }
    }
    for (ph = ptr_last_photo_obj_in_room_subobject_entry_type1E, i = 0; ph != NULL && i < 32; ph = ph->next, i++)
    {
        if (ph->flag)
        {
            p.photobits |= 1u << i;
        }
    }
    p.keycopied = g_CurrentPlayer->copiedgoldeneye != 0;
    p.alarm = alarmIsActive() != 0;
    p.calm = (u16) coopLocalCalm();
    coopNetSend(&p, sizeof(p));
}

static void coopApplyWorld(CoopWorldPkt *p)
{
    struct criteria_roomentered *r;
    struct criteria_deposit *d;
    struct criteria_picture *ph;
    s32 i;

    if (!coopNetIsHost())
    {
        if ((u32) objectiveregisters1 != p->stageflags)
        {
            portConLogf("ge: coop: stage flags %08x -> %08x\n", (u32) objectiveregisters1, p->stageflags);
            objectiveregisters1 = (s32) p->stageflags;
        }
        if (p->alarm && !alarmIsActive())
        {
            portConLogf("ge: coop: alarm on (host)\n");
            alarmActivate();
        }
        else if (!p->alarm && alarmIsActive())
        {
            portConLogf("ge: coop: alarm off (host)\n");
            alarmDeactivate();
        }
    }
    for (r = ptr_last_enter_room_subobject_entry_type20, i = 0; r != NULL && i < 32; r = r->next, i++)
    {
        if ((p->roombits & (1u << i)) && !r->status)
        {
            portConLogf("ge: coop: peer entered objective room %d\n", i);
            r->status = 1;
        }
    }
    for (d = ptr_last_deposit_in_room_subobject_entry_type21, i = 0; d != NULL && i < 32; d = d->next, i++)
    {
        if ((p->depositbits & (1u << i)) && !d->flag)
        {
            d->flag = 1;
        }
    }
    for (ph = ptr_last_photo_obj_in_room_subobject_entry_type1E, i = 0; ph != NULL && i < 32; ph = ph->next, i++)
    {
        if ((p->photobits & (1u << i)) && !ph->flag)
        {
            ph->flag = 1;
        }
    }
    if (p->keycopied && !g_CurrentPlayer->copiedgoldeneye)
    {
        g_CurrentPlayer->copiedgoldeneye = 1;
    }
}


/* ---- pickups ---------------------------------------------------------------
 * Whoever picks a setup item up first has it; it leaves the other world. Kept
 * items (keys, mission items) are only taken out of play there, so objective
 * checks can still find the record and count it as the team's. */


void coopPickedUp(ObjectRecord *obj, s32 kept)
{
    CoopEvent ev;
    s32 ofs;

    if (sEnabled <= 0 || !coopNetHasPeer() || !coopPartnerHere())
    {
        return;
    }
    ofs = coopSetupOfs(obj);
    if (ofs < 0)
    {
        return;
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_PICKUP;
    ev.u.pick.objofs = ofs;
    ev.u.pick.kept = kept;
    coopQueueEvent(&ev);
}

static void coopApplyPickup(CoopEvent *ev)
{
    ObjectRecord *obj;
    PropRecord *prop;

    if (coopSetupOfs((u8 *) g_CurrentSetup.propDefs + ev->u.pick.objofs) < 0)
    {
        return;
    }
    obj = (ObjectRecord *) ((u8 *) g_CurrentSetup.propDefs + ev->u.pick.objofs);
    prop = obj->prop;
    if (prop == NULL || bondinvHasPropInInv(prop))
    {
        return; /* gone already, or we grabbed it in the same instant */
    }
    if (ev->u.pick.kept)
    {
        chrpropDeregisterRooms(prop);
        chrpropDelist(prop);
        chrpropDisable(prop);
        if (sNumPeerHeld < COOP_MAX_PEERHELD)
        {
            sPeerHeld[sNumPeerHeld++] = ev->u.pick.objofs;
        }
    }
    else
    {
        objFreePermanently(obj, TRUE);
    }
    portConLogf("ge: coop: peer picked up obj %d (%s)\n", ev->u.pick.objofs, ev->u.pick.kept ? "kept" : "used");
}

s32 coopPeerHolds(ObjectRecord *obj)
{
    s32 ofs = coopSetupOfs(obj);
    s32 i;

    for (i = 0; ofs >= 0 && i < sNumPeerHeld; i++)
    {
        if (sPeerHeld[i] == ofs)
        {
            return 1;
        }
    }
    return 0;
}


/* ---- mission end -----------------------------------------------------------
 * The mission is completed once, for the team: whichever game reaches the
 * end (normally the host, whose level scripts run the exit) takes the other
 * along. Stage flags ride along so the joiner's objective check at the end
 * sees what the host saw. */

static s32 sApplyingRemoteEnd;

void coopMissionComplete(void)
{
    CoopEvent ev;

    coopCampaignMissionEnd(); /* a win together goes in coop_campaign.txt */
    if (sEnabled <= 0 || sApplyingRemoteEnd || !coopNetHasPeer() || !coopPartnerHere())
    {
        return;
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_MISSIONEND;
    ev.u.end.stage = bossGetStageNum();
    ev.u.end.stageflags = (u32) objectiveregisters1;
    coopQueueEvent(&ev);
    coopFlushEvents(); /* the stage may unload before the next tick */
    portConLogf("ge: coop: mission complete, telling the peer\n");
}

static void coopApplyMissionEnd(CoopEvent *ev)
{
    if (ev->u.end.stage != bossGetStageNum())
    {
        return;
    }
    if (!coopNetIsHost())
    {
        objectiveregisters1 = (s32) ev->u.end.stageflags;
    }
    portConLogf("ge: coop: peer completed the mission\n");
    sApplyingRemoteEnd = 1;
    bossReturnTitleStage();
    sApplyingRemoteEnd = 0;
}


/* ---- death and respawn -----------------------------------------------------
 * A dead player comes back beside the partner once the partner has been out
 * of combat for a few seconds, or after 20 s regardless. With no living
 * partner to come back to, the death is the normal mission failure. */

#define COOP_CALM_CAP        3600
#define COOP_RESPAWN_MIN     180   /* 3 s after the death fade */
#define COOP_RESPAWN_CALM    240   /* partner out of combat for 4 s */
#define COOP_RESPAWN_MAX     1200  /* 20 s */

static void coopTrackCombat(void)
{
    s32 i;

    if (g_CurrentPlayer->bonddead)
    {
        return;
    }
    if (g_CurrentPlayer->bondhealth < sLastHealth)
    {
        sLastHurtTick = sTick;
    }
    sLastHealth = g_CurrentPlayer->bondhealth;
    if (getCurrentPlayerWeaponId(GUNRIGHT) > ITEM_UNARMED)
    {
        sLastWeapon = getCurrentPlayerWeaponId(GUNRIGHT);
    }
    sDeadSince = 0;

    if (coopNetIsHost() && (sTick % 10) == 0)
    {
        for (i = 0; i < g_NumChrSlots; i++)
        {
            ChrRecord *chr = &g_ChrSlots[i];

            if (chr->model != NULL && chr->prop != NULL && !coopIsPuppet(chr->prop)
                && (chr->actiontype == ACT_ATTACK || chr->actiontype == ACT_ATTACKWALK || chr->actiontype == ACT_ATTACKROLL))
            {
                sLastFightTick = sTick;
                break;
            }
        }
    }
}

static s32 coopLocalCalm(void)
{
    u32 last = sLastHurtTick;
    u32 calm;

    if (coopNetIsHost() && (s32) (sLastFightTick - last) > 0)
    {
        last = sLastFightTick; /* guards only fight on the host */
    }
    calm = (last == 0) ? COOP_CALM_CAP : sTick - last;
    return (calm > COOP_CALM_CAP) ? COOP_CALM_CAP : (s32) calm;
}

static coord3d sDeathPos;
static StandTile *sDeathStan;
static f32 sDeathYaw;
static s32 sReviveHold;     /* ticks the partner has stood over us */
static s32 sBledOut;

#define COOP_REVIVE_DIST   140.0f  /* the partner within this of where we fell (xz) */
#define COOP_BLEED_OUT     3600    /* revive only: 60 s down and you are out of this mission */

/* A lobby setting (COOPSET_*): the host sets them on the lobby screen, the
 * joiner's game takes the host's from each hello. 0 is every setting's default. */
s32 coopSetting(s32 which)
{
    return (which >= 0 && which < COOPSET_MAX) ? sSettings[which] : 0;
}

void coopSetSetting(s32 which, s32 value)
{
    if (which >= 0 && which < COOPSET_MAX && (coopLobbyIsHost() || sEnabled <= 0))
    {
        sSettings[which] = (u8) value;
    }
}

s32 coopDownRule(void)
{
    return sDownRule;
}

void coopSetDownRule(s32 rule)
{
    if (rule >= COOPDOWN_BOTH && rule <= COOPDOWN_REVIVE && coopLobbyIsHost())
    {
        sDownRule = rule;
    }
}

/* The partner to come back beside: alive in this mission, and the calmest
 * (out of a fight longest). -1 when nobody is left standing. */
static s32 coopRespawnPartner(u32 *calmout)
{
    s32 p, best = -1;
    u32 bestcalm = 0;

    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        u32 calm;

        if (!coopRemoteAliveHere(p))
        {
            continue;
        }
        calm = sR[p].st.calm + (sTick - sR[p].calmTick);
        if (coopNetIsHost())
        {   /* guards are fighting someone: we see that first-hand */
            u32 fight = (sLastFightTick == 0) ? COOP_CALM_CAP : sTick - sLastFightTick;
            if (fight < calm)
            {
                calm = fight;
            }
        }
        if (best < 0 || calm > bestcalm)
        {
            best = p;
            bestcalm = calm;
        }
    }
    if (calmout != NULL)
    {
        *calmout = bestcalm;
    }
    return best;
}

s32 coopDeadTick(void)
{
    u32 dead, calm;
    s32 partner, p;

    coopStatsDown();
    partner = coopRespawnPartner(&calm);
    if (sEnabled <= 0 || !coopNetHasPeer() || partner < 0)
    {
        return 0; /* nobody left standing: the mission fails as usual */
    }
    if (sDeadSince == 0)
    {
        PropRecord *bond = getCurrentPlayerProp();

        sDeadSince = sTick;
        sReviveHold = 0;
        sBledOut = 0;
        sDeathStan = (bond != NULL) ? bond->stan : NULL;
        if (bond != NULL)
        {
            sDeathPos = bond->pos;
        }
        sDeathYaw = 6.2831855f - bondviewGetPlayerYawRadians();
        hudmsgBottomShow(sDownRule == COOPDOWN_TIMER ? "Waiting for your partners to get clear...\n"
                                                     : "Down! A partner can revive you.\n");
        portConLogf("ge: coop: dead, rule %d\n", sDownRule);
    }
    dead = sTick - sDeadSince;

    /* revive: a partner stands where we fell */
    if (sDownRule != COOPDOWN_TIMER && !sBledOut && sDeathStan != NULL)
    {
        s32 over = -1;

        for (p = 0; p < COOP_MAX_PLAYERS && over < 0; p++)
        {
            f32 dx = sR[p].st.pos[0] - sDeathPos.f[0];
            f32 dz = sR[p].st.pos[2] - sDeathPos.f[2];
            f32 dy = sR[p].st.pos[1] - sDeathPos.f[1];

            if (coopRemoteAliveHere(p) && dx * dx + dz * dz < COOP_REVIVE_DIST * COOP_REVIVE_DIST && dy < 250.0f && dy > -250.0f)
            {
                over = p;
            }
        }
        if (over >= 0)
        {
            if (++sReviveHold >= COOP_REVIVE_HOLD)
            {
                portConLogf("ge: coop: revived by player %d after %.1f s\n", over + 1, dead / 60.0f);
                sR[over].puppetGhost = 1;
                coopRespawnPlayerAt(&sDeathPos, sDeathStan, sDeathYaw, sLastWeapon);
                hudmsgBottomShow("Revived!\n");
                coopStatsNote(COOPSTAT_REVIVED);
                sLastHealth = g_CurrentPlayer->bondhealth;
                sDeadSince = 0;
                return 1;
            }
        }
        else
        {
            sReviveHold = 0;
        }
    }
    if (sDownRule == COOPDOWN_REVIVE)
    {
        if (!sBledOut && dead >= COOP_BLEED_OUT)
        {
            sBledOut = 1;
            hudmsgBottomShow("You bled out. You are out until the mission ends.\n");
            portConLogf("ge: coop: bled out\n");
        }
        return 1;   /* no timer: down until revived, or out */
    }

    if ((dead >= COOP_RESPAWN_MIN && calm >= COOP_RESPAWN_CALM) || dead >= COOP_RESPAWN_MAX)
    {
        CoopRemote *r = &sR[partner];

        portConLogf("ge: coop: respawning by player %d after %.1f s (calm %.1f s)\n",
                    partner + 1, dead / 60.0f, calm / 60.0f);
        r->puppetGhost = 1; /* we arrive inside the partner */
        coopRespawnPlayerAt(&r->puppet->pos, r->puppet->stan, 6.2831855f - r->st.yaw, sLastWeapon); /* view yaw runs the other way */
        coopStatsNote(COOPSTAT_RESPAWNS);
        sLastHealth = g_CurrentPlayer->bondhealth;
        sDeadSince = 0;
    }
    return 1;
}


/* Ticks until coopDeadTick brings us back: the minimum wait, the partner's
 * calm (if they stay calm from now), capped by the maximum. -1 = not waiting. */
static s32 coopRespawnEta(void)
{
    u32 dead, calm;
    s32 eta;

    if (sBledOut)
    {
        return -1;
    }
    if (coopRespawnPartner(&calm) < 0)
    {
        return -1;
    }
    if (sDeadSince == 0)
    {   /* still falling: count from now, if co-op will take this death */
        if (!g_CurrentPlayer->bonddead)
        {
            return -1;
        }
        dead = 0;
    }
    else
    {
        dead = sTick - sDeadSince;
    }
    if (sDownRule == COOPDOWN_REVIVE)
    {
        return ((s32) COOP_BLEED_OUT - (s32) dead > 0) ? (s32) COOP_BLEED_OUT - (s32) dead : 0;
    }
    eta = (s32) COOP_RESPAWN_MIN - (s32) dead;
    if ((s32) COOP_RESPAWN_CALM - (s32) calm > eta)
    {
        eta = (s32) COOP_RESPAWN_CALM - (s32) calm;
    }
    if ((s32) COOP_RESPAWN_MAX - (s32) dead < eta)
    {
        eta = (s32) COOP_RESPAWN_MAX - (s32) dead;
    }
    return (eta < 0) ? 0 : eta;
}

/* ---- HUD (coop_hud.c) ------------------------------------------------------- */

/* 1 when there is an i-th partner (from 0) in this mission; fills what the
 * HUD shows of them */
s32 coopHudPartnerAt(s32 i, CoopHudPartner *out)
{
    s32 p;
    CoopStatePkt *st;

    if (sEnabled <= 0 || !coopInMission())
    {
        return 0;
    }
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        if (coopRemoteHere(p) && i-- == 0)
        {
            break;
        }
    }
    if (p >= COOP_MAX_PLAYERS)
    {
        return 0;
    }
    st = &sR[p].st;
    out->pos.f[0] = st->pos[0];
    out->pos.f[1] = st->pos[1];
    out->pos.f[2] = st->pos[2];
    out->dead = st->dead;
    out->health = st->health;
    out->armour = st->armour;
    out->respawn = st->respawn;
    out->revivable = st->dead && st->downrule != COOPDOWN_TIMER && (st->downrule != COOPDOWN_REVIVE || st->respawn > 0);
    out->name = coopPlayerName(p);
    out->player = p;
    return 1;
}

s32 coopHudPartner(CoopHudPartner *out)
{
    return coopHudPartnerAt(0, out);
}

/* our own wait to respawn, -1 when not waiting */
s32 coopHudDownRule(void)
{
    return sDownRule;
}

s32 coopHudReviveHold(void)
{
    return (sDeadSince != 0) ? sReviveHold : 0;
}

s32 coopHudBledOut(void)
{
    return sDeadSince != 0 && sBledOut;
}

s32 coopHudRespawnEta(void)
{
    return (sEnabled > 0) ? coopRespawnEta() : -1;
}

/* ---- menus: hello and following the host ------------------------------------ */

static void coopSendHello(void)
{
    CoopHelloPkt p;

    s32 c, i;

    if ((sTick % (sOnLobbyScreen ? 15 : 60)) != 0 || !coopNetHasPeer())
    {
        return;
    }
    memset(&p, 0, sizeof(p));
    p.magic = COOP_MAGIC;
    p.version = COOP_VERSION;
    p.type = COOPPKT_HELLO;
    p.stage = bossGetStageNum();
    p.difficulty = selected_difficulty;
    p.mission = sMissionSerial;
    p.ready = (u8) sLobbyReady;
    p.session = coopSelfSession();
    p.inlobby = (u8) sOnLobbyScreen;
    p.downrule = (u8) sDownRule;
    memcpy(p.settings, sSettings, sizeof(p.settings));
    p.missionticks = coopInMission() ? sMissionTicks : 0;
    if (!coopNetIsHost())
    {
        coopNetSend(&p, sizeof(p));
        return;
    }
    for (i = 0; i < COOP_MAX_PLAYERS; i++)
    {   /* the roster: who is here, as the host sees it */
        CoopRosterEnt *e = &p.roster[i];

        e->present = (i == 0) || coopLobbyPlayerHere(i);
        if (!e->present)
        {
            continue;
        }
        e->id = coopPlayerId(i);
        snprintf(e->name, sizeof(e->name), "%s", coopPlayerName(i));
        e->ready = (i == 0) ? (u8) sLobbyReady : sChan[i - 1].hello.ready;
        e->inlobby = (i == 0) ? (u8) sOnLobbyScreen : sChan[i - 1].hello.inlobby;
    }
    for (c = 0; c < COOP_NET_MAX_PEERS; c++)
    {
        p.youare = (u8) coopPlayerOfChan(c);
        coopNetSendTo(c, &p, sizeof(p));
    }
}

/* Called every frontend tick (front.c). Returns 1 when it started a mission. */
s32 coopFrontTick(void)
{
    {   /* GE_COOP_DIFFICULTY=<mode>: once, at startup, the lobby's co-op difficulty (harness only; the joiner takes the host's from HELLO) */
        static s32 testdiff = -1;
        if (testdiff == -1)
        {
            const char *e = getenv("GE_COOP_DIFFICULTY");
            testdiff = 0;
            if (e != NULL)
            {
                sSettings[COOPSET_DIFFICULTY] = (u8) atoi(e);
                portConLogf("ge: coop: GE_COOP_DIFFICULTY=%d (%s)\n", atoi(e), coopDifficultyName(atoi(e)));
            }
        }
    }
    if (sEnabled < 0)
    {
        sEnabled = coopNetInit();
    }
    if (sEnabled <= 0)
    {
        return 0;
    }
    sTick++;
    coopPollNet();
    coopStatsFrontTick();   /* back from a mission: send our stats column */
    coopFlushEvents();
    coopSendHello();

    /* the joiner goes where the host goes, once per host mission */
    if (!coopNetIsHost() && sChan[0].helloSeen && sTick - sChan[0].helloTick < 180
        && sHostHello.stage != LEVELID_TITLE && sHostHello.stage > 0
        && sHostHello.mission != sFollowedMission)
    {
        s32 diff = sHostHello.difficulty;

        if (diff < DIFFICULTY_AGENT || diff >= DIFFICULTY_MAX)
        {
            diff = DIFFICULTY_AGENT;
        }
        sFollowedMission = sHostHello.mission;
        sLateJoin = sHostHello.missionticks > 600;   /* the host is well into it: start beside them */
        portConLogf("ge: coop: following the host into stage %d (difficulty %d)\n", sHostHello.stage, diff);
        if (selected_folder_num < FOLDER1 || selected_folder_num >= MAX_FOLDER_COUNT)
        {   /* a win saves to our own folder, as solo; folder 1 only if none is picked */
            selected_folder_num = FOLDER1;
        }
        set_selected_difficulty(diff);
        set_solo_and_ptr_briefing(sHostHello.stage);
        frontChangeMenu(MENU_RUN_STAGE, TRUE);
        return 1;
    }
    return 0;
}


/* ---- the lobby screen ---------------------------------------------------------
 * front.c draws it (menu MENU_COOP_LOBBY) and calls these. Steam is brought
 * up when the screen opens so the friend list can show; hosting creates a
 * lobby with no overlay (friends are invited from the list), joining takes a
 * friend's lobby or the first one found. A session started by GE_COOP or the
 * .bat files shows here too, it just has no Host / Join rows. */

static void coopSessionReset(void)
{
    s32 i;

    sSelfSession = 0;   /* a new one on first use */
    memset(sChan, 0, sizeof(sChan));
    for (i = 0; i < COOP_NET_MAX_PEERS; i++)
    {
        coopChanReset(&sChan[i]);
    }
    memset(&sHostHello, 0, sizeof(sHostHello));
    sFollowedMission = 0;
    sLobbyReady = 0;
    for (i = 0; i < COOP_MAX_PLAYERS; i++)
    {
        sR[i].valid = 0;
    }
}

static s32 coopLobbyStart(const char *spec)
{
    coopSessionReset();
    sEnabled = coopNetStart(spec);
    return sEnabled;
}

/* 1: co-op can be used here (Steam is up, or GE_COOP opened a session) */
s32 coopLobbyOpen(void)
{
    sOnLobbyScreen = 1;
    if (sEnabled < 0)
    {
        sEnabled = coopNetInit();
    }
    if (sEnabled > 0 && coopNetIsOpen())
    {
        return 1;
    }
    return coopLobbyStart("steam:idle");
}

void coopLobbyClose(void)
{
    sOnLobbyScreen = 0;
}

s32 coopLobbyIsSteam(void)
{
    return sEnabled > 0 && coopNetIsSteam();
}

s32 coopLobbyState(void)
{
    if (sEnabled <= 0 || !coopNetIsOpen())
    {
        return COOPLOBBY_OFF;
    }
    if (!coopNetIsSteam())
    {   /* a UDP session from GE_COOP: always "in" */
        return coopNetIsHost() ? COOPLOBBY_HOSTING : COOPLOBBY_JOINED;
    }
    switch (coopSteamLobbyState())
    {
        case 1:  return COOPLOBBY_BUSY;
        case 2:  return coopNetIsHost() ? COOPLOBBY_HOSTING : COOPLOBBY_JOINED;
        default: return COOPLOBBY_IDLE;
    }
}

s32 coopLobbyIsHost(void)
{
    return sEnabled > 0 && coopNetIsHost();
}

/* player p (not us) is in the session: its game is talking (hellos in the
 * last 3 s; a joiner learns about the others from the host's roster) */
s32 coopLobbyPlayerHere(s32 p)
{
    s32 c;

    if (sEnabled <= 0 || !coopNetHasPeer() || p < 0 || p >= COOP_MAX_PLAYERS)
    {
        return 0;
    }
    if (p == coopSelf())
    {
        return 1;
    }
    if (!coopNetIsHost() && !(sChan[0].helloSeen && sTick - sChan[0].helloTick < 180))
    {
        return 0;   /* no host, nobody */
    }
    c = coopChanOfPlayer(p);
    if (c >= 0)
    {
        return coopNetPeerConnected(c) && sChan[c].helloSeen && sTick - sChan[c].helloTick < 180;
    }
    return sHostHello.roster[p].present;
}

s32 coopLobbyPlayerReady(s32 p)
{
    if (!coopLobbyPlayerHere(p))
    {
        return 0;
    }
    if (p == coopSelf())
    {
        return sLobbyReady;
    }
    if (coopNetIsHost())
    {
        return sChan[p - 1].hello.ready;
    }
    return (p == 0) ? 1 : sHostHello.roster[p].ready;   /* the host starts it: always "ready" */
}

/* the other games are talking to us */
s32 coopLobbyHasPartner(void)
{
    s32 p;

    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        if (p != coopSelf() && coopLobbyPlayerHere(p))
        {
            return 1;
        }
    }
    return 0;
}

/* host: every partner is ready */
s32 coopLobbyPartnerReady(void)
{
    s32 p;

    if (!coopLobbyHasPartner())
    {
        return 0;
    }
    for (p = 1; p < COOP_MAX_PLAYERS; p++)
    {
        if (coopLobbyPlayerHere(p) && p != coopSelf() && !coopLobbyPlayerReady(p))
        {
            return 0;
        }
    }
    return 1;
}

s32 coopLobbyPartnerOnScreen(void)
{
    s32 p;

    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        if (p != coopSelf() && coopLobbyPlayerHere(p))
        {
            s32 c = coopChanOfPlayer(p);
            if ((c >= 0) ? sChan[c].hello.inlobby : sHostHello.roster[p].inlobby)
            {
                return 1;
            }
        }
    }
    return 0;
}

s32 coopLobbySelfReady(void)
{
    return sLobbyReady;
}

void coopLobbySetReady(s32 ready)
{
    sLobbyReady = ready != 0;
}

s32 coopLobbySelfPlayer(void)
{
    return coopSelf();
}

const char *coopLobbySelfName(void)
{
    return coopLobbyIsSteam() ? coopSteamSelfName() : "You";
}

/* a player's name: Steam's, or the host's roster for players we don't talk to */
const char *coopPlayerName(s32 p)
{
    static char fallback[COOP_MAX_PLAYERS][16];
    const char *n = NULL;
    s32 c;

    if (p < 0 || p >= COOP_MAX_PLAYERS)
    {
        return "";
    }
    if (p == coopSelf())
    {
        return coopLobbySelfName();
    }
    c = coopChanOfPlayer(p);
    if (c >= 0 && coopLobbyIsSteam())
    {
        n = coopNetPeerName(c);
    }
    else if (c < 0)
    {
        n = sHostHello.roster[p].name;
    }
    if (n != NULL && n[0] != '\0')
    {
        return n;
    }
    snprintf(fallback[p], sizeof(fallback[p]), "Player %d", p + 1);
    return fallback[p];
}

/* a player's SteamID (0 off Steam or not known) */
unsigned long long coopPlayerId(s32 p)
{
    s32 c;

    if (p < 0 || p >= COOP_MAX_PLAYERS || !coopLobbyIsSteam())
    {
        return 0;
    }
    if (p == coopSelf())
    {
        return coopSteamSelfId();
    }
    c = coopChanOfPlayer(p);
    return (c >= 0) ? coopNetPeerId(c) : sHostHello.roster[p].id;
}

/* the first partner's name (the lobby's two-player wording) */
const char *coopLobbyPartnerName(void)
{
    s32 p;

    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        if (p != coopSelf() && coopLobbyPlayerHere(p))
        {
            return coopPlayerName(p);
        }
    }
    return "Partner";
}

void coopLobbyHost(void)
{
    const char *e = getenv("GE_COOP");

    /* a UDP GE_COOP is a test setup: the lobby's Host uses it */
    coopLobbyStart((e != NULL && strncmp(e, "host:", 5) == 0) ? e : "steam:host");
}

void coopLobbyJoinAny(void)
{
    const char *e = getenv("GE_COOP");

    coopLobbyStart((e != NULL && strncmp(e, "join:", 5) == 0) ? e : "steam:join");
}

void coopLobbyJoinFriend(s32 i)
{
    char spec[48];
    unsigned long long lobby = coopSteamFriendLobby(i);

    if (lobby == 0)
    {
        return;
    }
    sprintf(spec, "steam:join:%llu", lobby);
    coopLobbyStart(spec);
}

/* back to Host / Join; Steam stays up for the friend list */
void coopLobbyLeave(void)
{
    coopLobbyStart(coopLobbyIsSteam() ? "steam:idle" : NULL);
}

s32 coopLobbyFriends(void)
{
    return coopLobbyIsSteam() ? coopSteamFriendsRefresh() : 0;
}

const char *coopLobbyFriendName(s32 i)
{
    return coopSteamFriendName(i);
}

s32 coopLobbyFriendHosting(s32 i)
{
    return coopSteamFriendHosting(i);
}

s32 coopLobbyFriendIsPartner(s32 i)
{
    return coopSteamFriendIsPeer(i);
}

void coopLobbyInvite(s32 i)
{
    coopSteamFriendInvite(i);
}

void coopLobbyInviteOverlay(void)
{
    coopSteamInviteOverlay();
}

/* a co-op game is going: after a mission the menus come back to the lobby */
s32 coopSessionActive(void)
{
    return sEnabled > 0 && coopNetIsOpen() && coopNetHasPeer();
}


/* ---- level scripts: either player ------------------------------------------
 * Level scripts run on the host and ask about "Bond". Position checks (near a
 * pad, in a room, below a height) are asked a second time with the partner as
 * Bond, so either player can trigger an exit or an event. */

/* k from 0: the k-th living partner as Bond; 0 when there are no more */
s32 coopAltBondBegin(s32 k)
{
    s32 p;

    if (g_CoopPropOverride != NULL || !coopNetIsHost())
    {
        return 0;
    }
    for (p = 1; p < COOP_MAX_PLAYERS; p++)
    {
        if (coopRemoteAliveHere(p) && k-- == 0)
        {
            g_CoopPropOverride = sR[p].puppet;
            return 1;
        }
    }
    return 0;
}

void coopAltBondEnd(void)
{
    g_CoopPropOverride = NULL;
}


/* ---- the joiner's explosives ------------------------------------------------
 * A grenade, mine or rocket the joiner fires exists only in its own game. Its
 * blast there shows the effect and hurts the joiner; the same blast is set
 * off in the host's game, which owns the damage to guards and objects. */

static s32 sApplyingRemoteBoom;

void coopExplosionCreated(PropRecord *source, coord3d *pos, s32 type, u8 *rooms)
{
    CoopEvent ev;
    s32 i;

    if (source != NULL || type == EXPLOSION_DEF_PLAYER || sApplyingRemoteBoom || !coopIsMirroringClient())
    {
        return; /* destroyed objects replay from the host's damage events */
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_EXPLODE;
    ev.u.boom.pos[0] = pos->f[0];
    ev.u.boom.pos[1] = pos->f[1];
    ev.u.boom.pos[2] = pos->f[2];
    ev.u.boom.type = (s16) type;
    memset(ev.u.boom.rooms, 0xFF, sizeof(ev.u.boom.rooms));
    for (i = 0; rooms != NULL && i < 7 && rooms[i] != 0xFF; i++)
    {
        ev.u.boom.rooms[i] = rooms[i];
    }
    coopQueueEventTo(&ev, 0);
    portConLogf("ge: coop: our explosion (type %d), sent to host\n", type);
}

/* coop_stats.c: the explosion being made is the partner's */
s32 coopApplyingPartnerExplosion(void)
{
    return sApplyingRemoteBoom;
}

static void coopApplyExplosion(CoopEvent *ev)
{
    coord3d pos;
    StandTile *stan;

    if (!coopNetIsHost() || !coopInMission())
    {
        return;
    }
    pos.f[0] = ev->u.boom.pos[0];
    pos.f[1] = ev->u.boom.pos[1];
    pos.f[2] = ev->u.boom.pos[2];
    stan = stanFindTileBelowPos(&pos, NULL, NULL);
    if (stan == NULL || ev->u.boom.rooms[0] == 0xFF)
    {
        return;
    }
    sApplyingRemoteBoom = ev->from;   /* whose (coop_stats.c) */
    explosionCreate(NULL, &pos, stan, ev->u.boom.type, 0, 0, ev->u.boom.rooms, 0);
    sApplyingRemoteBoom = 0;
    portConLogf("ge: coop: peer explosion (type %d) at %.0f %.0f %.0f\n", ev->u.boom.type, pos.f[0], pos.f[1], pos.f[2]);
}


/* ---- cutscenes and script presentation --------------------------------------
 * Level scripts run only on the host, but some of their commands are about
 * what the player sees: cutscene cameras, fades, locking the controls, on-
 * screen text, music. The host sends each of those commands' bytes and the
 * joiner runs that one command through its own interpreter. */

static s32 coopIsPresentationCmd(u8 cmd)
{
    switch (cmd)
    {
        case AI_CameraReturnToBond:
        case AI_CameraLookAtBondFromPad:
        case AI_CameraSwitch:
        case AI_CameraOrbitPad:
        case AI_BondDisableControl:
        case AI_BondEnableControl:
        case AI_ScreenFadeToBlack:
        case AI_ScreenFadeFromBlack:
        case AI_BondEquipItemCinema:
        case AI_BondHideWeapons:
        case AI_BondSetLockedVelocity:
        case AI_TextPrintBottom:
        case AI_TextPrintTop:
        case AI_MusicPlaySlot:
        case AI_MusicStopSlot:
            return 1;
        default:
            return 0;
    }
}

static s32 sApplyingRemoteAi;

void coopAiCommand(u8 *list, s32 offset, ChrRecord *self)
{
    CoopEvent ev;
    s32 len;
    u8 cmd = list[offset];

    if (!coopIsPresentationCmd(cmd) || sApplyingRemoteAi || sEnabled <= 0 || !coopNetIsHost()
        || !coopNetHasPeer() || !coopPartnerHere())
    {
        return;
    }
    len = chraiitemsize(list, offset);
    if (len <= 0 || len > (s32) sizeof(ev.u.ai.cmd) - 1)
    {
        portConLogf("ge: coop: script command %d is %d bytes, too long to send\n", cmd, len);
        return;
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_AICMD;
    ev.u.ai.chrnum = (self != NULL) ? self->chrnum : -1;
    ev.u.ai.len = (u8) len;
    memcpy(ev.u.ai.cmd, list + offset, len);
    coopQueueEvent(&ev);
}

static void coopApplyAiCommand(CoopEvent *ev)
{
    static u8 list[32];
    ChrRecord *chr;
    AIRecord *savedlist;
    s32 savedofs;
    s32 i;

    if (coopNetIsHost() || bossGetStageNum() == LEVELID_TITLE)
    {
        return;
    }
    /* the command runs as the same chr where there is one, else any chr:
     * these commands are about the player, not the chr running them */
    chr = (ev->u.ai.chrnum >= 0) ? chrFindByLiteralId(ev->u.ai.chrnum) : NULL;
    for (i = 0; chr == NULL && i < g_NumChrSlots; i++)
    {
        if (g_ChrSlots[i].model != NULL && g_ChrSlots[i].prop != NULL)
        {
            chr = &g_ChrSlots[i];
        }
    }
    if (chr == NULL)
    {
        return;
    }
    memcpy(list, ev->u.ai.cmd, ev->u.ai.len);
    list[ev->u.ai.len] = AI_EndList;
    savedlist = chr->ailist;
    savedofs = chr->aioffset;
    chr->ailist = (AIRecord *) list;
    chr->aioffset = 0;
    sApplyingRemoteAi = 1;
    ai((PropDefHeaderRecord *) chr, PROP_TYPE_CHR);
    sApplyingRemoteAi = 0;
    chr->ailist = savedlist;
    chr->aioffset = savedofs;
    portConLogf("ge: coop: host script command %d\n", list[0]);
}
