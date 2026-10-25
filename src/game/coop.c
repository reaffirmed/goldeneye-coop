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
#include <stdio.h>
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
f32 gunItemGetDestructionAmount(ITEM_IDS item);
void hudmsgBottomShow(char *mess);
TICKOP propPickupByPlayer(PropRecord *prop, bool showstring);
void objFreePermanently(ObjectRecord *obj, bool freeprop);
void alarmDeactivate(void);
bool alarmIsActive(void);
s32 objGetDestroyedLevel(ObjectRecord *obj);
void objApplyDamage(ObjectRecord *obj, f32 damage, coord3d *pos, ITEM_IDS itemnum, s32 playernum);
void objBounce(ObjectRecord *obj, coord3d *arg1);
void objDropRecursively(PropRecord *prop);
void propExecuteTickOperation(PropRecord *prop, TICKOP op);
void chrSetFiring(ChrRecord *self, s32 hand, s32 firing);
void sub_GAME_7F02BFE4(ChrRecord *self, s32 arg1, s32 arg2);
PropRecord *chrSpawnAtCoord(s32 bodynum, s32 headnum, coord3d *pos, StandTile *stan, f32 angle, AIListRecord *ailist, s32 spawnflags);
s32 chraiGetAIListID(AIRecord *AIList, bool *isGlobalAIList);
void chrStopFiring(ChrRecord *self);
void set_missionstate(MISSION_STATE_ID state);

extern void portConLogf(const char *fmt, ...);

#define COOP_MAGIC   0x50434547u /* "GECP" */
#define COOP_VERSION 9 /* 2: lobby fields in HELLO, health in STATE; 3: session ids; 4: up to four players; 5: guns, shots and characters;
                          6: host migration (AI sync, bye, addresses in the roster);
                          7: pings, chat, objective credit, team times, cross-save (COOPEV 16, 20..31, COOPPKT 10);
                          8: versus (COOPEV 32..36, COOPPKT 16);
                          9: shared pickups, thrown items, drops, knocks, Use, host blasts (COOPEV 17, 18, 25..28) */

enum { COOPPKT_STATE = 1, COOPPKT_GUARDS = 3, COOPPKT_EVENTS = 5, COOPPKT_WORLD = 6, COOPPKT_HELLO = 7, COOPPKT_AISYNC = 8, COOPPKT_BYE = 9 };
/* 10 and 11 (COOPPKT_EXT_FIRST..LAST in coop.h) are coop_ext.c's features' own packets */

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
    CoopLook look;  /* guns, movement, shots and character (coop_look.c) */
} CoopStatePkt;

#define COOP_ALL 0xFF   /* CoopEvent.to: everyone */

/* the host's roster in each hello: who is in the session */
typedef struct
{
    unsigned long long id;  /* SteamID, or the UDP address (coopNetPeerId); 0 when not known */
    char name[24];
    u8 present;
    u8 ready;
    u8 inlobby;
    u8 character;   /* the lobby's character pick (coopCharacter) */
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
    u8 character;       /* the sender's character pick (coopCharacter) */
    u32 session;        /* as in CoopEventsPkt */
    u8 settings[COOPSET_MAX];   /* host: the lobby's other settings (coopSetting) */
    u32 missionticks;   /* how long the sender has been in this mission */
    u8 youare;          /* host: the player number of the game it is sent to */
    u8 hold;            /* host: bit 0, the mission card holds everyone still; bits 4-7, who has loaded in (coop_screens.c) */
    u8 pad2[2];
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
       COOPEV_STATS = 9, COOPEV_GIVE = 10, COOPEV_GIVEBACK = 11,
       COOPEV_PLAYERNOTE = 12, COOPEV_ENDALL = 13 /* 14, 15: kept for host migration */,
       COOPEV_PING = 16, COOPEV_OBJKNOCK = 17, COOPEV_OBJUSE = 18 };   /* 32+ versus */
/* 20..31 (COOPEV_EXT_FIRST..LAST in coop.h) belong to coop_ext.c's features:
 * chat, objective credit, team times, cross-save */

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
        struct { s32 objofs; u8 bounce; u8 children; u8 pad[2]; f32 dir[3]; s32 stage; } knock;  /* to all */
        struct { s32 objofs; s32 kept; s32 stage; } pick;                       /* to all */
        struct { s32 objofs; s32 stage; } use;                                  /* joiner to host */
        struct { s32 stage; u32 stageflags; } end;                              /* both ways */
        struct { f32 pos[3]; s16 type; u8 rooms[8]; } boom;                     /* client -> host */
        struct { s16 chrnum; u8 len; u8 cmd[25]; } ai;                          /* host -> client */
        /* COOPEV_STATS: a CoopStatsMsg (coop_stats.c) in raw, both ways */
        CoopGive give;                                                          /* both ways (coop_give.c) */
        CoopPing ping;                                                          /* to all (coop_ping.c) */
        struct { u8 player; u8 what; u8 pad[2]; char name[24]; } note;          /* host -> all: COOPNOTE_* about a player */
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

/* host -> joiners once a second: what each chr's AI is doing and how hurt it
 * is, so a joiner that takes over as host (coopMigrate*) carries on from
 * there instead of starting every script again */
typedef struct
{
    s16 chrnum;
    u16 ailist;     /* chraiGetAIListID, 0xFFFF none */
    u16 aioffset;
    s16 aireturnlist;
    u8 actiontype;
    u8 morale;
    u8 alertness;
    u8 flags2;
    u8 random;
    u8 pad[3];
    u32 chrflags;
    u16 hidden;
    s16 padpreset1;
    s16 chrpreset1;
    s16 pad2;
    s32 timer60;
    f32 damage;
    f32 maxdamage;
} CoopAiEnt;

#define COOP_AI_PER_PKT 32

typedef struct
{
    u32 magic;
    u16 version;
    u16 type;
    u16 first;
    u16 count;
    CoopAiEnt ent[COOP_AI_PER_PKT];
} CoopAiPkt;

/* "I am leaving": sent a few times, unreliable, by a game that leaves the
 * session on purpose (the menus, the watch, closing the game) */
enum { COOPBYE_LEAVE = 1, COOPBYE_HANDOFF = 2 };
typedef struct
{
    u32 magic;
    u16 version;
    u16 type;
    u32 session;
    u8 reason;      /* COOPBYE_* */
    u8 player;      /* the sender's player number */
    u8 newhost;     /* HANDOFF: the player that takes over */
    u8 pad;
} CoopByePkt;

/* COOPEV_PLAYERNOTE: what happened to a joiner, from the host to everyone */
enum { COOPNOTE_LOST = 1, COOPNOTE_BACK, COOPNOTE_GONE, COOPNOTE_LEFT, COOPNOTE_LEFTMISSION };


static s32 sEnabled = -1;
#define COOP_MAX_PEERHELD 32
static s32 sPeerHeld[COOP_MAX_PEERHELD];
static s32 sNumPeerHeld;
/* this mission's shared pickups (see the pickups section) */
#define COOP_MAX_TEAMPICKS 96
static s32 sTeamPick[COOP_MAX_TEAMPICKS];       /* setup offsets */
static u8 sTeamPickDone[COOP_MAX_TEAMPICKS];    /* we have it, or it is gone here */
static u8 sTeamPickFrom[COOP_MAX_TEAMPICKS];
static s32 sNumTeamPicks;
static s32 sApplyingRemotePick;
static void coopTeamPicksTick(void);
static void coopResyncPicks(s32 player);
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
static s32 sWorldFrom;  /* the player whose WORLD coopApplyWorld is applying */

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
    CoopLookPuppet look;    /* the puppet's guns, animation and shots played (coop_look.c) */
    s32 lost;           /* its connection dropped: the Bond stands idle, guards and scripts leave it be */
    u32 lostMs;         /* when */
    u32 seenMs;         /* the newest STATE, wall clock */
} CoopRemote;
/* each level script's last "either player" check passer (coopAltBondCredit) */
#define COOP_SCRIPTPASS 32
static struct
{
    void *script;
    s8 player;
} sScriptPass[COOP_SCRIPTPASS];
static s32 sScriptPassNext;

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
    u32 heardMs;        /* anything from it, wall clock (coopNowMs) */
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


static s32 coopInMission(void);
static void coopApplyGuard(CoopGuardEnt *e);
static void coopApplyEvent(CoopEvent *ev);
static void coopQueueEvent(CoopEvent *ev);
static void coopFlushEvents(void);
static void coopApplyDoor(s32 doorofs, s32 state);
static void coopApplyObjDamage(CoopEvent *ev);
static void coopApplyObjKnock(CoopEvent *ev);
static void coopApplyObjUse(CoopEvent *ev);
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
static s32 coopNetInitFirst(void);
static u32 coopNowMs(void);
static void coopSessionTick(void);
static void coopRecvBye(s32 c, CoopByePkt *b);
static void coopRecvAiSync(CoopAiPkt *p);
static void coopSendAiSync(void);
static void coopApplyNote(CoopEvent *ev);
static void coopApplyEndAll(void);
static void coopKeepAlive(void);
static void coopHoldStart(void);
static void coopHoldTick(void);
/* the mission card (coopHoldStart) */
static s32 sHold;           /* holding the world still */
static u32 sHoldMs;         /* since when (coopNowMs) */
static u32 sHoldTick;       /* sTick at the start: only hellos after it count */
static u8 sHoldExpect;      /* host: the players (bits) it waits for */
static u8 sHoldArrived;     /* who has loaded in, the host's bit included */
static void coopPendingEndTick(void);
static void coopRemoteWatch(s32 p);
static void coopMigrateReset(void);
static s32 sHostWaiting;    /* joiner: the host went quiet or left; see coopSessionTick */
static s32 sMigrated;        /* this session has been through a migration: stay in the lobby after */
static s32 sPendingEnd;      /* end our mission at the next tick: 1 back to the lobby */
static void coopEndMissionLocal(void);
static CoopAiEnt sAiSync[COOP_MAX_CHRS];    /* joiner: the host's AI state per chr slot (COOPPKT_AISYNC) */
static u8 sAiSyncHave[COOP_MAX_CHRS];
static void coopSayBye(void);
static s32 coopLobbyStart(const char *spec);

static s32 coopLocalBody(void)
{
    s32 head;
    s32 body = coopCharacterBody(coopCharacter(), &head);

    if (body >= 0)
    {
        return body;    /* the lobby's pick */
    }
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

    /* the multiplayer game never sets the camera mode (versus) */
    return bond != NULL && bond->stan != NULL && (g_CameraMode == CAMERAMODE_FP || coopVersus());
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
    return coopRemoteHere(p) && !sR[p].st.dead && !sR[p].lost && sR[p].puppet != NULL && sR[p].puppet->chr != NULL
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
    coopCharacterBody(coopCharacter(), &p.headnum);
    coopLookFill(&p.look);
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
    r->seenMs = coopNowMs();
    r->calmTick = sTick;
}

/* a VERSUS packet (coop_versus.c): the host passes a joiner's on, like a STATE */
static void coopRecvVersus(s32 c, u8 *buf, s32 n)
{
    s32 q;
    u8 *player = buf + 8;   /* CoopVsPkt.player, after the magic, version and type */

    if (coopNetIsHost())
    {
        *player = (u8) coopPlayerOfChan(c);
        for (q = 0; q < COOP_NET_MAX_PEERS; q++)
        {
            if (q != c)
            {
                coopNetSendTo(q, buf, n);
            }
        }
    }
    if (*player < COOP_MAX_PLAYERS && *player != coopSelf())
    {
        coopVersusRecvPkt(buf, *player);
    }
}

/* a game on another build: its packets are dropped, so say why nothing
 * happens (once per peer and version) instead of "Connecting..." forever */
static void coopVersionMismatch(s32 c, s32 theirs)
{
    static u16 told[COOP_NET_MAX_PEERS];
    char text[96];

    if (told[c] == (u16) theirs)
    {
        return;
    }
    told[c] = (u16) theirs;
    portConLogf("ge: coop: player on channel %d runs co-op version %d, we run %d\n", c, theirs, COOP_VERSION);
    if (coopNetIsHost())
    {
        snprintf(text, sizeof(text), "A player tried to join with\nanother version of the game\n(theirs %d, yours %d).", theirs, COOP_VERSION);
    }
    else
    {
        snprintf(text, sizeof(text), "The host runs another version\n(theirs %d, yours %d). You both\nneed the same co-op build.", theirs, COOP_VERSION);
    }
    coopScreenError("Different Versions", text);
}

static void coopPollNet(void)
{
    u8 buf[1500];
    s32 n;
    s32 c;

    while ((n = coopNetRecvFrom(buf, sizeof(buf), &c)) > 0)
    {
        CoopStatePkt *p = (CoopStatePkt *) buf;

        if (n >= (s32) sizeof(u32) * 2 && p->magic == COOP_MAGIC && p->version != COOP_VERSION
            && c >= 0 && c < COOP_NET_MAX_PEERS)
        {
            coopVersionMismatch(c, p->version);
        }
        if (n < (s32) sizeof(u32) * 2 || p->magic != COOP_MAGIC || p->version != COOP_VERSION
            || c < 0 || c >= COOP_NET_MAX_PEERS)
        {
            continue;
        }
        sChan[c].heardMs = coopNowMs();
        if (p->type == COOPPKT_BYE && n >= (s32) sizeof(CoopByePkt))
        {
            coopRecvBye(c, (CoopByePkt *) buf);
            continue;
        }
        if (p->type == COOPPKT_AISYNC && !coopNetIsHost())
        {
            CoopAiPkt *a = (CoopAiPkt *) buf;
            s32 hdr = (s32) ((u8 *) &a->ent[0] - (u8 *) a);

            if (n >= hdr && a->count <= COOP_AI_PER_PKT && n >= hdr + a->count * (s32) sizeof(CoopAiEnt))
            {
                coopRecvAiSync(a);
            }
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
                sWorldFrom = coopPlayerOfChan(c);
                coopApplyWorld((CoopWorldPkt *) buf);
            }
            continue;
        }
        if (p->type == COOPPKT_STATE && n >= (s32) sizeof(CoopStatePkt))
        {
            coopRecvState(c, p);
        }
        if (p->type == COOPPKT_VERSUS && n >= coopVersusPktSize())
        {
            coopRecvVersus(c, buf, n);
        }
        if (p->type >= COOPPKT_EXT_FIRST && p->type <= COOPPKT_EXT_LAST)
        {
            coopExtPacket(buf, n, coopPlayerOfChan(c));
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
    coopLookReset(&r->look);
    coopLookPuppetTick(&r->look, prop->chr, &r->st.look, r->st.dead, COOP_PUPPET_CHRNUM + coopSelf());
    portConLogf("ge: coop: puppet for player %d spawned at %.0f %.0f %.0f\n", r->st.player + 1, pos.f[0], pos.f[1], pos.f[2]);
    coopFeedToast("A partner is here", r->st.player);
}

static void coopPuppetUpdate(CoopRemote *r)
{
    ChrRecord *chr = r->puppet->chr;
    coord3d pos;
    StandTile *stan;

    pos.f[0] = r->st.pos[0];
    pos.f[1] = r->st.pos[1];
    pos.f[2] = r->st.pos[2];

    stan = coopFindStan(&pos, r->puppet->stan, &r->puppet->pos);
    if (stan == NULL)
    {
        return;
    }
    r->puppetPrev = pos;

    r->puppet->stan = stan;
    r->puppet->pos = pos;
    setsuboffset(chr->model, &pos);
    setsubroty(chr->model, r->st.yaw);
    sub_GAME_7F01FC10(chr->model, &r->puppet->pos, &r->puppet->pos, &chr->ground);
    chrDetectRooms(chr);

    /* the guns in their hands, the MP body animations, their shots (coop_look.c) */
    coopLookPuppetTick(&r->look, chr, &r->st.look, r->st.dead, COOP_PUPPET_CHRNUM + coopSelf());
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
    /* our camera inside their body (both start on one pad, or we watch
     * them while we are down): don't draw it */
    if (d2 < 45.0f * 45.0f || coopSpectateTarget() == r->st.player)
    {
        r->puppet->chr->chrflags |= CHRFLAG_HIDDEN;
    }
    else
    {
        r->puppet->chr->chrflags &= ~CHRFLAG_HIDDEN;
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
    memset(sAiSyncHave, 0, sizeof(sAiSyncHave));
    sMissionTicks = 0;
    sNumPeerHeld = 0;
    memset(sScriptPass, 0, sizeof(sScriptPass));    /* a new mission's scripts */
    sNumTeamPicks = 0;
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
        sR[i].lost = 0;
    }
    coopDifficultyStageStart();
    coopVersusStageStart();
    coopHoldStart();        /* after coopVersusStageStart: versus never holds */
    coopPingStageStart();
    coopAudioStageStart();
    coopItemsStageStart();
}

static void coopTickBody(void);

void coopTick(void)
{
    coopPresenceGameTick(1);    /* Steam / Discord status, invites (coop_presence.c) */
    if (coopVersus())
    {   /* in a versus match the multiplayer loop leaves the last player current: we are player 0 */
        const s32 cur = get_cur_playernum();

        set_cur_player(0);
        coopTickBody();
        set_cur_player(cur);
        return;
    }
    coopTickBody();
}

static void coopTickBody(void)
{
    if (sEnabled < 0)
    {
        sEnabled = coopNetInitFirst();
    }
    if (!sEnabled)
    {
        return;
    }
    if (g_ClockTimer <= 0)
    {
        if (sHold)
        {
            coopHoldTick(); /* the mission card: everyone loads in first */
            return;
        }
        coopKeepAlive();    /* paused, or a cutscene has the controls: still here */
        return;
    }

    sTick++;
    coopPollNet();
    coopSessionTick();
    if (bossGetStageNum() != LEVELID_TITLE)
    {
        coopExtTick(coopInMission());   /* chat, spectating, objective credit, ... (coop_ext.c); the menus' is coopFrontTick's */
    }
    coopFlushEvents();
    coopSendHello();
    coopPendingEndTick();

    if (!coopInMission())
    {
        return;
    }

    sMissionTicks++;
    if (coopVersus())
    {   /* the multiplayer game's own rules; the others' bodies, not puppets (coop_versus.c) */
        coopSendState();
        coopVersusTick();
        return;
    }
    coopStatsMissionTick(coopPartnerHere());
    coopAudioTick();        /* the downed and revived stings (coop_audio.c) */
    coopSendState();
    coopTrackCombat();
    coopPingTick();
    if ((sTick % 15) == 0)
    {
        coopTeamPicksTick();    /* shared pickups that had to wait */
    }
    coopItemsTick((s32) sMissionTicks);   /* harness: GE_COOP_TESTAIM (coop_items.c) */
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
    if (coopNetIsHost() && coopNetHasPeer() && coopPartnerHere() && (sTick % 60) == 30)
    {
        coopSendAiSync();   /* what a joiner needs to take over (coopMigrate) */
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
        coopRemoteWatch(p);     /* lost, back or gone (host migration section) */
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
            if (r->puppet != NULL)
            {
                coopResyncPicks(p);     /* what the team has picked up so far */
            }
        }
        else if (r->puppet->chr == NULL)
        {
            r->puppet = NULL; /* freed under us (stage cleanup); respawn */
        }
        else if (r->puppet->chr->bodynum != r->st.bodynum || r->puppet->chr->headnum != r->st.headnum)
        {
            coopPuppetRemove(r);    /* they picked another character: built again next tick */
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
            coopFeedToast("A partner rejoined", p);
        }
    }
    if (sLateJoin && coopRemoteAliveHere(0) && sMissionTicks > 30 && !g_CurrentPlayer->bonddead)
    {   /* joined a mission under way: go to the host, not the level's start */
        sLateJoin = 0;
        sR[0].puppetGhost = 1;
        coopRespawnPlayerAt(&sR[0].puppet->pos, sR[0].puppet->stan, 6.2831855f - sR[0].st.yaw, sLastWeapon);
        coopFeedToast("Joined your partners", -1);
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
    {   /* GE_COOP_TESTFF=<mission ticks>: from then, shoot the first partner in the chest every 2 s, as the gun would (harness only) */
        static s32 testff = -2;
        if (testff == -2)
        {
            const char *e = getenv("GE_COOP_TESTFF");
            testff = (e != NULL) ? atoi(e) : -1;
        }
        if (testff >= 0 && sMissionTicks >= testff && (sMissionTicks % 120) == 0)
        {
            s32 p;
            for (p = 0; p < COOP_MAX_PLAYERS; p++)
            {
                if (p != coopSelf() && coopRemoteHere(p) && sR[p].puppet != NULL && sR[p].puppet->chr != NULL)
                {
                    coord3d v = { 0.0f, 0.0f, 1.0f };
                    portConLogf("ge: coop: testff: shooting player %d\n", p + 1);
                    handles_shot_actors(sR[p].puppet->chr, HIT_CHEST, &v, ITEM_AK47, TRUE);
                    break;
                }
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
    if (coopPartnerHere() && coopInMission())
    {   /* GE_COOP_TESTPING=<s>[:<pitch>]: <s> seconds into the mission press ping,
         * looking <pitch> degrees up (default -20, down) for the second before;
         * GE_COOP_TESTPINGCHR=<chrnum> (-1 the nearest) aims at that guard instead,
         * GE_COOP_TESTPINGSTEP=<units> from it (coop_ping.c; harness only) */
        static s32 testping = -2;
        static s32 testpitch;
        if (testping == -2)
        {
            const char *e = getenv("GE_COOP_TESTPING");
            testping = (e != NULL) ? atoi(e) * 60 : -1;
            testpitch = (e != NULL && strchr(e, ':') != NULL) ? atoi(strchr(e, ':') + 1) : -20;
        }
        if (testping > 60 && (s32) sMissionTicks >= testping - 60 && (s32) sMissionTicks < testping)
        {
            coopPingTestPress(testpitch);   /* hold the aim until the view has caught up */
        }
        if (testping > 0 && (s32) sMissionTicks == testping)
        {
            portConLogf("ge: coop: testping: pressing ping\n");
            coopPingPress();
        }
    }
    if (coopPartnerHere() && coopInMission())
    {
        coopLookTestTick((s32) sMissionTicks);   /* GE_COOP_TESTFIRE, GE_COOP_TESTLOOK (coop_look.c) */
    }
    if (coopHostAbortAsks())
    {   /* GE_COOP_TESTABORT=<s>:handover|endall: the host aborts from the watch that far into the mission, and answers (harness only) */
        static s32 testabort = -2;
        static s32 handover;
        if (testabort == -2)
        {
            const char *e = getenv("GE_COOP_TESTABORT");
            testabort = (e != NULL) ? atoi(e) * 60 : -1;
            handover = (e != NULL && strstr(e, "end") == NULL);
        }
        if (testabort > 0 && (s32) sMissionTicks >= testabort)
        {
            testabort = -1;
            portConLogf("ge: coop: testabort (%s)\n", handover ? "hand over" : "end all");
            coopHostAbort(handover);
            coopEndMissionLocal();  /* the watch's own abort */
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
    {   /* GE_COOP_TESTOBJ=1: every 5 s, a heavy "shot" on the nearest breakable setup object;
         * GE_COOP_TESTOBJ=<model> (2 or more) only on objects of that model, at any distance (harness only) */
        static s32 testobj = -1;
        if (testobj == -1)
        {
            testobj = getenv("GE_COOP_TESTOBJ") != NULL ? atoi(getenv("GE_COOP_TESTOBJ")) : 0;
        }
        if (testobj && (sTick % 300) == 150 && coopInMission())
        {
            PropRecord *bond = getCurrentPlayerProp();
            PropRecord *it, *best = NULL;
            f32 bestd = 1e30f;

            for (it = g_ActivePropsHead; it != NULL; it = it->next)
            {
                if (it->type == PROP_TYPE_OBJ && it->obj != NULL && coopSetupOfs(it->obj) >= 0
                    && objIsMortal(it->obj) && !objGetDestroyedLevel(it->obj)
                    && (testobj < 2 || it->obj->obj == testobj))
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
    {   /* GE_COOP_TESTUSE=<grabtag>:<usetag>:<s>: <s> seconds into the mission pick up the
         * object tagged <grabtag> (-1 none), 5 s later press Use on <usetag> (harness only) */
        static s32 testuse = -1, grabtag, usetag;
        if (testuse == -1)
        {
            const char *e = getenv("GE_COOP_TESTUSE");
            testuse = 0;
            if (e != NULL && sscanf(e, "%d:%d:%d", &grabtag, &usetag, &testuse) != 3)
            {
                testuse = 0;
            }
        }
        if (testuse > 0 && coopInMission() && (sMissionTicks == (u32) testuse * 60 || sMissionTicks == (u32) testuse * 60 + 300))
        {
            ObjectRecord *obj = objFindByTagId(sMissionTicks == (u32) testuse * 60 ? grabtag : usetag);
            if (obj != NULL && obj->prop != NULL)
            {
                TICKOP op;
                portConLogf("ge: coop: testuse %s obj %d type %d\n", sMissionTicks == (u32) testuse * 60 ? "grab" : "use",
                    coopSetupOfs(obj), obj->type);
                if (sMissionTicks == (u32) testuse * 60)
                {
                    op = propPickupByPlayer(obj->prop, TRUE);
                }
                else
                {
                    op = propobjInteract(obj->prop);
                }
                propExecuteTickOperation(obj->prop, op);
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
    return sEnabled && !coopNetIsHost() && sLastSnapTick != 0 && coopRemoteHere(0) && !coopVersus();
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

/* A shot on a partner's puppet never touches the puppet: it is that player's
 * Bond, and a local death frees its model under the puppet code (the crash
 * in setsuboffset). With friendly fire on, the hit goes to that player's own
 * game, scaled as a guard's hit on Bond. Returns 1 for any puppet. */
s32 coopShotPuppet(ChrRecord *chr, s32 hitpart, coord3d *vector, s32 weaponid, s32 isPlayer)
{
    CoopEvent ev;
    s32 p;
    f32 damage;

    if (!sEnabled || chr == NULL || chr->prop == NULL || (p = coopPlayerOfProp(chr->prop)) < 0)
    {
        return 0;
    }
    if (!isPlayer || !coopSetting(COOPSET_FRIENDLYFIRE) || !coopRemoteAliveHere(p) || !coopInMission())
    {
        return 1;
    }
    damage = 0.125f * gunItemGetDestructionAmount(weaponid);
    if (hitpart == HIT_HEAD)
    {
        damage *= 2.0f;
    }
    else if (hitpart == HIT_GENERALHALF)
    {
        damage *= 0.25f;
    }
    else if (hitpart == HIT_GENERAL)
    {
        damage *= 0.5f;
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_HIT;
    ev.u.hit.damage = damage;
    ev.u.hit.angle = 0.0f;
    coopQueueEventTo(&ev, p);
    portConLogf("ge: coop: friendly fire on player %d (part %d, weapon %d): %.3f\n", p + 1, hitpart, weaponid, damage);
    (void) vector;
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
    coopObjNoteActor(ev->from, 1);
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
                portConLogf("ge: coop: hit from player %d: %.3f\n", ev->from + 1, ev->u.hit.damage);
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
        case COOPEV_OBJKNOCK:
            coopApplyObjKnock(ev);
            break;
        case COOPEV_OBJUSE:
            coopApplyObjUse(ev);
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
        case COOPEV_PLAYERNOTE:
            coopApplyNote(ev);
            break;
        case COOPEV_ENDALL:
            coopApplyEndAll();
            break;
        case COOPEV_PING:
            if (coopInMission())
            {
                coopPingReceive(&ev->u.ping, ev->from);
            }
            break;
        default:
            if (ev->kind >= COOPEV_VSHIT)
            {
                coopVersusApplyEvent(ev->kind, ev->from, ev->u.raw);
            }
            else if (ev->kind >= COOPEV_EXT_FIRST && ev->kind <= COOPEV_EXT_LAST)
            {
                coopExtApply(ev->kind, ev->u.raw, ev->from);
            }
            break;
    }
}

/* ---- versus (coop_versus.c) --------------------------------------------- */

void coopSendVersusEvent(s32 kind, s32 to, const void *payload, s32 len)
{
    CoopEvent ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = (u8) kind;
    if (payload != NULL)
    {
        memcpy(ev.u.raw, payload, (len < (s32) sizeof(ev.u.raw)) ? len : (s32) sizeof(ev.u.raw));
    }
    coopQueueEventTo(&ev, to);
}

void coopSendVersusPkt(void *pkt, s32 len)
{
    CoopStatePkt *p = (CoopStatePkt *) pkt;

    p->magic = COOP_MAGIC;
    p->version = COOP_VERSION;
    coopNetSend(pkt, len);
}

s32 coopRemoteInfo(s32 p, CoopRemoteInfo *out)
{
    CoopRemote *r;

    if (!coopRemoteHere(p))
    {
        return 0;
    }
    r = &sR[p];
    out->pos[0] = r->st.pos[0];
    out->pos[1] = r->st.pos[1];
    out->pos[2] = r->st.pos[2];
    out->yaw = r->st.yaw;
    out->dead = r->st.dead;
    out->health = r->st.health;
    out->armour = r->st.armour;
    out->look = r->st.look;
    return 1;
}

u32 coopMatchSerial(void)
{
    return coopNetIsHost() ? sMissionSerial : sFollowedMission;
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

/* coop_ping.c: our ping, for everyone */
void coopPingSend(CoopPing *ping)
{
    CoopEvent ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_PING;
    ev.u.ping = *ping;
    coopQueueEvent(&ev);
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

    if (sEnabled <= 0 || sApplyingRemoteObj || !coopNetHasPeer() || !coopPartnerHere() || coopVersus())
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
        coopObjNoteActor(ev->from, 1);
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

/* A shot knocks things loose (a padlock off a gate, what sat on a crate)
 * only in the shooter's game; the knock is sent so it happens everywhere.
 * The direction travels in world space: objBounce wants it in the view's. */

static s32 sKnockChildren;
static s32 sApplyingKnock;

void coopObjKnockChildren(s32 haschildren)
{
    sKnockChildren = haschildren;
}

void coopObjKnocked(ObjectRecord *obj, s32 bounce, coord3d *viewdir)
{
    CoopEvent ev;
    coord3d dir;
    s32 ofs;

    if (sEnabled <= 0 || sApplyingKnock || coopVersus() || !coopNetHasPeer() || !coopPartnerHere() || (!bounce && !sKnockChildren))
    {
        return;
    }
    ofs = coopSetupOfs(obj);
    if (ofs < 0)
    {
        return;
    }
    dir = *viewdir;
    mtx4RotateVecInPlace(currentPlayerGetViewToWorldMtxf(), (f32 *) &dir);
    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_OBJKNOCK;
    ev.u.knock.objofs = ofs;
    ev.u.knock.bounce = (u8) (bounce != 0);
    ev.u.knock.children = (u8) (sKnockChildren != 0);
    ev.u.knock.dir[0] = dir.f[0];
    ev.u.knock.dir[1] = dir.f[1];
    ev.u.knock.dir[2] = dir.f[2];
    ev.u.knock.stage = bossGetStageNum();
    coopQueueEvent(&ev);
}

static void coopApplyObjKnock(CoopEvent *ev)
{
    ObjectRecord *obj;
    Mtxf *v2w;
    coord3d dir;
    s32 i;

    if (ev->u.knock.stage != bossGetStageNum() || !coopInMission() || g_CurrentSetup.propDefs == NULL
        || coopSetupOfs((u8 *) g_CurrentSetup.propDefs + ev->u.knock.objofs) < 0)
    {
        return;
    }
    obj = (ObjectRecord *) ((u8 *) g_CurrentSetup.propDefs + ev->u.knock.objofs);
    if (obj->prop == NULL)
    {
        return;
    }
    /* world to view: the view's rotation transposed */
    v2w = currentPlayerGetViewToWorldMtxf();
    for (i = 0; i < 3; i++)
    {
        dir.f[i] = v2w->m[i][0] * ev->u.knock.dir[0] + v2w->m[i][1] * ev->u.knock.dir[1] + v2w->m[i][2] * ev->u.knock.dir[2];
    }
    sApplyingKnock = 1;
    if (ev->u.knock.children)
    {
        objDropRecursively(obj->prop);
    }
    if (ev->u.knock.bounce)
    {
        objBounce(obj, &dir);
    }
    sApplyingKnock = 0;
    portConLogf("ge: coop: player %d knocked obj %d loose\n", ev->from + 1, ev->u.knock.objofs);
}

/* A joiner pressing Use on a console, switch or the Runway plane marks it
 * activated only in its own game, and the level scripts that wait for that
 * run on the host: the press is sent there. */

static s32 sApplyingUse;

void coopObjUsed(ObjectRecord *obj)
{
    CoopEvent ev;
    s32 ofs;

    if (sEnabled <= 0 || sApplyingUse || coopVersus() || coopNetIsHost() || !coopNetHasPeer() || !coopPartnerHere())
    {
        return;
    }
    ofs = coopSetupOfs(obj);
    if (ofs < 0)
    {
        return;
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_OBJUSE;
    ev.u.use.objofs = ofs;
    ev.u.use.stage = bossGetStageNum();
    coopQueueEvent(&ev);
}

static void coopApplyObjUse(CoopEvent *ev)
{
    ObjectRecord *obj;

    if (!coopNetIsHost() || ev->u.use.stage != bossGetStageNum() || !coopInMission() || g_CurrentSetup.propDefs == NULL
        || coopSetupOfs((u8 *) g_CurrentSetup.propDefs + ev->u.use.objofs) < 0)
    {
        return;
    }
    obj = (ObjectRecord *) ((u8 *) g_CurrentSetup.propDefs + ev->u.use.objofs);
    if (obj->prop == NULL)
    {
        return;
    }
    sApplyingUse = 1;
    if (obj->type == PROPDEF_ALARM)
    {   /* the joiner's switch flipped its own alarm; the host's state is the one the joiners follow */
        if (alarmIsActive())
        {
            alarmDeactivate();
        }
        else
        {
            alarmActivate();
        }
    }
    obj->runtime_bitflags |= RUNTIMEBITFLAG_ACTIVATED;
    sub_GAME_7F03E6A0(obj->prop);
    coopObjNoteActor(ev->from, 0);  /* an objective the press completes is theirs (coop_objective.c) */
    sApplyingUse = 0;
    portConLogf("ge: coop: player %d used obj %d (type %d)\n", ev->from + 1, ev->u.use.objofs, obj->type);
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
            coopObjNoteActor(sWorldFrom, 0);
        }
    }
    for (d = ptr_last_deposit_in_room_subobject_entry_type21, i = 0; d != NULL && i < 32; d = d->next, i++)
    {
        if ((p->depositbits & (1u << i)) && !d->flag)
        {
            d->flag = 1;
            coopObjNoteActor(sWorldFrom, 0);
        }
    }
    for (ph = ptr_last_photo_obj_in_room_subobject_entry_type1E, i = 0; ph != NULL && i < 32; ph = ph->next, i++)
    {
        if ((p->photobits & (1u << i)) && !ph->flag)
        {
            ph->flag = 1;
            coopObjNoteActor(sWorldFrom, 0);
        }
    }
    if (p->keycopied && !g_CurrentPlayer->copiedgoldeneye)
    {
        g_CurrentPlayer->copiedgoldeneye = 1;
        coopObjNoteActor(sWorldFrom, 0);
    }
}


/* ---- pickups ---------------------------------------------------------------
 * What one player picks up, everyone gets: a gun with its ammo, ammo, keys
 * and mission items (the covert modem a level script hands the host at the
 * start included). Each game plays the pickup on its own copy of the setup
 * item, so the item really is in everyone's inventory and objective checks
 * find it there. Body armour is the exception: whoever takes it has it, and
 * it leaves the other worlds.
 *
 * Every game keeps the list of this mission's shared pickups. A pickup that
 * cannot happen yet here (dead, in a cutscene, or before the mission began)
 * waits on the list and is tried again; the host sends the whole list to a
 * player that arrives, so a late joiner gets what the team has. */


static s32 coopTeamPickFind(s32 ofs)
{
    s32 i;

    for (i = 0; i < sNumTeamPicks; i++)
    {
        if (sTeamPick[i] == ofs)
        {
            return i;
        }
    }
    return -1;
}

static s32 coopTeamPickAdd(s32 ofs, s32 from, s32 done)
{
    s32 i = coopTeamPickFind(ofs);

    if (i < 0 && sNumTeamPicks < COOP_MAX_TEAMPICKS)
    {
        i = sNumTeamPicks++;
        sTeamPick[i] = ofs;
        sTeamPickDone[i] = 0;
        sTeamPickFrom[i] = (u8) from;
    }
    if (i >= 0 && done)
    {
        sTeamPickDone[i] = 1;
    }
    return i;
}

static void coopSendPickup(s32 ofs, s32 to)
{
    CoopEvent ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_PICKUP;
    ev.u.pick.objofs = ofs;
    ev.u.pick.stage = bossGetStageNum();
    coopQueueEventTo(&ev, to);
}

void coopPickedUp(ObjectRecord *obj, s32 kept)
{
    s32 ofs;

    if (coopVersus())
    {   /* each arena keeps its own guns; only the scenario's item is shared */
        if (obj->type == PROPDEF_COLLECTABLE)
        {
            coopVersusPickedUp(((WeaponObjRecord *) obj)->weaponnum);
        }
        return;
    }
    if (sEnabled <= 0 || sApplyingRemotePick)
    {
        return;
    }
    coopObjNoteSelf();   /* a collect objective that turns now is ours (coop_objective.c) */
    ofs = coopSetupOfs(obj);
    if (ofs < 0)
    {
        coopItemsPickedUp(obj);     /* a dropped gun or a thrown item (coop_items.c) */
        return;
    }
    coopTeamPickAdd(ofs, coopSelf(), 1);
    portConLogf("ge: coop: we picked up obj %d (type %d)%s\n", ofs, obj->type, coopPartnerHere() ? "" : ", no partner here yet");
    if (coopNetHasPeer() && coopPartnerHere())
    {
        coopSendPickup(ofs, COOP_ALL);
    }
}

/* one shared pickup, here: 1 when it is settled (taken, or not ours to take) */
static s32 coopTeamPickTry(s32 i)
{
    ObjectRecord *obj;
    PropRecord *prop;
    TICKOP op;

    if (coopSetupOfs((u8 *) g_CurrentSetup.propDefs + sTeamPick[i]) < 0)
    {
        return 1;
    }
    obj = (ObjectRecord *) ((u8 *) g_CurrentSetup.propDefs + sTeamPick[i]);
    prop = obj->prop;
    if (prop == NULL || bondinvHasPropInInv(prop))
    {
        return 1; /* gone already, or we have it */
    }
    if (obj->type == PROPDEF_ARMOUR)
    {   /* not shared: it leaves this world */
        objFreePermanently(obj, TRUE);
        portConLogf("ge: coop: player %d took body armour %d: removed here\n", sTeamPickFrom[i] + 1, sTeamPick[i]);
        return 1;
    }
    if (!coopInMission() || g_CurrentPlayer->bonddead || g_ClockTimer <= 0)
    {
        return 0; /* later */
    }
    sApplyingRemotePick = 1;
    op = propPickupByPlayer(prop, TRUE);
    if (op != TICKOP_NONE)
    {
        propExecuteTickOperation(prop, op);
    }
    sApplyingRemotePick = 0;
    if (op == TICKOP_NONE)
    {
        return 0;
    }
    portConLogf("ge: coop: player %d picked up obj %d: ours too\n", sTeamPickFrom[i] + 1, sTeamPick[i]);
    return 1;
}

static void coopTeamPicksTick(void)
{
    s32 i;

    for (i = 0; i < sNumTeamPicks; i++)
    {
        if (!sTeamPickDone[i] && coopTeamPickTry(i))
        {
            sTeamPickDone[i] = 1;
        }
    }
}

/* host: a player arrived in the mission; everything the team has picked up so far */
static void coopResyncPicks(s32 player)
{
    s32 i;

    if (!coopNetIsHost() || sNumTeamPicks == 0)
    {
        return;
    }
    for (i = 0; i < sNumTeamPicks; i++)
    {
        coopSendPickup(sTeamPick[i], player);
    }
    portConLogf("ge: coop: resent %d pickups to player %d\n", sNumTeamPicks, player + 1);
}

static void coopApplyPickup(CoopEvent *ev)
{
    s32 i;

    if (ev->u.pick.stage != bossGetStageNum() || g_CurrentSetup.propDefs == NULL
        || coopSetupOfs((u8 *) g_CurrentSetup.propDefs + ev->u.pick.objofs) < 0)
    {
        return;
    }
    i = coopTeamPickFind(ev->u.pick.objofs);
    if (i >= 0 && sTeamPickDone[i])
    {
        return;
    }
    i = coopTeamPickAdd(ev->u.pick.objofs, ev->from, 0);
    coopObjNoteActor(ev->from, 0);
    if (i >= 0 && coopTeamPickTry(i))
    {
        sTeamPickDone[i] = 1;
    }
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

    coopTeamTimeFreeze();     /* the team's time as the mission ends (coop_teamtime.c) */
    coopCampaignMissionEnd(); /* a win together goes in coop_campaign.txt */
    if (sEnabled <= 0 || sApplyingRemoteEnd || !coopNetHasPeer() || !coopPartnerHere())
    {
        return;
    }
    coopTeamTimeSend();     /* host: the team's time first, so it is in before the debrief (coop_teamtime.c) */
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
    coopTeamTimeSend();     /* host: a joiner finished it; it debriefed on its own estimate, this corrects it */
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
        coopSpectateStop();
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
                coopSpectateStop();
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
        coopSpectateTick();
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
        coopSpectateStop();
        return 1;
    }
    coopSpectateTick();
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
    p.character = (u8) coopCharacter();
    memcpy(p.settings, sSettings, sizeof(p.settings));
    p.missionticks = coopInMission() ? sMissionTicks : 0;
    p.hold = (u8) ((sHold ? 1 : 0) | (sHoldArrived << 4));
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
        e->id = (i == 0) ? coopPlayerId(0) : coopNetPeerId(i - 1);   /* where to reach them if we go (coopMigrate) */
        snprintf(e->name, sizeof(e->name), "%s", coopPlayerName(i));
        e->ready = (i == 0) ? (u8) sLobbyReady : sChan[i - 1].hello.ready;
        e->inlobby = (i == 0) ? (u8) sOnLobbyScreen : sChan[i - 1].hello.inlobby;
        e->character = (i == 0) ? (u8) coopCharacter() : sChan[i - 1].hello.character;
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
            const char *f = getenv("GE_COOP_FRIENDLYFIRE");
            const char *h = getenv("GE_COOP_HOSTLEAVES");
            testdiff = 0;
            if (f != NULL)
            {
                sSettings[COOPSET_FRIENDLYFIRE] = (u8) atoi(f);
            }
            if (h != NULL)
            {
                sSettings[COOPSET_HOSTLEAVES] = (u8) atoi(h);
            }
            if (e != NULL)
            {
                sSettings[COOPSET_DIFFICULTY] = (u8) atoi(e);
                portConLogf("ge: coop: GE_COOP_DIFFICULTY=%d (%s)\n", atoi(e), coopDifficultyName(atoi(e)));
            }
        }
    }
    if (sEnabled < 0)
    {
        sEnabled = coopNetInitFirst();
    }
    if (sEnabled <= 0)
    {
        return 0;
    }
    sTick++;
    coopPollNet();
    coopSessionTick();
    coopExtTick(0);         /* chat, objective credit, ... (coop_ext.c) */
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
        if (coopSetting(COOPSET_VSMODE) & 1)
        {   /* versus (coop_versus.c, front.c coopVsStartMatch): the players it started with, from the start */
            if (sHostHello.missionticks > 600 || !coopVersusRoster((u32) coopSetting(COOPSET_VSMODE) >> 4, sHostHello.mission))
            {
                portConLogf("ge: versus: the host's match started without us\n");
                return 0;
            }
            coopVsStartMatch(sHostHello.stage);
            return 1;
        }
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
        sR[i].lost = 0;
    }
    coopMigrateReset();
}

/* The first start of the network (GE_COOP, -coophost / -coopjoin, the
 * lobby): the channels' numbering starts at 1 as after a reset, or the first
 * event queued on each would sit at seq 0, which is never sent. */
static s32 coopNetInitFirst(void)
{
    coopSessionReset();
    return coopNetInit();
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
        sEnabled = coopNetInitFirst();
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

/* player p's character pick: ours, a joiner's from its hello (host), the
 * host's roster (joiner) */
s32 coopPlayerCharacter(s32 p)
{
    s32 c;

    if (p < 0 || p >= COOP_MAX_PLAYERS)
    {
        return 0;
    }
    if (p == coopSelf())
    {
        return coopCharacter();
    }
    c = coopChanOfPlayer(p);
    return (c >= 0 && coopNetIsHost()) ? sChan[c].hello.character : sHostHello.roster[p].character;
}

s32 coopPropIsPartner(PropRecord *prop)
{
    return coopIsPuppet(prop);
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
    coopSayBye();   /* the others hear it now; a host hands the lobby to the next player */
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
{   /* after a migration that left us alone we still host the lobby */
    return sEnabled > 0 && coopNetIsOpen() && (coopNetHasPeer() || (sMigrated && coopNetIsHost()));
}


/* ---- when someone leaves: host migration -------------------------------------
 * The host runs the world (guards, scripts, doors, objects), so losing it is
 * the hard case. A joiner that hears nothing from the host for
 * COOP_SILENT_MS shows a banner and waits COOP_HOST_WAIT_MS for it to come
 * back (a hitch, a slow load). After that, or at once when the host says
 * goodbye (COOPPKT_BYE) or leaves the Steam lobby, the players still here
 * pick a new host: the lowest player number left in the host's last roster.
 * Everyone renumbers in that order (the new host 0, the rest 1..), the new
 * host keeps a transport slot for each of them and takes over its own copy
 * of the world, which has been mirroring the old host's all along: guard
 * positions and animations from the snapshots, doors, objects, pickups,
 * stage flags and alarm from the events, and from the AI sync below each
 * chr's script position, health and AI state. If the player picked does not
 * answer within COOP_NEWHOST_WAIT_MS the next one is tried.
 *
 * The lobby's Host Leaves rule (COOPSET_HOSTLEAVES) picks between carrying
 * on with the new host and everyone going back to the lobby, which the new
 * host then runs. A joiner that drops is kept as an idle Bond guards ignore
 * for COOP_DROP_MS and then removed; it can come back in that time or later
 * through the lobby (rejoin, coopChanSession). The host leaving on purpose
 * (the watch's Abort) is asked whether to hand the mission over or end it
 * for everyone. */

#define COOP_SILENT_MS        3000    /* nothing from the host this long: say so */
#define COOP_HOST_WAIT_MS     12000   /* and wait this much longer for it to come back */
#define COOP_NEWHOST_WAIT_MS  8000    /* the player picked to host must answer within this */
#define COOP_LOST_MS          3000    /* host: a joiner quiet this long has lost its connection */
#define COOP_DROP_MS          30000   /* and is gone after this long lost */
#define COOP_REMOTE_GONE_MS   40000   /* joiner: a relayed partner silent this long is gone */
#define COOP_HOSTTITLE_MS     2000    /* joiner: the host back in the menus this long, mid-mission */

enum { HOSTW_NONE, HOSTW_SILENT, HOSTW_NEWHOST };

static u32 sHostWaitMs;         /* when the current wait began */
static s32 sHostLeftHow;        /* 0 went quiet, else COOPBYE_* (a lobby leave counts as COOPBYE_LEAVE) */
static CoopRosterEnt sMigRoster[COOP_MAX_PLAYERS];   /* the old host's roster when it went */
static s32 sMigSelf;            /* our player number then */
static s32 sMigNewHost;         /* HOSTW_NEWHOST: the old player number we wait on */
static char sGoneHostName[24];
static u32 sHostTitleMs;        /* joiner: the host's hellos have said "menus" since */
static char sLobbyNotice[56];
static u32 sLobbyNoticeMs;
static s32 sAtExitSet;

static u32 coopNowMs(void)
{
    return (u32) (osGetTime() / 46875ull);
}

static s32 coopHostLeavesRule(void)
{
    return sSettings[COOPSET_HOSTLEAVES];
}

/* session news: a toast in play (coop_chat.c), and the same news on the lobby screen */
static void coopBanner(const char *line1, const char *line2)
{
    char text[112];

    snprintf(text, sizeof(text), (line2 != NULL && line2[0] != '\0') ? "%s|%s" : "%s", line1, line2);
    coopFeedToast(text, COOPTOAST_ALERT);
    snprintf(sLobbyNotice, sizeof(sLobbyNotice), "%s", line1);  /* one line fits the lobby row */
    sLobbyNoticeMs = coopNowMs();
}

static void coopMigrateReset(void)
{
    sHostWaiting = HOSTW_NONE;
    sHostLeftHow = 0;
    sMigNewHost = -1;
    sMigrated = 0;
    sPendingEnd = 0;
    sHostTitleMs = 0;
}

static void coopNote(s32 p, s32 what);

/* ---- leaving on purpose ---- */

static void coopSendBye(s32 reason, s32 newhost)
{
    CoopByePkt b;
    s32 i;

    if (sEnabled <= 0 || !coopNetIsOpen() || !coopNetHasPeer())
    {
        return;
    }
    memset(&b, 0, sizeof(b));
    b.magic = COOP_MAGIC;
    b.version = COOP_VERSION;
    b.type = COOPPKT_BYE;
    b.session = coopSelfSession();
    b.reason = (u8) reason;
    b.player = (u8) coopSelf();
    b.newhost = (u8) newhost;
    for (i = 0; i < 3; i++)
    {   /* unreliable: a few copies, the lobby leave or the silence covers the rest */
        coopNetSend(&b, sizeof(b));
    }
    portConLogf("ge: coop: said goodbye (%s)\n", reason == COOPBYE_HANDOFF ? "handing over" : "leaving");
}

/* the player a leaving host hands over to: the next one in the roster */
static s32 coopNextHost(void)
{
    s32 p;

    for (p = 1; p < COOP_MAX_PLAYERS; p++)
    {
        if (coopLobbyPlayerHere(p))
        {
            return p;
        }
    }
    return -1;
}

/* the game is closing: whoever is still connected hears it now, not after
 * the silence */
static void coopAtExit(void)
{
    if (sEnabled > 0 && coopNetIsOpen() && coopNetHasPeer())
    {
        if (coopNetIsHost())
        {
            coopSendAiSync();
        }
        coopSendBye(coopNetIsHost() ? COOPBYE_HANDOFF : COOPBYE_LEAVE, coopNetIsHost() ? coopNextHost() : 0);
    }
}

/* the lobby's Leave (a host hands the lobby to the next player) */
static void coopSayBye(void)
{
    if (coopNetIsHost())
    {
        coopSendBye(COOPBYE_HANDOFF, coopNextHost());
    }
    else
    {
        coopSendBye(COOPBYE_LEAVE, 0);
    }
}

/* The watch's Abort, confirmed (options.c). For a host with partners in the
 * mission it is a question first: 1 = ask (hand over or end for everyone). */
s32 coopHostAbortAsks(void)
{
    return sEnabled > 0 && coopNetIsHost() && coopInMission() && coopPartnerHere();
}

const char *coopHostAbortNextName(void)
{
    s32 p = coopNextHost();

    return (p > 0) ? coopPlayerName(p) : "";
}

/* the answer: handoff 1 gives the mission to the next player and leaves the
 * session; 0 ends it for everyone, who all go back to the lobby. The watch
 * then aborts our own mission as usual. */
void coopHostAbort(s32 handoff)
{
    if (!coopHostAbortAsks())
    {
        return;
    }
    if (handoff)
    {
        s32 next = coopNextHost();

        coopSendAiSync();
        coopFlushEvents();
        coopSendBye(COOPBYE_HANDOFF, next);
        portConLogf("ge: coop: host aborted, handing the mission to player %d\n", next + 1);
        coopLobbyStart(coopLobbyIsSteam() ? "steam:idle" : NULL);
    }
    else
    {
        CoopEvent ev;

        memset(&ev, 0, sizeof(ev));
        ev.kind = COOPEV_ENDALL;
        coopQueueEvent(&ev);
        coopFlushEvents();
        portConLogf("ge: coop: host ended the mission for everyone\n");
    }
}

/* our mission ends without a win: the vanilla Abort, minus its save reset */
static void coopEndMissionLocal(void)
{
    if (!coopInMission())
    {
        return;
    }
    set_missionstate(MISSION_STATE_0);
    bossRunTitleStage();
    mission_failed_or_aborted = TRUE;
    portConLogf("ge: coop: mission ended, back to the lobby\n");
}

static void coopPendingEndTick(void)
{
    if (sPendingEnd)
    {
        sPendingEnd = 0;
        coopEndMissionLocal();
    }
}

static void coopApplyEndAll(void)
{
    if (coopNetIsHost() || !coopInMission())
    {
        return;
    }
    coopBanner("THE HOST ENDED THE MISSION", "BACK TO THE LOBBY");
    coopScreenError("Mission Ended", "The host ended the mission\nfor everyone.");
    sPendingEnd = 1;
}

/* while paused (the watch) or in a cutscene coopTick does not run: keep
 * saying hello so nobody takes the quiet for a dropped game */
static void coopKeepAlive(void)
{
    static u32 last;
    u32 now = coopNowMs();
    u32 t = sTick;

    if (sEnabled <= 0 || !coopNetHasPeer() || now - last < 500)
    {
        return;
    }
    last = now;
    sTick = 0;  /* coopSendHello sends when sTick is a multiple of its period */
    coopSendHello();
    sTick = t;
}

/* ---- the mission card: everyone starts together (coop_screens.c draws it) ----
 * A co-op mission holds still on every game (lv.c lvlTick takes it as a pause)
 * until each player who was in the session when it started has loaded in, so
 * nobody plays the first seconds alone. The host decides: it waits for a fresh
 * hello from each of them in this stage (or 30 s, or until they drop) and its
 * hello's hold byte lets the joiners go. A late joiner, and versus, never hold. */
#define COOP_HOLD_MS      30000
#define COOP_HOLD_JOIN_MS 35000     /* a joiner gives up on the host's word a little later */

static void coopHoldStart(void)
{
    s32 p;

    sHold = 0;
    sHoldExpect = 0;
    sHoldArrived = 1;
    if (sEnabled <= 0 || !coopNetHasPeer() || coopVersus() || bossGetStageNum() == LEVELID_TITLE
        || getenv("GE_COOP_NOHOLD") != NULL)
    {
        return;
    }
    if (coopNetIsHost())
    {
        for (p = 1; p < COOP_MAX_PLAYERS; p++)
        {
            if (coopLobbyPlayerHere(p))
            {
                sHoldExpect |= (u8) (1 << p);
            }
        }
        sHold = sHoldExpect != 0;
    }
    else
    {
        sHold = !sLateJoin && sChan[0].helloSeen;
    }
    if (sHold)
    {
        sHoldMs = coopNowMs();
        sHoldTick = sTick;
        portConLogf("ge: coop: mission card up, waiting for %s\n", coopNetIsHost() ? "the others to load in" : "the host's go");
    }
}

static void coopHoldEnd(const char *why)
{
    u32 t = sTick;

    sHold = 0;
    portConLogf("ge: coop: mission card down after %d ms (%s)\n", (int) (coopNowMs() - sHoldMs), why);
    if (coopNetIsHost())
    {   /* tell them at once, not at the next hello */
        sTick = 0;
        coopSendHello();
        sTick = t;
    }
}

/* the held world's tick: the network keeps going, the mission does not */
static void coopHoldTick(void)
{
    const u32 now = coopNowMs();
    s32 p;

    sTick++;
    coopPollNet();
    coopSessionTick();
    coopFlushEvents();
    if ((sTick % 15) == 0)
    {
        u32 t = sTick;

        sTick = 0;
        coopSendHello();
        sTick = t;
    }
    if (coopNetIsHost())
    {
        for (p = 1; p < COOP_MAX_PLAYERS; p++)
        {
            const s32 c = coopChanOfPlayer(p);

            if (!(sHoldExpect & (1 << p)))
            {
                continue;
            }
            if (!coopLobbyPlayerHere(p))
            {   /* dropped while loading: don't wait for them */
                sHoldExpect &= (u8) ~(1 << p);
                portConLogf("ge: coop: player %d dropped before loading in\n", p + 1);
            }
            else if (c >= 0 && sChan[c].helloTick > sHoldTick && sChan[c].hello.stage == bossGetStageNum())
            {
                if (!(sHoldArrived & (1 << p)))
                {
                    portConLogf("ge: coop: player %d has loaded in\n", p + 1);
                }
                sHoldArrived |= (u8) (1 << p);
            }
        }
        if ((sHoldArrived & sHoldExpect) == sHoldExpect)
        {
            coopHoldEnd("everyone is in");
        }
        else if (now - sHoldMs > COOP_HOLD_MS)
        {
            coopHoldEnd("gave up waiting");
        }
        return;
    }
    if (sChan[0].helloTick > sHoldTick && sHostHello.stage == bossGetStageNum())
    {
        sHoldArrived = (u8) (sHostHello.hold >> 4);
        if (!(sHostHello.hold & 1))
        {
            coopHoldEnd("the host's go");
            return;
        }
    }
    if (now - sHoldMs > COOP_HOLD_JOIN_MS || sHostWaiting != HOSTW_NONE)
    {
        coopHoldEnd("no word from the host");
    }
}

/* lv.c lvlTick: 1 while the card holds the stage still */
s32 coopHoldWorld(void)
{
    return sHold;
}

/* the card's state: who is in (bits, by player) and the seconds the host
 * still waits; 0 when it is not up */
s32 coopHoldInfo(u8 *arrived, s32 *secs)
{
    s32 left;

    if (!sHold)
    {
        return 0;
    }
    left = (s32) (COOP_HOLD_MS - (coopNowMs() - sHoldMs) + 999) / 1000;
    *arrived = (u8) (sHoldArrived | (1 << coopSelf()));
    *secs = (left < 0) ? 0 : left;
    return 1;
}

/* ---- the AI sync: host sends, joiners keep ---- */

static void coopSendAiSync(void)
{
    CoopAiPkt pkt;
    s32 hdr = (s32) ((u8 *) &pkt.ent[0] - (u8 *) &pkt);
    s32 i, index = 0;

    if (g_ChrSlots == NULL || !coopNetHasPeer())
    {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = COOP_MAGIC;
    pkt.version = COOP_VERSION;
    pkt.type = COOPPKT_AISYNC;
    for (i = 0; i < g_NumChrSlots; i++)
    {
        ChrRecord *chr = &g_ChrSlots[i];
        CoopAiEnt *e;
        bool global;
        s32 id;

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
        id = (chr->ailist != NULL) ? chraiGetAIListID(chr->ailist, &global) : -1;
        e->chrnum = chr->chrnum;
        e->ailist = (id >= 0) ? (u16) id : 0xFFFF;
        e->aioffset = chr->aioffset;
        e->aireturnlist = chr->aireturnlist;
        e->actiontype = chr->actiontype;
        e->morale = chr->morale;
        e->alertness = chr->alertness;
        e->flags2 = chr->flags2;
        e->random = chr->random;
        e->chrflags = (u32) chr->chrflags;
        e->hidden = chr->hidden;
        e->padpreset1 = chr->padpreset1;
        e->chrpreset1 = chr->chrpreset1;
        e->timer60 = chr->timer60;
        e->damage = chr->damage;
        e->maxdamage = chr->maxdamage;
        if (pkt.count == COOP_AI_PER_PKT)
        {
            coopNetSend(&pkt, hdr + pkt.count * (s32) sizeof(CoopAiEnt));
            pkt.count = 0;
        }
    }
    if (pkt.count > 0)
    {
        coopNetSend(&pkt, hdr + pkt.count * (s32) sizeof(CoopAiEnt));
    }
}

static void coopRecvAiSync(CoopAiPkt *p)
{
    s32 k;

    if (!coopInMission() || g_ChrSlots == NULL)
    {
        return;
    }
    for (k = 0; k < p->count; k++)
    {
        ChrRecord *chr = chrFindByLiteralId(p->ent[k].chrnum);
        s32 slot;

        if (chr == NULL || chr->prop == NULL || coopIsPuppet(chr->prop))
        {
            continue;
        }
        slot = chr - g_ChrSlots;
        if (slot >= 0 && slot < COOP_MAX_CHRS)
        {
            sAiSync[slot] = p->ent[k];
            sAiSyncHave[slot] = 1;
        }
    }
}

#define COOP_AI_HIDDEN_BITS (CHRHIDDEN_TIMER_ACTIVE | CHRHIDDEN_BACKGROUND_AI | CHRHIDDEN_ALERT_GUARD_RELATED | CHRHIDDEN_0400)

/* We are the host now: our guards stop mirroring and run their own AI from
 * where the old host's were. */
static void coopTakeOverWorld(void)
{
    s32 i, n = 0, dead = 0;

    for (i = 0; g_ChrSlots != NULL && i < g_NumChrSlots && i < COOP_MAX_CHRS; i++)
    {
        ChrRecord *chr = &g_ChrSlots[i];
        CoopAiEnt *e = &sAiSync[i];

        sChrTarget[i].target = -1;
        sChrTarget[i].lastupdate60 = -1;
        sChrTarget[i].laststimulus60 = -1;
        if (!sAiSyncHave[i] || chr->model == NULL || chr->prop == NULL || coopIsPuppet(chr->prop) || chr->chrnum != e->chrnum)
        {
            continue;
        }
        if (e->ailist != 0xFFFF && ailistFindById(e->ailist) != NULL)
        {
            chr->ailist = ailistFindById(e->ailist);
            chr->aioffset = e->aioffset;
            chr->aireturnlist = e->aireturnlist;
        }
        chr->morale = e->morale;
        chr->alertness = e->alertness;
        chr->flags2 = e->flags2;
        chr->random = e->random;
        chr->chrflags = (CHRFLAG) e->chrflags;
        chr->hidden = (chr->hidden & ~COOP_AI_HIDDEN_BITS) | (e->hidden & COOP_AI_HIDDEN_BITS);
        chr->padpreset1 = e->padpreset1;
        chr->chrpreset1 = e->chrpreset1;
        chr->timer60 = e->timer60;
        chr->damage = e->damage;
        chr->maxdamage = e->maxdamage;
        chr->sleep = 0;
        if (e->actiontype == ACT_DIE || e->actiontype == ACT_DEAD)
        {   /* dead on the host: stays down and fades as a body does */
            chrStopFiring(chr);
            chr->actiontype = ACT_DEAD;
            chr->act_dead.allowfade = -1;
            chr->act_init.padding[0] = 0;
            dead++;
        }
        else
        {
            chr->actiontype = ACT_INIT;     /* stands, then its script picks up from aioffset */
        }
        n++;
    }
    sLastSnapTick = 0;
    sTickChr = NULL;
    g_CoopPropOverride = NULL;
    portConLogf("ge: coop: took over %d chrs (%d dead) from the old host\n", n, dead);
}

/* ---- renumbering ---- */

/* newof[old player number] = new number, -1 for players no longer in the session */
static void coopRenumber(const s32 *newof)
{
    CoopRemote old[COOP_MAX_PLAYERS];
    CoopRosterEnt roster[COOP_MAX_PLAYERS];
    u32 now = coopNowMs();
    s32 p, c;

    coopVersusRenumber(newof);   /* a versus match's player map too */
    memcpy(old, sR, sizeof(old));
    memset(sR, 0, sizeof(sR));
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        sR[p].puppetAnim = -1;
    }
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        CoopRemote *r;

        if (p == sMigSelf)
        {
            continue;
        }
        if (newof[p] < 0)
        {
            if (old[p].puppet != NULL)
            {
                coopPuppetRemove(&old[p]);  /* the old host's Bond goes with it */
            }
            continue;
        }
        r = &sR[newof[p]];
        *r = old[p];
        r->st.player = (u8) newof[p];
        r->seenMs = now;
        r->seenTick = sTick;
        if (r->puppet != NULL && r->puppet->chr != NULL)
        {
            r->puppet->chr->chrnum = COOP_PUPPET_CHRNUM + newof[p];
        }
    }
    memset(roster, 0, sizeof(roster));
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        if (newof[p] >= 0)
        {
            roster[newof[p]] = sMigRoster[p];
        }
    }
    memcpy(sHostHello.roster, roster, sizeof(roster));
    sHostHello.youare = (u8) newof[sMigSelf];
    memset(sChan, 0, sizeof(sChan));
    for (c = 0; c < COOP_NET_MAX_PEERS; c++)
    {
        coopChanReset(&sChan[c]);
        sChan[c].heardMs = now;
    }
    coopStatsRemap(newof);
}

/* The host is gone: pick the next one and go over to it. */
static void coopMigrate(void)
{
    s32 list[COOP_MAX_PLAYERS], newof[COOP_MAX_PLAYERS];
    unsigned long long ids[COOP_NET_MAX_PEERS];
    s32 n = 0, p, i;
    char line[56];

    for (p = 1; p < COOP_MAX_PLAYERS; p++)
    {
        if (sMigRoster[p].present || p == sMigSelf)
        {
            list[n++] = p;
        }
    }
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        newof[p] = -1;
    }
    for (i = 0; i < n; i++)
    {
        newof[list[i]] = i;
    }
    if (n == 0 || newof[sMigSelf] < 0)
    {   /* cannot happen (we are always in the list); start over alone */
        list[0] = sMigSelf;
        n = 1;
        newof[sMigSelf] = 0;
    }
    portConLogf("ge: coop: host migration: player %d hosts (we were %d, now %d; %d players)\n",
                list[0] + 1, sMigSelf + 1, newof[sMigSelf] + 1, n);

    if (list[0] == sMigSelf)
    {
        for (i = 1; i < n && i <= COOP_NET_MAX_PEERS; i++)
        {
            ids[i - 1] = sMigRoster[list[i]].id;
        }
        sMissionSerial = sHostHello.mission;    /* the joiners have followed this one already */
        coopNetMigrate(1, ids, n - 1);
        coopRenumber(newof);
        if (coopInMission())
        {
            coopTakeOverWorld();
        }
        sHostWaiting = HOSTW_NONE;
        snprintf(line, sizeof(line), "%s %s", sGoneHostName, sHostLeftHow == COOPBYE_HANDOFF ? "HANDED OVER" : "LEFT");
        coopBanner("YOU ARE THE HOST NOW", line);
    }
    else
    {
        ids[0] = sMigRoster[list[0]].id;
        coopNetMigrate(0, ids, 1);
        coopRenumber(newof);
        sHostWaiting = HOSTW_NEWHOST;
        sHostWaitMs = coopNowMs();
        sMigNewHost = list[0];
    }
    sMigrated = 1;
    if (coopHostLeavesRule() == 1 && coopInMission())
    {
        sPendingEnd = 1;    /* the lobby's rule: back to the lobby, with the new host */
        coopScreenError("The Host Left", coopNetIsHost() ? "The lobby's Host Leaves rule\nsends everyone back here.\nYou are the host now."
                                                         : "The lobby's Host Leaves rule\nsends everyone back here.\nThe next player hosts now.");
    }
}

/* the roster to pick from: the host's last word on who is here; player 0 is
 * the one that is gone */
static void coopMigrateFromRoster(void)
{
    memcpy(sMigRoster, sHostHello.roster, sizeof(sMigRoster));
    sMigSelf = coopSelf();
    sMigRoster[sMigSelf].present = 1;
    coopMigrate();
}

static void coopUpper(char *s)
{
    for (; *s != '\0'; s++)
    {
        if (*s >= 'a' && *s <= 'z')
        {
            *s -= 'a' - 'A';
        }
    }
}

/* the host is gone (how: 0 went quiet, else COOPBYE_*) */
static void coopHostGone(s32 how)
{
    if (coopNetIsHost() || !sChan[0].helloSeen)
    {
        return;
    }
    snprintf(sGoneHostName, sizeof(sGoneHostName), "%s", coopPlayerName(0));
    coopUpper(sGoneHostName);
    sHostLeftHow = how;
    portConLogf("ge: coop: the host %s\n", how == COOPBYE_HANDOFF ? "handed over" : how ? "left" : "went quiet");
    coopMigrateFromRoster();
}

static void coopRecvBye(s32 c, CoopByePkt *b)
{
    s32 p = coopPlayerOfChan(c);

    if (coopNetIsHost())
    {
        if (sR[p].valid || coopLobbyPlayerHere(p))
        {
            portConLogf("ge: coop: player %d left the session\n", p + 1);
            coopNote(p, COOPNOTE_LEFT);
        }
        sR[p].valid = 0;
        sR[p].lost = 0;
        sChan[c].helloSeen = 0;
        coopNetDropPeer(c);
        return;
    }
    if (c == 0 && sHostWaiting != HOSTW_NEWHOST)
    {
        coopHostGone(b->reason == COOPBYE_HANDOFF ? COOPBYE_HANDOFF : COOPBYE_LEAVE);
    }
}

/* ---- joiners that drop (host), and notes about them (everyone) ---- */

static void coopNote(s32 p, s32 what)
{
    CoopEvent ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = COOPEV_PLAYERNOTE;
    ev.u.note.player = (u8) p;
    ev.u.note.what = (u8) what;
    snprintf(ev.u.note.name, sizeof(ev.u.note.name), "%s", coopPlayerName(p));
    coopQueueEventTo(&ev, COOP_ALL);
    coopApplyNote(&ev);
}

static void coopApplyNote(CoopEvent *ev)
{
    s32 p = ev->u.note.player;
    char msg[64];
    const char *name = ev->u.note.name[0] ? ev->u.note.name : "A partner";

    if (p >= COOP_MAX_PLAYERS)
    {
        return;
    }
    switch (ev->u.note.what)
    {
        case COOPNOTE_LOST:
            sR[p].lost = 1;
            sR[p].lostMs = coopNowMs();
            snprintf(msg, sizeof(msg), "%s lost connection.\n", name);
            break;
        case COOPNOTE_BACK:
            sR[p].lost = 0;
            snprintf(msg, sizeof(msg), "%s is back.\n", name);
            break;
        case COOPNOTE_GONE:
            sR[p].valid = 0;
            sR[p].lost = 0;
            snprintf(msg, sizeof(msg), "%s disconnected.\n", name);
            break;
        case COOPNOTE_LEFT:
            sR[p].valid = 0;
            sR[p].lost = 0;
            snprintf(msg, sizeof(msg), "%s left the game.\n", name);
            break;
        case COOPNOTE_LEFTMISSION:
            sR[p].valid = 0;
            sR[p].lost = 0;
            snprintf(msg, sizeof(msg), "%s left the mission.\n", name);
            break;
        default:
            return;
    }
    portConLogf("ge: coop: %s", msg);
    if (coopInMission())
    {
        coopFeedToast(msg, p);
    }
    else
    {
        snprintf(sLobbyNotice, sizeof(sLobbyNotice), "%s", msg);
        sLobbyNoticeMs = coopNowMs();
    }
}

static void coopRemoteWatch(s32 p)
{
    CoopRemote *r = &sR[p];
    u32 now = coopNowMs();
    s32 c;

    if (!r->valid)
    {
        return;
    }
    if (!coopNetIsHost())
    {   /* the host: coopSessionTick. Other joiners: the host's notes, and
         * silence long past the host's own drop as a last resort */
        if (p != 0 && sHostWaiting == HOSTW_NONE && now - r->seenMs > COOP_REMOTE_GONE_MS)
        {
            portConLogf("ge: coop: nothing from player %d for %d s, dropping its Bond\n", p + 1, COOP_REMOTE_GONE_MS / 1000);
            r->valid = 0;
            r->lost = 0;
        }
        return;
    }
    c = coopChanOfPlayer(p);
    if (c < 0)
    {
        return;
    }
    if (!coopNetPeerConnected(c))
    {   /* the transport let it go (it left the Steam lobby) */
        coopNote(p, COOPNOTE_LEFT);
        return;
    }
    if (sChan[c].helloSeen && (s32) (sChan[c].helloTick - r->seenTick) > 0 && sChan[c].hello.stage != r->st.stage)
    {   /* its newest hello is from somewhere else: it left this mission (Abort, or its game failed it) */
        if (coopInMission() && r->st.stage == bossGetStageNum())
        {
            coopNote(p, COOPNOTE_LEFTMISSION);
        }
        r->valid = 0;
        r->lost = 0;
        return;
    }
    if (!r->lost && now - sChan[c].heardMs > COOP_LOST_MS)
    {
        coopNote(p, COOPNOTE_LOST);
    }
    else if (r->lost && now - sChan[c].heardMs < 1000)
    {
        coopNote(p, COOPNOTE_BACK);
    }
    else if (r->lost && now - r->lostMs > COOP_DROP_MS)
    {
        coopNote(p, COOPNOTE_GONE);
        sChan[c].helloSeen = 0;
        coopNetDropPeer(c);
    }
}

/* ---- every tick, menus and missions ---- */

static void coopSessionTick(void)
{
    u32 now = coopNowMs();
    u32 quiet;
    s32 c;

    if (!sAtExitSet)
    {
        sAtExitSet = 1;
        atexit(coopAtExit);
    }
    if (sEnabled <= 0 || !coopNetIsOpen())
    {
        return;
    }
    if (coopNetIsHost())
    {   /* a slot nobody has spoken from for a long time (the lobby, or a
         * joiner kept for after a migration that never came) is freed */
        for (c = 0; c < COOP_NET_MAX_PEERS; c++)
        {
            if (coopNetPeerConnected(c) && sChan[c].heardMs != 0 && now - sChan[c].heardMs > COOP_LOST_MS + COOP_DROP_MS
                && !sR[coopPlayerOfChan(c)].valid)
            {
                portConLogf("ge: coop: freeing slot %d (silent)\n", c);
                sChan[c].helloSeen = 0;
                coopNetDropPeer(c);
            }
        }
        return;
    }
    if (!sChan[0].helloSeen && sHostWaiting != HOSTW_NEWHOST)
    {
        return;     /* not connected to a host yet */
    }
    quiet = now - sChan[0].heardMs;
    switch (sHostWaiting)
    {
        case HOSTW_NONE:
            if (coopNetHostLeft())
            {
                coopHostGone(COOPBYE_LEAVE);
            }
            else if (quiet > COOP_SILENT_MS)
            {
                sHostWaiting = HOSTW_SILENT;
                sHostWaitMs = now;
                portConLogf("ge: coop: nothing from the host for %d s, waiting for it\n", (int) (quiet / 1000));
            }
            else if (coopInMission() && sHostHello.stage == LEVELID_TITLE && sMissionTicks > 300 && !sPendingEnd)
            {   /* the host is in the menus while we play on: it left the mission */
                if (sHostTitleMs == 0)
                {
                    sHostTitleMs = now;
                }
                else if (now - sHostTitleMs > COOP_HOSTTITLE_MS)
                {
                    coopBanner("THE HOST LEFT THE MISSION", "BACK TO THE LOBBY");
                    coopScreenError("Back to the Lobby", "The host left the mission,\nso it ended for everyone.");
                    sPendingEnd = 1;
                    sHostTitleMs = 0;
                }
            }
            else
            {
                sHostTitleMs = 0;
            }
            break;
        case HOSTW_SILENT:
            if (quiet < 1000)
            {
                sHostWaiting = HOSTW_NONE;
                portConLogf("ge: coop: the host is back\n");
                coopBanner("THE HOST IS BACK", "");
            }
            else if (coopNetHostLeft())
            {
                coopHostGone(COOPBYE_LEAVE);
            }
            else if (now - sHostWaitMs > COOP_HOST_WAIT_MS)
            {
                coopHostGone(0);
            }
            break;
        case HOSTW_NEWHOST:
            if (sChan[0].helloSeen)
            {
                char line[56];

                sHostWaiting = HOSTW_NONE;
                snprintf(line, sizeof(line), "%s IS THE HOST NOW", sMigRoster[sMigNewHost].name);
                coopUpper(line);
                coopBanner(line, sHostLeftHow == COOPBYE_HANDOFF ? "THE OLD HOST HANDED OVER" : "THE OLD HOST LEFT");
                portConLogf("ge: coop: connected to the new host, we are player %d now\n", coopSelf() + 1);
            }
            else if (now - sHostWaitMs > COOP_NEWHOST_WAIT_MS)
            {   /* it did not take over: it is player 0 in our renumbered
                 * roster now, so go round again without it */
                portConLogf("ge: coop: the player picked did not take over, trying the next\n");
                coopMigrateFromRoster();
            }
            break;
    }
}

/* ---- for the HUD and the lobby screen ---- */

/* the host-wait notice's two lines (capitals), 1 while the host is silent or
 * a new one is being reached; one-off news goes through coopBanner as a toast */
s32 coopHudSessionBanner(char *line1, char *line2, s32 cap)
{
    u32 now = coopNowMs();

    if (sEnabled <= 0)
    {
        return 0;
    }
    if (sHostWaiting == HOSTW_SILENT)
    {
        s32 left = (s32) (COOP_HOST_WAIT_MS - (now - sHostWaitMs) + 999) / 1000;

        snprintf(line1, cap, "HOST NOT RESPONDING: %s", coopPlayerName(0));
        snprintf(line2, cap, coopHostLeavesRule() == 1 ? "BACK TO THE LOBBY IN %d" : "WAITING FOR THE HOST %d",
                 left < 0 ? 0 : (int) left);
        return 1;
    }
    if (sHostWaiting == HOSTW_NEWHOST)
    {
        snprintf(line1, cap, "%s %s", sGoneHostName, sHostLeftHow == COOPBYE_HANDOFF ? "HANDED OVER" : "LEFT");
        snprintf(line2, cap, "CONNECTING TO THE NEW HOST...");
        return 1;
    }
    return 0;
}

/* a line for the lobby screen about who came and went, NULL when none */
const char *coopLobbyNotice(void)
{
    static char text[64];
    char l1[56], l2[56];

    if (sEnabled > 0 && sHostWaiting != HOSTW_NONE && coopHudSessionBanner(l1, l2, sizeof(l1)))
    {
        snprintf(text, sizeof(text), "%s\n", l2);
        return text;
    }
    if (sLobbyNoticeMs != 0 && coopNowMs() - sLobbyNoticeMs < 15000)
    {
        snprintf(text, sizeof(text), "%s", sLobbyNotice);
        if (text[0] != '\0' && text[strlen(text) - 1] != '\n')
        {
            strcat(text, "\n");
        }
        return text;
    }
    return NULL;
}

/* player p's Bond stands idle: its game stopped talking (name tags, status) */
s32 coopHudPlayerLost(s32 p)
{
    if (p == 0 && !coopNetIsHost())
    {
        return sHostWaiting == HOSTW_SILENT;
    }
    return p >= 0 && p < COOP_MAX_PLAYERS && sR[p].lost;
}

/* the lobby's rule for a host that leaves: 0 the next player hosts, 1 everyone back to the lobby */
s32 coopHostLeavesCount(void)
{
    return 2;
}

const char *coopHostLeavesName(s32 rule)
{
    return (rule == 1) ? "Back to Lobby\n" : "Next Player Hosts\n";
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

/* A level script's "either player" check (chrai.c) passed: k 0 for our own
 * Bond, else the (k-1)-th living partner. The script's last passer is kept, so
 * when it then sets stage flags (an objective's trigger: the elevator, a
 * room) the credit goes to the player its check was about, not to whoever a
 * different script's check (run every tick) last saw. */

void coopAltBondCredit(void *script, s32 k)
{
    s32 who = coopSelf();
    s32 p, i;

    if (k > 0)
    {
        for (p = 1, k--, who = -1; p < COOP_MAX_PLAYERS && who < 0; p++)
        {
            if (coopRemoteAliveHere(p) && k-- == 0)
            {
                who = p;
            }
        }
    }
    if (who < 0)
    {
        return;
    }
    for (i = 0; i < COOP_SCRIPTPASS && sScriptPass[i].script != script; i++)
    {
    }
    if (i == COOP_SCRIPTPASS)
    {
        i = sScriptPassNext;
        sScriptPassNext = (sScriptPassNext + 1) % COOP_SCRIPTPASS;
    }
    sScriptPass[i].script = script;
    sScriptPass[i].player = (s8) who;
}

void coopScriptSetsFlags(void *script)
{
    s32 i;

    for (i = 0; i < COOP_SCRIPTPASS; i++)
    {
        if (sScriptPass[i].script == script)
        {
            coopObjNoteActor(sScriptPass[i].player, 0);
            return;
        }
    }
}


/* ---- explosives -------------------------------------------------------------
 * A grenade, mine or rocket a joiner fires exists only in its own game. Its
 * blast there shows the effect and hurts the joiner; the same blast is set
 * off in the host's game, which owns the damage to guards and objects. The
 * host in turn sends every such blast (its own, the guards', and the ones
 * relayed from a joiner) to the other joiners, whose copy shows the effect
 * and hurts their own Bond. Object explosions are replayed through OBJDMG. */

static s32 sApplyingRemoteBoom;
static s32 sApplyingHostBoom;    /* joiner: playing the host's explosion */

void coopExplosionCreated(PropRecord *source, coord3d *pos, s32 type, u8 *rooms)
{
    CoopEvent ev;
    s32 i;

    if (coopVersus())
    {
        coopVersusExplosion(source, pos, type, rooms);
        return;
    }
    if (source != NULL || type == EXPLOSION_DEF_PLAYER || sApplyingHostBoom)
    {
        return; /* destroyed objects replay from the host's damage events */
    }
    if (coopItemsExploding())
    {
        return; /* a thrown item: every world sets off its own copy (coop_items.c) */
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
    if (coopIsMirroringClient())
    {
        if (!sApplyingRemoteBoom)
        {
            coopQueueEventTo(&ev, 0);
            portConLogf("ge: coop: our explosion (type %d), sent to host\n", type);
        }
        return;
    }
    if (sEnabled > 0 && coopNetIsHost() && coopNetHasPeer() && coopInMission())
    {   /* the host's (its guards' grenades, a launcher round, a joiner's passed on): seen and felt everywhere */
        s32 p;

        for (p = 1; p < COOP_MAX_PLAYERS; p++)
        {
            if (p != sApplyingRemoteBoom && coopRemoteHere(p))
            {
                coopQueueEventTo(&ev, p);
            }
        }
    }
}

/* coop_stats.c: the explosion being made is the partner's */
s32 coopApplyingPartnerExplosion(void)
{
    if (sApplyingRemoteBoom == 0 && coopNetIsHost() && coopItemsExplodingOwner() > 1)
    {
        return coopItemsExplodingOwner() - 1;   /* the host's copy of a joiner's mine or grenade */
    }
    return sApplyingRemoteBoom;
}

/* coop_items.c */
s32 coopInMissionNow(void)
{
    return sEnabled > 0 && coopInMission();
}

static void coopApplyExplosion(CoopEvent *ev)
{
    coord3d pos;
    StandTile *stan;

    if (!coopInMission())
    {
        return;
    }
    pos.f[0] = ev->u.boom.pos[0];
    pos.f[1] = ev->u.boom.pos[1];
    pos.f[2] = ev->u.boom.pos[2];
    stan = stanFindTileBelowPos(&pos, NULL, NULL);
    if (stan == NULL)
    {   /* on a ceiling or a wall's top: look from a little higher */
        coord3d up = pos;

        up.f[1] += 150.0f;
        stan = stanFindTileBelowPos(&up, NULL, NULL);
    }
    if (stan == NULL || ev->u.boom.rooms[0] == 0xFF)
    {
        portConLogf("ge: coop: player %d's explosion at %.0f %.0f %.0f has no floor here, dropped\n", ev->from + 1, pos.f[0], pos.f[1], pos.f[2]);
        return;
    }
    if (!coopNetIsHost())
    {   /* the host's: our copy shows it and hurts our Bond; guards and objects are the host's business */
        sApplyingHostBoom = 1;
        explosionCreate(NULL, &pos, stan, ev->u.boom.type, 0, 0, ev->u.boom.rooms, 0);
        sApplyingHostBoom = 0;
        portConLogf("ge: coop: host explosion (type %d) at %.0f %.0f %.0f\n", ev->u.boom.type, pos.f[0], pos.f[1], pos.f[2]);
        return;
    }
    sApplyingRemoteBoom = ev->from;   /* whose (coop_stats.c) */
    coopObjNoteActor(ev->from, 1);
    explosionCreate(NULL, &pos, stan, ev->u.boom.type, 0, 0, ev->u.boom.rooms, 0);
    sApplyingRemoteBoom = 0;
    portConLogf("ge: coop: peer explosion (type %d) at %.0f %.0f %.0f\n", ev->u.boom.type, pos.f[0], pos.f[1], pos.f[2]);
}


/* ---- cutscenes and script presentation --------------------------------------
 * Level scripts run only on the host, but some of their commands are about
 * what the player sees: cutscene cameras, fades, locking the controls, on-
 * screen text, music, and the props an ending moves into its shot (the
 * Runway plane, whose own script then takes off on the synced stage flag).
 * The host sends each of those commands' bytes and the joiner runs that one
 * command through its own interpreter. */

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
        case AI_HideAllChrs:
        case AI_ShowAllChrs:
        case AI_MoveObject:     /* the Runway plane to its take-off pad */
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

/* ---- the feature batch (coop_ext.c and friends) ------------------------------
 * Chat, spectating, objective credit, team times and cross-save keep their
 * own files and reach the session through these. */

/* a reliable event of an EXT kind, to player `to` or COOP_ALL */
void coopExtSend(s32 kind, s32 to, const void *payload, s32 len)
{
    CoopEvent ev;

    if (sEnabled <= 0 || kind < COOPEV_EXT_FIRST || kind > COOPEV_EXT_LAST)
    {
        return;
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = (u8) kind;
    memcpy(ev.u.raw, payload, (len < (s32) sizeof(ev.u.raw)) ? len : (s32) sizeof(ev.u.raw));
    coopQueueEventTo(&ev, to);
}

s32 coopExtSelf(void)
{
    return (sEnabled > 0) ? coopSelf() : 0;
}

s32 coopExtIsHost(void)
{
    return sEnabled > 0 && coopNetIsHost();
}

u32 coopExtMissionTicks(void)
{
    return sMissionTicks;
}

/* another player in this mission with us: where they are and look */
s32 coopExtRemoteView(s32 p, CoopExtView *out)
{
    CoopRemote *r;

    if (sEnabled <= 0 || !coopRemoteHere(p))
    {
        return 0;
    }
    r = &sR[p];
    out->pos.f[0] = r->st.pos[0];
    out->pos.f[1] = r->st.pos[1];
    out->pos.f[2] = r->st.pos[2];
    out->yaw = r->st.yaw;
    out->pitch = r->st.look.aimpitch;
    out->dead = r->st.dead;
    out->prop = (r->puppet != NULL && r->puppet->chr != NULL) ? r->puppet : NULL;
    out->stan = (out->prop != NULL) ? out->prop->stan : NULL;
    out->weapon = r->st.look.weapon[0];
    out->health = r->st.health;
    return 1;
}

/* the host's hello: how long it has been in this mission (joiner) */
u32 coopExtHostMissionTicks(void)
{
    return (sEnabled > 0 && !coopNetIsHost()) ? sHostHello.missionticks : sMissionTicks;
}

/* a packet of an EXT type to player `to` (a joiner can reach only the host),
 * or every peer with COOP_ALL */
void coopExtSendPacket(void *pkt, s32 len, s32 to)
{
    u32 *hdr = (u32 *) pkt;
    s32 c;

    if (sEnabled <= 0 || len < 8)
    {
        return;
    }
    hdr[0] = COOP_MAGIC;
    ((u16 *) pkt)[2] = COOP_VERSION;
    for (c = 0; c < COOP_NET_MAX_PEERS; c++)
    {
        if (coopNetPeerConnected(c) && (to == COOP_ALL || coopPlayerOfChan(c) == to))
        {
            coopNetSendTo(c, pkt, len);
        }
    }
}

/* co-op is on: a session or Steam's idle lobby screen (coop_presence.c) */
s32 coopEnabled(void)
{
    return sEnabled > 0;
}

/* the lobby screen is up (coop_browse.c refreshes Public Games only then) */
s32 coopLobbyOnScreen(void)
{
    return sOnLobbyScreen;
}

/* join a Steam lobby by id (a public game from coop_browse.c) */
void coopLobbyJoinLobbyId(unsigned long long lobby)
{
    char spec[48];

    sprintf(spec, "steam:join:%llu", lobby);
    coopLobbyStart(spec);
}

/* the co-op protocol's version: public games list only those of ours */
s32 coopExtVersion(void)
{
    return COOP_VERSION;
}

/* ---- names ------------------------------------------------------------------
 * Steam names are UTF-8; the game's fonts have ASCII. Accented Latin letters
 * keep their base letter (Camilo, Jose, Muller), anything else (emoji,
 * symbols, other scripts) is left out rather than shown as '?'. */

static const char sLatin1[] =       /* U+00C0..U+00FF */
    "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs" "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";
static const char sLatinA[] =       /* U+0100..U+017F */
    "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiJjJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";

void coopAsciiName(char *out, s32 cap, const char *in, s32 upper)
{
    const u8 *p = (const u8 *) in;
    s32 n = 0;

    while (p != NULL && *p != '\0' && *p != '\n' && n < cap - 1)
    {
        u32 cp;
        s32 len;
        char c = 0;
        s32 k;

        if (*p < 0x80)
        {
            cp = *p;
            len = 1;
        }
        else if ((*p & 0xE0) == 0xC0)
        {
            cp = *p & 0x1F;
            len = 2;
        }
        else if ((*p & 0xF0) == 0xE0)
        {
            cp = *p & 0x0F;
            len = 3;
        }
        else if ((*p & 0xF8) == 0xF0)
        {
            cp = *p & 0x07;
            len = 4;
        }
        else
        {
            p++;    /* a stray continuation byte */
            continue;
        }
        for (k = 1; k < len; k++)
        {
            if ((p[k] & 0xC0) != 0x80)
            {
                break;  /* cut short (a name clipped to its buffer) */
            }
            cp = (cp << 6) | (p[k] & 0x3F);
        }
        p += k;
        if (k < len)
        {
            continue;
        }
        if (cp >= 0x20 && cp < 0x7F)
        {
            c = (char) cp;
        }
        else if (cp >= 0xC0 && cp <= 0xFF)
        {
            c = sLatin1[cp - 0xC0];
        }
        else if (cp >= 0x100 && cp <= 0x17F)
        {
            c = sLatinA[cp - 0x100];
        }
        else if (cp == 0xA0 || cp == 0x3000)
        {
            c = ' ';
        }
        if (c == 0)
        {
            continue;
        }
        if (upper && c >= 'a' && c <= 'z')
        {
            c -= 'a' - 'A';
        }
        if (c == ' ' && (n == 0 || out[n - 1] == ' '))
        {
            continue;   /* a dropped symbol leaves no double space */
        }
        out[n++] = c;
    }
    while (n > 0 && out[n - 1] == ' ')
    {
        n--;
    }
    if (n == 0 && cap > 1)
    {
        out[n++] = '?';
    }
    out[n] = '\0';
}
