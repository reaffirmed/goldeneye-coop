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
#include "gun.h"
#include "math_atan2f.h"   /* the game's math.h has no atan2f: undeclared, it returned garbage (-w hides it) */

extern s32 chrlvCurrentPlayerCall7F0B0E24(ChrRecord *self);

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

void play_sound_for_shot_actor(ChrRecord *self);
u32 randomGetNext(void);
extern void portConLogf(const char *fmt, ...);

#define COOP_MAGIC   0x50434547u /* "GECP" */
#define COOP_VERSION 14 /* 2: lobby fields in HELLO, health in STATE; 3: session ids; 4: up to four players; 5: guns, shots and characters;
                          6: host migration (AI sync, bye, addresses in the roster);
                          7: pings, chat, objective credit, team times, cross-save (COOPEV 16, 20..31, COOPPKT 10);
                          8: versus (COOPEV 32..36, COOPPKT 16);
                          9: shared pickups, thrown items, drops, knocks, Use, host blasts (COOPEV 17, 18, 25..28);
                          10: hold Use to revive (STATE revive), more stats (COOPEV 56..63, COOPPKT 24..25 kept for
                              revive, cutscenes and the results);
                          11: sender clocks in STATE and GUARDS (smooth movement), round trip (COOPPKT_TIME);
                          12: lives, bleed-out and respawn times (COOPSET 8..10, 16 settings in HELLO, STATE out);
                              gadgets in USE, where a throw stuck (COOPEV 29), the tank (COOPPKT 11);
                          13: Player Scaling (COOPSET 11), Friendly Fire Reduced and blasts, the victim scales a
                              partner's hit (HIT friendly), a blast's owner (EXPLODE), Friendly hits stat;
                          14: the host's mission countdown in WORLD, its blast relayed (COOPEV_AICMD) */

enum { COOPPKT_STATE = 1, COOPPKT_GUARDS = 3, COOPPKT_EVENTS = 5, COOPPKT_WORLD = 6, COOPPKT_HELLO = 7, COOPPKT_AISYNC = 8, COOPPKT_BYE = 9 };
/* 10 and 11 (COOPPKT_EXT_FIRST..LAST in coop.h) are coop_ext.c's features' own packets;
 * 30 (COOPPKT_VOICE, 30..31 kept for it) is voice chat's (coop_voice.c) */

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
    u8 revive;      /* standing: the player we hold Use over to revive, + 1 (0 none); down: ticks of
                       COOP_REVIVE_HOLD the reviver has held (coop_revive.c) */
    u16 calm;       /* ticks since the sender was last in a fight (capped) */
    u8 out;         /* down for good this mission: no lives left, or bled out (COOPOUT_*) */
    u8 pad[3];
    CoopLook look;  /* guns, movement, shots and character (coop_look.c) */
    u32 ms;         /* the sender's clock (coopNowMs) as it sent this: the puppet is drawn by it (coop_interp.c) */
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
    /* host: each player's line to it (coopNetLink), for every lobby's party
     * panel. Added after v0.2.0 without a version change: a shorter hello
     * from an older build is taken with these zero (not measured). */
    u16 linkrtt[COOP_MAX_PLAYERS];  /* round trip, ms */
    u8 linkloss[COOP_MAX_PLAYERS];  /* packet loss, % */
    u8 linkjit[COOP_MAX_PLAYERS];   /* jitter, ms */
} CoopHelloPkt;
#define COOP_HELLO_V020 ((s32) (size_t) &((CoopHelloPkt *) 0)->linkrtt)   /* what a v0.2.0 build sends */

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
    f32 clock;          /* the host's mission countdown (ticks left) */
    u8 clockflags;      /* COOP_CLOCK_* */
    u8 pad[3];
} CoopWorldPkt;

#define COOP_CLOCK_RUNNING 1
#define COOP_CLOCK_SHOWN   2
#define COOP_CLOCK_SET     4    /* the host's level script has used the countdown */

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
        struct { f32 damage; f32 angle; s16 chrnum; u8 friendly; u8 pad; } hit;  /* host -> client; chrnum: the guard that fired, -1 not a guard; friendly: a partner's shot, unscaled */
        struct { s16 chrnum; s16 hitpart; s32 weaponid; f32 vector[3]; u8 predicted; u8 predidx; u8 pad[2]; } shot;  /* client -> host; predicted: COOPPRED_* the shooter's game played, predidx which one */
        struct { s32 doorofs; s32 state; } door;                                /* both ways */
        struct { s32 objofs; f32 damage; f32 pos[3]; s16 item; s16 player; } obj; /* both ways */
        struct { s32 objofs; u8 bounce; u8 children; u8 pad[2]; f32 dir[3]; s32 stage; } knock;  /* to all */
        struct { s32 objofs; s32 kept; s32 stage; } pick;                       /* to all */
        struct { s32 objofs; s32 stage; s16 gadget; s16 pad; } use;             /* joiner to host; gadget: the item used on it (bomb defuser...), 0 a Use press */
        struct { s32 stage; u32 stageflags; } end;                              /* both ways */
        struct { f32 pos[3]; s16 type; u8 rooms[8]; u8 owner; } boom;           /* client -> host, and the host's to all; owner: 1 + the player who set it off, 0 not a player's */
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
#define COOP_PARTNER_SIGHT  6000.0f    /* coopChrNearPartner */
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
enum { GSF_HIDDEN = 1, GSF_FLIP = 2, GSF_LOOP = 4, GSF_FIRE_R = 8, GSF_FIRE_L = 16, GSF_DYING = 32 };

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
    u32 ms;         /* the host's clock as it took the snapshot */
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
static s32 sNoInterp;   /* GE_COOP_NOINTERP=1: draw each packet as it lands (to compare) */
static s32 sNoLagComp;  /* GE_COOP_NOLAGCOMP=1: guards aim at the drawn puppet, every hit counts */
static s32 sCoverSaves; /* joiner: guards' hits dropped behind cover, this run */
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
static u32 sDropInSince;        /* sLateJoin: sTick we started watching for a safe moment, 0 not yet */
static s32 sHostWaitFar;        /* joiner: the host's silence began on a poor line of ours, or after an earlier one */
static s32 sHostHiccups;        /* joiner: times the host went quiet and came back this mission */
static void coopDropInTick(void);
static s32 sOnLobbyScreen;      /* front.c: the lobby screen is up */
static u32 sLastHurtTick;   /* local Bond lost health */
static u32 sLastFightTick;  /* host: a guard was attacking someone */
static f32 sLastHealth;
static s32 sLastWeapon = ITEM_UNARMED;
static u32 sDeadSince;      /* sTick our Bond's death fade finished, 0 alive */
static s32 sReviveHold;     /* while down: ticks a partner has held Use over us */
static u32 sReviveFirst;    /* while down: the tick a partner first held Use over us, 0 none */
static s32 sBledOut;        /* out until the mission ends: COOPOUT_BLEDOUT (revive only, down too long) or COOPOUT_LIVES */
static s32 sLivesLeft = -1; /* this mission's lives left (COOPSET_LIVES), -1 unlimited */
static s32 coopLivesStart(void);
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
    CoopTrack track;    /* its recent positions, by its clock: the puppet is drawn between them */
    coord3d aimPos;     /* host: where this player is about now, for the guards (lag compensation) */
    StandTile *aimStan; /* NULL: use the drawn spot */
    f32 noise[2];       /* host: how loud each of their guns is now (gunTickNoise), for the guards' hearing */
    u8 noiseSeq;        /* the shotseq coopPuppetNoise last saw */
    u8 noiseSeqValid;
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
static CoopClock sClock[COOP_MAX_PLAYERS];  /* each player's clock against ours (coop_interp.c) */

/* a reliable event channel per transport peer (host: a joiner each; joiner: the host) */
typedef struct
{
    CoopEvent out[COOP_EVENT_RING];
    u32 outSentMs[COOP_EVENT_RING]; /* when each was last sent (coopNowMs) */
    u32 outNext;        /* seq the next queued event gets */
    u32 outAcked;       /* the peer has everything up to here */
    u32 inDone;         /* we have applied everything up to here */
    CoopEvent in[COOP_EVENT_RING];  /* events that came ahead of a missing one, by seq */
    u32 inSeq[COOP_EVENT_RING];     /* the seq each slot of in holds, 0 none */
    u32 outSentUpTo;
    u32 evLastSend;
    s32 ackDirty;
    u32 session;        /* the peer's session, 0 until heard */
    CoopHelloPkt hello; /* its last hello */
    u32 helloTick;
    s32 helloSeen;
    u32 heardMs;        /* anything from it, wall clock (coopNowMs) */
    unsigned long long peerId;  /* host: who holds the slot now (coopNetPeerId), 0 for nobody */
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
static PropRecord *sAimSwapped;     /* the puppet moved to its aim position for that tick */
static coord3d sAimSavedPos;
static StandTile *sAimSavedStan;
PropRecord *g_CoopPropOverride;     /* read by getCurrentPlayerProp() */
static ChrRecord *sAltChr;          /* the guard coopChrAltBegin is trying other players for */
static s32 sAltFrom = -1;           /* the player it is on outside those tries */

static s32 sEvForwarding;   /* host: passing a joiner's event on, keep its sender */

/* client guard mirroring */
static u32 sGuardSnap;
static u32 sChrLastSync[COOP_MAX_CHRS]; /* sTick of the last snapshot entry */
static CoopTrack sGuardTrack[COOP_MAX_CHRS];   /* joiner: each mirrored chr's recent positions, by the host's clock */
static s16 sGuardTrackNum[COOP_MAX_CHRS];      /* the chrnum its track is of */
static u32 sGuardPktMs;                         /* the guard packet being applied: its clock */
static u32 sGuardStale;                         /* joiner: guard entries that came after a newer one (counted for the log) */
static u8 sChrHostDying[COOP_MAX_CHRS];         /* the host's says this chr is dying or dead */
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
static void coopSendHelloNow(void);
static void coopApplyExplosion(CoopEvent *ev);
static void coopApplyAiCommand(CoopEvent *ev);
static s32 sPendingCinemaList = -1;
static s32 sHostEndingHere;    /* joiner: we play the host's ending; its exit is the host's to send */
static void coopPendingCinemaTick(void);
static s32 coopLocalCalm(void);
static void coopTrackCombat(void);
static void coopApplyWorld(CoopWorldPkt *p);
static void coopCradleFallTick(void);
static s32 sClockSet;
static s32 coopSetupOfs(void *rec);
static StandTile *coopFindStan(coord3d *pos, StandTile *from, coord3d *frompos);
static s32 coopIsMirroringClient(void);
static void coopSendGuards(void);
static void coopReapGuards(void);
static void coopGuardsInterpTick(void);
static s32 coopPredictHolds(s32 slot, ChrRecord *chr, ModelAnimation *hostanim, s32 hostdying);
static void coopPredictShot(ChrRecord *chr, s32 hitpart, coord3d *vector, s32 weaponid, u8 *kind, u8 *idx);
static void coopPredictReset(void);
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

static s32 sInCinema;   /* the intro or a scripted cinema runs (coop_cutscene.c) */

/* in the stage, playing it or watching its intro or a cutscene: the world
 * (guards, objects, puppets) keeps up with the host's */
static s32 coopInStage(void)
{
    PropRecord *bond = getCurrentPlayerProp();

    return coopInMission() || (sInCinema && bond != NULL && bond->stan != NULL && !coopVersus());
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

/* a puppet, or our own Bond's body (the intro swirl and cutscenes give it a
 * chr slot): never a guard to mirror, send or remove */
static s32 coopIsPlayerChr(ChrRecord *chr)
{
    return coopIsPuppet(chr->prop) || (g_CurrentPlayer != NULL && chr->prop == g_CurrentPlayer->prop);
}

static s32 coopRespawnEta(void);
static s32 coopLocalCalm(void);
static void coopRemotesTick(void);
static void coopCinemaTick(void);
static void coopQueueEventTo(CoopEvent *ev, s32 to);

/* host: STATE to channel c (ours, and the others' it relays) at half rate,
 * 30 a second: with three or four players, or on a poor line. Every player's
 * STATE to every joiner at 60 a second was most of the host's upload (each
 * player added 10 KB/s to every other joiner's line and to the host's), so a
 * third or fourth player raised everyone's ping on a slow upload. Partners
 * are drawn between samples (coop_interp.c), which allows for the longer
 * gap; the newest shots ride in every STATE (COOP_SHOTS_KEPT). Not in versus,
 * which has its own tuning. */
static s32 coopStateThin(s32 c)
{
    CoopLink l;

    if (coopVersus())
    {
        return 0;
    }
    return coopLobbyPlayerCount() >= 3 || (coopNetLink(coopPlayerOfChan(c), &l) && l.quality == COOPLINK_POOR);
}

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
    p.revive = (u8) (p.dead ? ((sDeadSince != 0 && sReviveHold < 255) ? sReviveHold : 0) : coopReviveTarget());
    p.calm = (u16) coopLocalCalm();
    p.out = (u8) sBledOut;
    p.ms = coopNowMs();
    if (coopNetIsHost())
    {
        s32 c;

        for (c = 0; c < COOP_NET_MAX_PEERS; c++)
        {
            if ((sTick & 1) == 0 || !coopStateThin(c))
            {
                coopNetSendTo(c, &p, sizeof(p));
            }
        }
    }
    else
    {
        coopNetSend(&p, sizeof(p));
    }

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
    memset(ch->inSeq, 0, sizeof(ch->inSeq));
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
    {   /* kept by seq: jitter reorders packets, and one lost must not cost the ones after it a resend */
        u32 seq = e->firstseq + k;

        if ((s32) (seq - ch->inDone) > 0 && (s32) (seq - ch->inDone) <= COOP_EVENT_RING)
        {
            ch->in[seq % COOP_EVENT_RING] = e->ev[k];
            ch->inSeq[seq % COOP_EVENT_RING] = seq;
        }
    }
    while (ch->inSeq[(ch->inDone + 1) % COOP_EVENT_RING] == ch->inDone + 1)
    {   /* in order, from the first not yet applied */
        CoopEvent ev;

        ch->inDone++;
        ch->inSeq[ch->inDone % COOP_EVENT_RING] = 0;
        ev = ch->in[ch->inDone % COOP_EVENT_RING];
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
    if (e->count > 0)
    {   /* a bare ack needs none back */
        ch->ackDirty = 1;
    }
}

/* a STATE packet on channel c */
static void coopRecvState(s32 c, CoopStatePkt *p)
{
    CoopRemote *r;
    s32 q;

    if (coopNetIsHost())
    {
        static u8 relayed[COOP_NET_MAX_PEERS][COOP_NET_MAX_PEERS];

        p->player = (u8) coopPlayerOfChan(c);
        for (q = 0; q < COOP_NET_MAX_PEERS; q++)
        {   /* every joiner sees every other one (every other STATE where coopStateThin) */
            if (q != c && ((++relayed[q][c] & 1) != 0 || !coopStateThin(q)))
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
    if (p->stage == bossGetStageNum())
    {   /* every sample counts for the drawing, a late one too (coop_interp.c) */
        CoopSample smp;

        coopClockSample(&sClock[p->player], p->ms, coopNowMs());
        smp.ms = p->ms;
        smp.pos[0] = p->pos[0];
        smp.pos[1] = p->pos[1];
        smp.pos[2] = p->pos[2];
        smp.yaw = p->yaw;
        coopTrackPush(&r->track, &smp);
    }
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

/* a VOICE packet (coop_voice.c): the host stamps who is talking and passes a
 * joiner's on to the other joiners, as a VERSUS packet */
static void coopRecvVoice(s32 c, u8 *buf, s32 n)
{
    s32 q;
    u8 *player = buf + 8;   /* CoopVoicePkt.player, after the magic, version and type */

    if (n < 16)
    {
        return;
    }
    if (coopNetIsHost())
    {
        *player = (u8) coopPlayerOfChan(c);
        for (q = 0; q < COOP_NET_MAX_PEERS; q++)
        {
            if (q != c && coopNetPeerConnected(q))
            {
                coopNetSendTo(q, buf, n);
            }
        }
    }
    coopVoiceReceive(buf, n);
}

/* ---- round trip ------------------------------------------------------------
 * Every half second, each game pings each transport peer with its clock; the
 * answer echoes it, and the difference is a round trip. Wall clock, so a
 * paused game still answers (coopKeepAlive). */

typedef struct
{
    u32 magic;
    u16 version;
    u16 type;       /* COOPPKT_TIME */
    u32 ms;         /* the sender's clock */
    u32 echo;       /* an answer: the ping's ms */
    u8 answer;
    u8 pad[3];
} CoopTimePkt;

static u32 sTimeSent[COOP_NET_MAX_PEERS];
static f32 sRtt[COOP_NET_MAX_PEERS];     /* smoothed, ms; 0 not measured yet */
static f32 sRttDev[COOP_NET_MAX_PEERS];  /* smoothed |round trip - sRtt|: the jitter, ms */
static f32 sLoss[COOP_NET_MAX_PEERS];    /* smoothed share of pings that got no answer, 0..1 */
static s32 sLossN[COOP_NET_MAX_PEERS];   /* pings settled (answered or given up on) */
#define COOP_PINGS_OUT 8
#define COOP_PING_GIVEUP_MS 2000
static u32 sPingOut[COOP_NET_MAX_PEERS][COOP_PINGS_OUT];  /* clocks of pings not answered yet, 0 free */

/* a ping settled: answered, or not in COOP_PING_GIVEUP_MS */
static void coopLossSample(s32 c, s32 lost)
{
    f32 k = sLossN[c] < 10 ? 1.0f / (f32) (sLossN[c] + 1) : 0.1f;   /* an average of the first ten, then the last ~10 */

    sLoss[c] += ((lost ? 1.0f : 0.0f) - sLoss[c]) * k;
    sLossN[c]++;
}

static void coopSendTime(s32 c, s32 answer, u32 echo)
{
    CoopTimePkt t;

    memset(&t, 0, sizeof(t));
    t.magic = COOP_MAGIC;
    t.version = COOP_VERSION;
    t.type = COOPPKT_TIME;
    t.ms = coopNowMs();
    t.echo = echo;
    t.answer = (u8) answer;
    coopNetSendTo(c, &t, sizeof(t));
}

static void coopRecvTime(s32 c, CoopTimePkt *t)
{
    if (!t->answer)
    {
        coopSendTime(c, 1, t->ms);
        return;
    }
    {
        f32 rtt = (f32) (s32) (coopNowMs() - t->echo);
        s32 k;

        for (k = 0; k < COOP_PINGS_OUT && sPingOut[c][k] != t->echo; k++)
        {
        }
        if (k == COOP_PINGS_OUT || t->echo == 0)
        {
            return;     /* a duplicate, or one already given up on */
        }
        sPingOut[c][k] = 0;
        coopLossSample(c, 0);
        if (rtt < 0.0f || rtt > 10000.0f)
        {
            return;
        }
        {
            f32 dev = rtt - sRtt[c];

            dev = (dev < 0.0f) ? -dev : dev;
            sRttDev[c] = (sRtt[c] == 0.0f) ? 0.0f : sRttDev[c] + (dev - sRttDev[c]) * 0.2f;
        }
        sRtt[c] = (sRtt[c] == 0.0f) ? rtt : sRtt[c] + (rtt - sRtt[c]) * 0.2f;
        if (sRtt[c] < 1.0f)
        {
            sRtt[c] = 1.0f;     /* measured: under a millisecond on one machine */
        }
    }
}

/* In a mission, a line that stays poor for 4 s gets a toast (once a minute at
 * most): on a joiner about its own line to the host, on the host about each
 * joiner's. The party panel and the HUD's partner rows show it all along. */
static void coopLinkWarnTick(u32 now)
{
    static u32 poorSince[COOP_MAX_PLAYERS], told[COOP_MAX_PLAYERS];
    char text[80];
    s32 p;

    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        CoopLink l;

        if (p == coopSelf() || (!coopNetIsHost() && p != 0) || !coopNetLink(p, &l) || l.quality != COOPLINK_POOR
            || !coopInMission() || !coopRemoteHere(p))
        {
            poorSince[p] = 0;
            continue;
        }
        if (poorSince[p] == 0)
        {
            poorSince[p] = now;
        }
        if (now - poorSince[p] < 4000 || (told[p] != 0 && now - told[p] < 60000))
        {
            continue;
        }
        told[p] = now;
        /* the host hears about a joiner's line, a joiner about its own: either way it is that pair's */
        snprintf(text, sizeof(text), "%s|slow connection (%d ms, %d%% loss)", coopPlayerName(p), (int) l.rtt, (int) l.loss);
        coopFeedToast(text, p);
        portConLogf("ge: coop: net: player %d's line is poor: %d ms, %d%% loss, %d ms jitter\n", p + 1, (int) l.rtt, (int) l.loss, (int) l.jitter);
    }
}

static void coopTimeTick(void)
{
    u32 now = coopNowMs();
    s32 c;
    static s32 envread;
    static u32 lastLog, lastLinkLog;

    if (!envread)
    {
        const char *e = getenv("GE_COOP_NOINTERP");

        envread = 1;
        sNoInterp = (e != NULL && atoi(e) != 0);
        e = getenv("GE_COOP_NOLAGCOMP");
        sNoLagComp = (e != NULL && atoi(e) != 0);
    }
    for (c = 0; c < COOP_NET_MAX_PEERS; c++)
    {
        s32 k, slot = -1;

        if (!coopNetPeerConnected(c))
        {
            sRtt[c] = sRttDev[c] = sLoss[c] = 0.0f;
            sLossN[c] = 0;
            memset(sPingOut[c], 0, sizeof(sPingOut[c]));
            continue;
        }
        for (k = 0; k < COOP_PINGS_OUT; k++)
        {
            if (sPingOut[c][k] != 0 && now - sPingOut[c][k] > COOP_PING_GIVEUP_MS)
            {
                sPingOut[c][k] = 0;
                coopLossSample(c, 1);
            }
            if (sPingOut[c][k] == 0)
            {
                slot = k;
            }
        }
        if (now - sTimeSent[c] >= 500)
        {
            sTimeSent[c] = now;
            if (slot >= 0 && now != 0)
            {
                sPingOut[c][slot] = now;    /* the answer echoes it (coopRecvTime) */
            }
            coopSendTime(c, 0, 0);
        }
    }
    coopLinkWarnTick(now);
    if (now - lastLinkLog >= 10000 && !coopInMission())
    {   /* in the menus too: the party panel shows these */
        s32 p;

        lastLinkLog = now;
        for (p = 1; p < COOP_MAX_PLAYERS; p++)
        {
            CoopLink l;

            if (coopNetLink(p, &l))
            {
                portConLogf("ge: coop: net: player %d's line to the host %d ms, %d%% loss, %d ms jitter (quality %d)\n", p + 1,
                            (int) l.rtt, (int) l.loss, (int) l.jitter, (int) l.quality);
            }
        }
    }
    if (now - lastLog >= 10000 && coopInMission())
    {
        s32 p;

        lastLog = now;
        if (sGuardStale != 0)
        {
            portConLogf("ge: coop: net: %u guard snapshot entries came after newer ones (positions kept, the rest skipped)\n", sGuardStale);
        }
        for (p = 0; p < COOP_NET_MAX_PEERS; p++)
        {   /* what we send each peer: the busiest packet types */
            unsigned int by[32], pkts, total = coopNetSentTake(p, by, &pkts);
            char line[160];
            s32 k, n = 0;

            if (total == 0 || !coopNetPeerConnected(p))
            {
                continue;
            }
            for (k = 0; k < 32 && n < (s32) sizeof(line) - 16; k++)
            {
                if (by[k] * 20 >= total)
                {
                    n += snprintf(line + n, sizeof(line) - n, " %d:%u", k, (by[k] * 100 + total / 2) / total);
                }
            }
            line[n] = '\0';
            portConLogf("ge: coop: net: to player %d %.1f KB/s in %u packets/s (%% by type:%s)\n", coopPlayerOfChan(p) + 1,
                        total / 10240.0f, pkts / 10, line);   /* this log comes every 10 s */
        }
        for (p = 0; p < COOP_MAX_PLAYERS; p++)
        {
            if (p != coopSelf() && sR[p].valid && sClock[p].valid)
            {
                CoopLink l;

                coopNetLink(p, &l);
                portConLogf("ge: coop: net: player %d round trip %d ms, drawn %d ms behind (send every %.0f ms, jitter %.1f), "
                            "link %d ms %d%% loss %d ms jitter (quality %d)\n", p + 1,
                            coopNetRttMs(p), coopClockDelay(&sClock[p]), sClock[p].interval, sClock[p].jitter,
                            (int) l.rtt, (int) l.loss, (int) l.jitter, (int) l.quality);
            }
        }
    }
}

/* Player p's line to the host: round trip, loss and jitter, and a quality.
 * The host measures each joiner's itself; a joiner measures its own and has
 * the host's word (its hello) for the others. The host's own seat (p 0) is
 * the line from us to it on a joiner, and nothing on the host. */
s32 coopNetLink(s32 p, CoopLink *out)
{
    s32 c;

    memset(out, 0, sizeof(*out));
    if (sEnabled <= 0 || p < 0 || p >= COOP_MAX_PLAYERS)
    {
        return 0;
    }
    if (coopNetIsHost() ? (p != 0) : (p == 0 || p == coopSelf()))
    {
        c = coopNetIsHost() ? coopChanOfPlayer(p) : 0;
        if (c < 0 || c >= COOP_NET_MAX_PEERS || !coopNetPeerConnected(c) || sRtt[c] == 0.0f)
        {
            return 0;
        }
        out->rtt = (s32) (sRtt[c] + 0.5f);
        out->jitter = (s32) (sRttDev[c] + 0.5f);
        /* a ping is lost if either leg is: the one-way share is about 1 - sqrt(1 - both) */
        out->loss = (sLossN[c] >= 4) ? (s32) ((1.0f - sqrtf(1.0f - (sLoss[c] > 0.99f ? 0.99f : sLoss[c]))) * 100.0f + 0.5f) : 0;
    }
    else if (!coopNetIsHost() && sHostHello.linkrtt[p] != 0)
    {
        out->rtt = sHostHello.linkrtt[p];
        out->loss = sHostHello.linkloss[p];
        out->jitter = sHostHello.linkjit[p];
    }
    else
    {
        return 0;
    }
    if (out->rtt >= COOP_LINK_POOR_RTT || out->loss >= COOP_LINK_POOR_LOSS || out->jitter >= COOP_LINK_POOR_JITTER)
    {
        out->quality = COOPLINK_POOR;
    }
    else if (out->rtt >= COOP_LINK_FAIR_RTT || out->loss >= COOP_LINK_FAIR_LOSS || out->jitter >= COOP_LINK_FAIR_JITTER)
    {
        out->quality = COOPLINK_FAIR;
    }
    else
    {
        out->quality = COOPLINK_GOOD;
    }
    return 1;
}

/* the round trip to player p in ms, 0 when not known; a joiner's partner is reached through the host */
s32 coopNetRttMs(s32 p)
{
    s32 c;

    if (sEnabled <= 0 || p < 0 || p >= COOP_MAX_PLAYERS || p == coopSelf())
    {
        return 0;
    }
    if (coopNetIsHost())
    {
        c = coopChanOfPlayer(p);
        return (c >= 0 && c < COOP_NET_MAX_PEERS) ? (s32) (sRtt[c] + 0.5f) : 0;
    }
    /* a joiner only talks to the host: another joiner is two of those away (about) */
    return (s32) (sRtt[0] * (p == 0 ? 1.0f : 2.0f) + 0.5f);
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
            if (coopInStage() && coopRemoteHere(0))
            {
                coopClockSample(&sClock[0], g->ms, coopNowMs());
                sGuardPktMs = g->ms;
                for (k = 0; k < g->count; k++)
                {
                    coopApplyGuard(&g->ent[k]);
                }
                sLastSnapTick = sTick;
            }
            continue;
        }
        if (p->type == COOPPKT_HELLO && n >= COOP_HELLO_V020)
        {
            CoopChan *ch = &sChan[c];

            if (n < (s32) sizeof(CoopHelloPkt))
            {   /* an older build's: no link numbers */
                memset(buf + n, 0, sizeof(CoopHelloPkt) - (size_t) n);
            }
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
            if (coopInStage() && coopRemoteHere(coopPlayerOfChan(c)))
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
        if (p->type == COOPPKT_VOICE)
        {
            coopRecvVoice(c, buf, n);
        }
        if (p->type >= COOPPKT_EXT_FIRST && p->type <= COOPPKT_EXT_LAST)
        {
            coopExtPacket(buf, n, coopPlayerOfChan(c));
        }
        if (p->type == COOPPKT_TIME && n >= (s32) sizeof(CoopTimePkt))
        {
            coopRecvTime(c, (CoopTimePkt *) buf);
            continue;
        }
        if (p->type >= COOPPKT_REPL && p->type <= COOPPKT_REPLREQ)
        {
            coopReplPacket(buf, n, coopPlayerOfChan(c));
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
    r->aimStan = NULL;
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
    CoopSample at;
    f32 yaw = r->st.yaw;

    pos.f[0] = r->st.pos[0];
    pos.f[1] = r->st.pos[1];
    pos.f[2] = r->st.pos[2];
    if (!sNoInterp && coopTrackAt(&r->track, coopClockRenderMs(&sClock[r->st.player], coopNowMs()), &at) >= 0)
    {   /* a moment behind, between two of their samples: smooth however the packets came */
        pos.f[0] = at.pos[0];
        pos.f[1] = at.pos[1];
        pos.f[2] = at.pos[2];
        yaw = at.yaw;
    }
    stan = coopFindStan(&pos, r->puppet->stan, &r->puppet->pos);
    if (stan == NULL)
    {
        return;
    }
    r->puppetPrev = pos;

    r->puppet->stan = stan;
    r->puppet->pos = pos;
    setsuboffset(chr->model, &pos);
    setsubroty(chr->model, yaw);
    sub_GAME_7F01FC10(chr->model, &r->puppet->pos, &r->puppet->pos, &chr->ground);
    chrDetectRooms(chr);

    if (coopCutsceneActive())
    {   /* our Bond's cinema animation, no guns; the MP animations start over after */
        coopCutscenePuppetAnim(chr);
        r->look.anim = 0;
        return;
    }
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
    s32 watched;

    if (d2 < 60.0f * 60.0f)
    {
        r->puppetGhost = 1;
    }
    else if (d2 > 90.0f * 90.0f)
    {
        r->puppetGhost = 0;
    }
    if (coopCutsceneActive())
    {
        r->puppetGhost = 1;     /* in nobody's way */
    }
    if (coopTankDriving(r->st.player))
    {
        r->puppetGhost = 1;     /* inside the tank (coop_tank.c) */
    }
    /* our camera inside their body (both start on one pad, or we watch
     * them while we are down; watching, our own body is not the camera),
     * or a cutscene, which shows only our own character: don't draw it */
    if ((d2 < 45.0f * 45.0f && !coopSpectateHud(&watched)) || coopSpectateTarget() == r->st.player || coopCutsceneActive()
        || coopTankDriving(r->st.player))
    {
        r->puppet->chr->chrflags |= CHRFLAG_HIDDEN;
    }
    else
    {
        r->puppet->chr->chrflags &= ~CHRFLAG_HIDDEN;
    }
}

/* host: a partner's gunfire is heard by the guards as our own is
 * (bondview2.c, chrCheckGuardsHeardSound with gunTickNoise's loudness),
 * with them as Bond, so the guards that hear it come after them. Only
 * their shots used to wake a guard, and only the ones that hit it. */
static void coopPuppetNoise(CoopRemote *r, s32 p)
{
    f32 loudest = 0.0f;
    s32 fired = 0;
    s32 newshot;
    s32 h;

    if (!coopNetIsHost() || coopVersus() || !coopRemoteAliveHere(p))
    {
        r->noise[0] = r->noise[1] = 0.0f;
        r->noiseSeqValid = 0;
        return;
    }
    newshot = r->noiseSeqValid && r->st.look.shotseq != r->noiseSeq;
    r->noiseSeq = r->st.look.shotseq;
    r->noiseSeqValid = 1;
    for (h = GUNRIGHT; h <= GUNLEFT; h++)
    {
        s32 item = r->st.look.weapon[h];
        WeaponStats *ws;
        f32 down, downmax;

        if (item < 0 || item >= ITEM_IDS_MAX)
        {
            item = ITEM_UNARMED;
        }
        ws = get_ptr_item_statistics(item);
        if (ws == NULL)
        {
            continue;
        }
        if (item != ITEM_UNARMED && ((r->st.look.flash & (1 << h)) || (newshot && h == GUNRIGHT)))
        {
            r->noise[h] += ws->NoiseIncreasePerShot;
            if (r->noise[h] > ws->LoudnessMax)
            {
                r->noise[h] = ws->LoudnessMax;
            }
            if (r->noise[h] > loudest)
            {
                loudest = r->noise[h];
            }
            fired = 1;
        }
        down = (ws->NoiseIncreasePerShot * g_GlobalTimerDelta) / (ws->NoiseDecayLinearTime * 60.0f);
        downmax = ((r->noise[h] - ws->LoudnessMin) * g_GlobalTimerDelta) / (ws->NoiseDecayScaledTime * 60.0f);
        r->noise[h] -= (down > downmax) ? down : downmax;
        if (r->noise[h] < ws->LoudnessMin)
        {
            r->noise[h] = ws->LoudnessMin;
        }
    }
    if (fired && g_CoopPropOverride == NULL)
    {
        static u32 logged;

        if (sTick - logged >= 60)
        {
            logged = sTick;
            portConLogf("ge: coop: guards hear player %d's gunfire (loudness %.1f)\n", p + 1, loudest);
        }
        g_CoopPropOverride = r->puppet;
        chrCheckGuardsHeardSound(loudest);
        g_CoopPropOverride = NULL;
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

/* host: a guard a partner may be looking at. The game stops animating guards
 * off the host's screen, and the partners see the frozen pose. */
s32 coopChrNearPartner(ChrRecord *chr)
{
    s32 p;

    if (sEnabled <= 0 || !coopNetIsHost() || chr == NULL || chr->prop == NULL || coopIsPlayerChr(chr))
    {
        return 0;
    }
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        if (coopRemoteAliveHere(p))
        {
            f32 dx = sR[p].puppet->pos.f[0] - chr->prop->pos.f[0];
            f32 dy = sR[p].puppet->pos.f[1] - chr->prop->pos.f[1];
            f32 dz = sR[p].puppet->pos.f[2] - chr->prop->pos.f[2];

            if (dx * dx + dy * dy + dz * dz < COOP_PARTNER_SIGHT * COOP_PARTNER_SIGHT)
            {
                return 1;
            }
        }
    }
    return 0;
}

/* ---- NPCs and players inside each other ----------------------------------
 * Each game moves its own Bond and (on the host) the guards, but places the
 * partners' bodies and (on a joiner) the guards where packets say, without
 * a collision test. A guard heading for a partner's body, or a hostage or
 * Boris following one, can end up inside it on one game or the other, and
 * the stock movement test then blocks every step of both: a move out of a
 * shape crosses its edge. So a player's body and an NPC that already overlap
 * don't block each other's moves until they are apart, and a player who is
 * down (bleeding out, or out until the mission ends) blocks nobody. */
#define COOP_MOVERS 4
static PropRecord *sMovers[COOP_MOVERS];   /* the props being moved or tested from now (chrSetMoving) */

void coopCollideMover(PropRecord *prop, s32 on)
{
    s32 i, free = -1;

    if (sEnabled <= 0 || prop == NULL)
    {
        return;
    }
    for (i = 0; i < COOP_MOVERS; i++)
    {
        if (sMovers[i] == prop)
        {
            if (!on)
            {
                sMovers[i] = NULL;
            }
            return;
        }
        if (sMovers[i] == NULL && free < 0)
        {
            free = i;
        }
    }
    if (on && free >= 0)
    {
        sMovers[free] = prop;
    }
}

/* our own Bond or a partner's body: its player number, else -1 */
static s32 coopBodyPlayer(PropRecord *prop)
{
    if (g_CurrentPlayer != NULL && prop == g_CurrentPlayer->prop)
    {
        return coopSelf();
    }
    return coopPlayerOfProp(prop);
}

static s32 coopBodyDown(PropRecord *prop, s32 p)
{
    if (p == coopSelf())
    {
        return g_CurrentPlayer->bonddead;
    }
    return sR[p].st.dead != 0;
}

/* how far a prop's own collision reaches: a player's radius, a chr's width */
static f32 coopBodyRadius(PropRecord *prop)
{
    if (prop->type == PROP_TYPE_VIEWER)
    {
        f32 r, h, pad;

        bondviewGetCollisionRadius(prop, &r, &h, &pad);
        return r;
    }
    return (prop->type == PROP_TYPE_CHR && prop->chr != NULL) ? prop->chr->chrwidth : 0.0f;
}

/* mover's round footprint already reaches into prop's shape (a chr's is a
 * diamond, |dx| + |dz| < width; a player's a near circle), by more than the
 * sliver a blocked move leaves */
static s32 coopBodiesOverlap(PropRecord *mover, PropRecord *prop)
{
    f32 dx = mover->pos.f[0] - prop->pos.f[0];
    f32 dy = mover->pos.f[1] - prop->pos.f[1];
    f32 dz = mover->pos.f[2] - prop->pos.f[2];
    f32 r = coopBodyRadius(mover);
    f32 gap;

    if (dy > 200.0f || dy < -200.0f)
    {
        return 0;
    }
    if (prop->type == PROP_TYPE_CHR)
    {
        gap = ((dx < 0.0f ? -dx : dx) + (dz < 0.0f ? -dz : dz) - coopBodyRadius(prop)) * 0.70710677f;
    }
    else
    {
        gap = sqrtf(dx * dx + dz * dz) - coopBodyRadius(prop);
    }
    return gap < r - 2.0f;
}

s32 coopCollideSkip(PropRecord *prop)
{
    s32 p, i;

    if (sEnabled <= 0 || prop == NULL || coopVersus() || !coopPartnerHere())
    {
        return 0;
    }
    p = coopBodyPlayer(prop);
    if (p >= 0 && coopBodyDown(prop, p))
    {
        return 1;
    }
    for (i = 0; i < COOP_MOVERS; i++)
    {
        PropRecord *m = sMovers[i];
        PropRecord *npc;

        if (m == NULL || m == prop || (coopBodyPlayer(m) >= 0) == (p >= 0))
        {
            continue;   /* two players (the start-pad ghost handles those) or two NPCs: stock */
        }
        npc = (p >= 0) ? m : prop;
        if (npc->type != PROP_TYPE_CHR || npc->chr == NULL)
        {
            continue;
        }
        if (coopBodiesOverlap(m, prop))
        {
            return 1;
        }
    }
    return 0;
}

s32 coopPropIsGhost(PropRecord *prop)
{
    s32 p = coopPlayerOfProp(prop);

    return p >= 0 && sR[p].puppetGhost;
}

static u32 sWorldHostFlags;
static u32 sWorldOwnFlags;

void coopStageStart(void)
{
    s32 i;

    sPendingCinemaList = -1;
    sHostEndingHere = 0;
    sWorldHostFlags = sWorldOwnFlags = 0;

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
        coopTrackReset(&sGuardTrack[i]);
        sGuardTrackNum[i] = -1;
        sChrLastSync[i] = 0;
        sChrFireSeen[i][0] = sChrFireSeen[i][1] = 0;
        sChrFiring[i] = 0;
    }
    sLastSnapTick = 0;
    memset(sAiSyncHave, 0, sizeof(sAiSyncHave));
    memset(sChrHostDying, 0, sizeof(sChrHostDying));
    coopPredictReset();
    sMissionTicks = 0;
    sHostHiccups = 0;
    sNumPeerHeld = 0;
    memset(sScriptPass, 0, sizeof(sScriptPass));    /* a new mission's scripts */
    sNumTeamPicks = 0;
    sClockSet = 0;
    sLastHurtTick = sLastFightTick = sDeadSince = 0;
    sBledOut = 0;
    sLivesLeft = coopLivesStart();
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
        coopTrackReset(&sR[i].track);
        sR[i].aimStan = NULL;
    }
    coopDifficultyStageStart();
    coopVersusStageStart();
    coopHoldStart();        /* after coopVersusStageStart: versus never holds */
    coopPingStageStart();
    coopAudioStageStart();
    coopItemsStageStart();
    coopReplStageStart();
    coopCutsceneStageStart();
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
    memset(sMovers, 0, sizeof(sMovers));    /* nothing is mid-move between ticks */
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
        coopChatTickStill();    /* a line opened just before still takes its keys (coop_chat.c) */
        if (sHold)
        {
            coopHoldTick(); /* the mission card: everyone loads in first */
            return;
        }
        coopKeepAlive();    /* paused, or a cutscene has the controls: still here */
        return;
    }

    sTick++;
    sInCinema = coopCutsceneTick();     /* before the packets: they ask coopInStage */
    coopPollNet();
    coopPendingCinemaTick();
    coopTimeTick();
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
        if (coopInStage())
        {
            coopCinemaTick();
        }
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
    coopReviveTick((s32) sMissionTicks);   /* hold Use over a downed partner (coop_revive.c) */
    if (!sLateJoin)
    {   /* dropping in: nothing of ours until we arrive (coopDropInTick) */
        coopSendState();
    }
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
    coopReplTick(sMissionTicks);    /* every setup object's state, from the host (coop_repl.c) */

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
        if (!sNoInterp)
        {
            coopGuardsInterpTick();
        }
    }

    coopCradleFallTick();
    coopHarnessTick();

    coopRemotesTick();
}

/* The intro or a scripted cinema runs (coop_cutscene.c): no play, but the
 * session goes on, so the host's actors keep moving on every screen and
 * every player's Bond is in the shot. */
static void coopCinemaTick(void)
{
    coopSendState();
    if (coopNetIsHost() && coopNetHasPeer() && coopPartnerHere())
    {
        if ((sTick % 3) == 0)
        {
            coopSendGuards();
        }
        if ((sTick % 30) == 0)
        {
            coopSendWorld();
        }
        if ((sTick % 60) == 30)
        {
            coopSendAiSync();
        }
    }
    if (coopIsMirroringClient())
    {
        coopReapGuards();
        coopClientGunfire();
    }
    coopRemotesTick();
}

/* ---- lag compensation -------------------------------------------------------
 * Guards against joiners: the host's guards see and shoot at a joiner's
 * puppet, which is drawn a moment in the past (the trip here and the drawing
 * delay). For each guard tick aimed at a joiner, the puppet stands where the
 * joiner is about now instead: their newest sample run on at its speed by its
 * age plus half the round trip, at most COOP_AIM_LEAD_MS, along the floor
 * (coopRemoteAim; swapped in coopChrTickBegin and back in coopChrTickEnd).
 * And on the joiner, a guard's hit that comes in after we got behind cover
 * is dropped: the line from the guard (its newest snapshot) to our Bond, the
 * same floor test chrCanSeeBond uses, is blocked now (coopHitBehindCover).
 * Our own shots need nothing: a joiner already hits what it sees, and the
 * host applies the hit to that guard wherever it has got to. */

#define COOP_AIM_LEAD_MS 400     /* lag compensation: at most this far ahead (a long line: a 500 ms round trip plus the drawing delay) */

static void coopRemoteAim(CoopRemote *r, s32 p)
{
    const CoopTrack *t = &r->track;
    const CoopSample *a, *b;
    s32 lead, span, k;
    coord3d pos;

    r->aimStan = NULL;
    if (!coopNetIsHost() || r->puppet == NULL || t->n < 2 || !sClock[p].valid || sNoLagComp)
    {
        return;
    }
    a = &t->s[t->n - 2];
    b = &t->s[t->n - 1];
    span = (s32) (b->ms - a->ms);
    /* the sender's clock now, about: ours less the offset, plus the half trip the offset took off */
    lead = (s32) (coopNowMs() - (u32) sClock[p].off - b->ms) + coopNetRttMs(p) / 2;
    if (lead < 0)
    {
        lead = 0;
    }
    if (lead > COOP_AIM_LEAD_MS)
    {
        lead = COOP_AIM_LEAD_MS;
    }
    for (k = 0; k < 3; k++)
    {
        pos.f[k] = b->pos[k];
        if (span > 0 && span < 250)
        {
            pos.f[k] += (b->pos[k] - a->pos[k]) * (f32) lead / (f32) span;
        }
    }
    r->aimStan = coopFindStan(&pos, r->puppet->stan, &r->puppet->pos);
    r->aimPos = pos;
    if ((sTick % 300) == 0 && r->aimStan != NULL)
    {
        f32 dx = pos.f[0] - r->puppet->pos.f[0], dz = pos.f[2] - r->puppet->pos.f[2];

        portConLogf("ge: coop: lagcomp: guards aim at player %d %d ms ahead, %.0f units from the drawn puppet\n", p + 1, lead, sqrtf(dx * dx + dz * dz));
    }
}

/* joiner: a guard's hit that the host decided before we reached cover */
static s32 coopHitBehindCover(CoopEvent *ev)
{
    ChrRecord *chr;
    PropRecord *bond = getCurrentPlayerProp();
    StandTile *stan;
    coord3d from;
    s32 slot, clear;
    f32 height;

    if (coopNetIsHost() || sNoLagComp || ev->u.hit.chrnum < 0 || (chr = chrFindByLiteralId(ev->u.hit.chrnum)) == NULL
        || chr->prop == NULL || chr->prop->stan == NULL || bond == NULL || bond->stan == NULL)
    {
        return 0;
    }
    from = chr->prop->pos;
    slot = chr - g_ChrSlots;
    if (slot >= 0 && slot < COOP_MAX_CHRS && sGuardTrack[slot].n > 0 && sGuardTrackNum[slot] == chr->chrnum)
    {   /* where it fired from: its newest snapshot, not where it is drawn */
        const CoopSample *nw = &sGuardTrack[slot].s[sGuardTrack[slot].n - 1];

        from.f[0] = nw->pos[0];
        from.f[1] = nw->pos[1];
        from.f[2] = nw->pos[2];
    }
    stan = coopFindStan(&from, chr->prop->stan, &chr->prop->pos);
    if (stan == NULL)
    {
        return 0;
    }
    height = chr->chrheight - 20.0f;
    clear = stanTestLineUnobstructed(&stan, from.f[0], from.f[2], bond->pos.f[0], bond->pos.f[2],
                                     CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PATHBLOCKER | CDTYPE_AIOPAQUE, height, height, 0.0f, 1.0f)
            && stan == bond->stan;
    if (clear)
    {
        return 0;
    }
    sCoverSaves++;
    portConLogf("ge: coop: lagcomp: guard %d's hit (%.3f) dropped: we are behind cover now\n", ev->u.hit.chrnum, ev->u.hit.damage);
    return 1;
}

/* GE_COOP_MOTIONLOG=1 (tests): how evenly each puppet moves on screen. Every
 * 2 s of movement: the mean step per tick, its coefficient of variation, and
 * the ticks it stood still or jumped (over twice the mean) while moving */
static void coopMotionLog(s32 p, coord3d *pos)
{
    static s32 on = -1;
    static coord3d prev[COOP_MAX_PLAYERS];
    static f32 sum[COOP_MAX_PLAYERS], sum2[COOP_MAX_PLAYERS];
    static s32 n[COOP_MAX_PLAYERS], still[COOP_MAX_PLAYERS], jump[COOP_MAX_PLAYERS];
    static f32 last[COOP_MAX_PLAYERS];
    f32 dx, dz, d;

    if (on < 0)
    {
        const char *e = getenv("GE_COOP_MOTIONLOG");

        on = (e != NULL && atoi(e) != 0);
    }
    if (!on)
    {
        return;
    }
    dx = pos->f[0] - prev[p].f[0];
    dz = pos->f[2] - prev[p].f[2];
    prev[p] = *pos;
    d = sqrtf(dx * dx + dz * dz);
    if (d > 200.0f)
    {
        return;     /* spawned or respawned */
    }
    if (d < 0.01f && last[p] < 0.01f)
    {
        if (n[p] >= 120)
        {
            goto report;
        }
        return;     /* standing */
    }
    last[p] = d;
    n[p]++;
    sum[p] += d;
    sum2[p] += d * d;
    if (d < 0.01f)
    {
        still[p]++;
    }
    if (n[p] > 10 && d > 2.0f * sum[p] / (f32) n[p])
    {
        jump[p]++;
    }
    if (n[p] < 120)
    {
        return;
    }
report:
    {
        f32 mean = sum[p] / (f32) n[p];
        f32 var = sum2[p] / (f32) n[p] - mean * mean;

        portConLogf("ge: coop: motion: player %d step %.2f cv %.2f still %d jumps %d of %d ticks\n", p + 1, mean,
                    mean > 0.0f ? sqrtf(var > 0.0f ? var : 0.0f) / mean : 0.0f, still[p], jump[p], n[p]);
    }
    n[p] = still[p] = jump[p] = 0;
    sum[p] = sum2[p] = 0.0f;
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
            coopRemoteAim(r, p);
            coopPuppetNoise(r, p);
            coopMotionLog(p, &r->puppet->pos);
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
    if (sLateJoin && sMissionTicks > 30 && !g_CurrentPlayer->bonddead)
    {   /* joined a mission under way: in beside a partner, not at the level's start */
        coopDropInTick();
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
    {   /* GE_COOP_TESTDIE=<ticks>: kill our Bond that far into the mission, 900 if not given;
         * GE_COOP_TESTDIEAGAIN=<ticks>: and again that long after each time (harness only) */
        static s32 testdie = -1, again;
        if (testdie == -1)
        {
            const char *e = getenv("GE_COOP_TESTDIE");
            const char *a = getenv("GE_COOP_TESTDIEAGAIN");
            testdie = (e == NULL) ? 0 : (atoi(e) > 1) ? atoi(e) : 900;
            again = (a != NULL) ? atoi(a) : 0;
        }
        if (testdie && sMissionTicks >= (u32) testdie && coopPartnerHere() && !g_CurrentPlayer->bonddead)
        {
            testdie = (again > 0) ? (s32) sMissionTicks + again : 0;
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
    {   /* GE_COOP_TESTFLAG=<hex>:<s>: host sets those stage flags <s> seconds into the mission
         * (harness only: an objective's earlier steps, e.g. Dam's modem 100) */
        static s32 testflag = -2;
        static unsigned int flagbits;
        if (testflag == -2)
        {
            const char *e = getenv("GE_COOP_TESTFLAG");
            testflag = -1;
            if (e != NULL && sscanf(e, "%x:%d", &flagbits, &testflag) != 2)
            {
                testflag = -1;
            }
        }
        if (testflag >= 0 && coopNetIsHost() && coopInMission() && sMissionTicks >= (u32) testflag * 60)
        {
            testflag = -1;
            objectiveregisters1 |= flagbits;
            portConLogf("ge: coop: testflag: stage flags |= %x\n", flagbits);
        }
    }
    {   /* GE_COOP_TESTCLOCK=1: log the mission countdown every 5 s (harness only) */
        static s32 testclock = -1;
        static u32 clocklast;
        if (testclock == -1)
        {
            testclock = getenv("GE_COOP_TESTCLOCK") != NULL;
        }
        if (testclock && coopInMission() && (sMissionTicks < clocklast || sMissionTicks >= clocklast + 300))
        {
            clocklast = sMissionTicks;
            portConLogf("ge: coop: testclock: %s, %s, %.1f s left, mission %u s\n",
                        countdownTimerIsRunning() ? "running" : "stopped",
                        (clock_drawn_flag & 1) ? "hidden" : "shown",
                        countdownTimerGetValue() / 60.0f, sMissionTicks / 60);
        }
    }
    if (coopNetIsHost() && coopInMission() && coopRemoteHere(1) && sR[1].puppet != NULL)
    {   /* GE_COOP_TESTHIT=<s>: <s> seconds in, a small hit on player 2 from the nearest guard that
         * can see their puppet and one from the farthest guard (harness only: lag compensation) */
        static s32 testhit = -2;
        if (testhit == -2)
        {
            const char *e = getenv("GE_COOP_TESTHIT");
            testhit = (e != NULL) ? atoi(e) * 60 : -1;
        }
        if (testhit >= 0 && sMissionTicks == (u32) testhit)
        {
            ChrRecord *nearc = NULL, *farc = NULL;
            f32 nd = 0.0f, fd = 0.0f;
            s32 i, k;

            for (i = 0; i < g_NumChrSlots; i++)
            {
                ChrRecord *c = &g_ChrSlots[i];
                StandTile *st;
                f32 dx, dz, d;

                if (c->model == NULL || c->prop == NULL || c->prop->stan == NULL || coopIsPuppet(c->prop) || c->actiontype == ACT_DEAD || c->actiontype == ACT_DIE)
                {
                    continue;
                }
                dx = c->prop->pos.f[0] - sR[1].puppet->pos.f[0];
                dz = c->prop->pos.f[2] - sR[1].puppet->pos.f[2];
                d = dx * dx + dz * dz;
                st = c->prop->stan;
                if (stanTestLineUnobstructed(&st, c->prop->pos.f[0], c->prop->pos.f[2], sR[1].puppet->pos.f[0], sR[1].puppet->pos.f[2],
                                             CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PATHBLOCKER | CDTYPE_AIOPAQUE, c->chrheight - 20.0f, c->chrheight - 20.0f, 0.0f, 1.0f)
                    && st == sR[1].puppet->stan && (nearc == NULL || d < nd))
                {
                    nearc = c;
                    nd = d;
                }
                if (farc == NULL || d > fd)
                {
                    farc = c;
                    fd = d;
                }
            }
            for (k = 0; k < 2; k++)
            {
                ChrRecord *c = k == 0 ? nearc : farc;
                CoopEvent ev;

                if (c == NULL)
                {
                    portConLogf("ge: coop: testhit: no %s guard\n", k == 0 ? "seeing" : "far");
                    continue;
                }
                memset(&ev, 0, sizeof(ev));
                ev.kind = COOPEV_HIT;
                ev.u.hit.damage = 0.01f;
                ev.u.hit.chrnum = (s16) c->chrnum;
                coopQueueEventTo(&ev, 1);
                portConLogf("ge: coop: testhit: guard %d (%s, %.0f away) hits player 2\n", c->chrnum, k == 0 ? "sees them" : "farthest", sqrtf(k == 0 ? nd : fd));
            }
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
    {   /* GE_COOP_TESTBLAST=<mission ticks>: then, once, our own blast (a launcher round's) at the first partner's feet (harness only) */
        static s32 testblast = -2;
        if (testblast == -2)
        {
            const char *e = getenv("GE_COOP_TESTBLAST");
            testblast = (e != NULL) ? atoi(e) : -1;
        }
        if (testblast >= 0 && sMissionTicks >= testblast)
        {
            s32 p;
            testblast = -1;
            for (p = 0; p < COOP_MAX_PLAYERS; p++)
            {
                if (p != coopSelf() && coopRemoteAliveHere(p))
                {
                    PropRecord *pp = sR[p].puppet;
                    portConLogf("ge: coop: testblast: at player %d\n", p + 1);
                    explosionCreate(NULL, &pp->pos, pp->stan, 2, 0, 0, pp->rooms, 0);
                    break;
                }
            }
        }
    }

    if (coopPartnerHere() && coopInMission())
    {   /* GE_COOP_TESTGOTO=<chrnum>: once, 5 s into the mission, stand where that chr stands;
         * a chr not there yet (spawned later: Facility's Doak) is looked for every second
         * until 90 s (harness only) */
        static s32 testgoto = -2;
        static s32 inmission;
        if (testgoto == -2)
        {
            const char *e = getenv("GE_COOP_TESTGOTO");
            testgoto = (e != NULL) ? atoi(e) : -1;
        }
        if (testgoto >= 0 && ++inmission >= 300 && inmission % 60 == 0 && inmission <= 5400)
        {   /* 1024 and up: the chr running that ai list (Statue's Valentin is 1042) */
            ChrRecord *c = (testgoto < 0x400) ? chrFindByLiteralId(testgoto) : NULL;
            s32 i;

            for (i = 0; testgoto >= 0x400 && c == NULL && i < g_NumChrSlots; i++)
            {
                if (g_ChrSlots[i].model != NULL && g_ChrSlots[i].prop != NULL && g_ChrSlots[i].ailist == ailistFindById(testgoto))
                {
                    c = &g_ChrSlots[i];
                }
            }
            if (c == NULL && (inmission == 300 || inmission == 5400))
            {
                portConLogf("ge: coop: testgoto: no chr %d\n", testgoto);
            }
            if (c != NULL && c->prop != NULL && c->prop->stan != NULL)
            {
                coopRespawnPlayerAt(&c->prop->pos, c->prop->stan, 0.0f, sLastWeapon);
                portConLogf("ge: coop: testgoto -> chr %d at %.0f %.0f %.0f\n", testgoto,
                            c->prop->pos.f[0], c->prop->pos.f[1], c->prop->pos.f[2]);
                testgoto = -1;
            }
        }
    }
    if (coopPartnerHere() && coopInMission())
    {   /* GE_COOP_TESTPAD=<pad>:<s>: <s> seconds into the mission, stand on that
         * (unbound) pad; 330 is the Dam bungee room (harness only) */
        static s32 testpad = -2, padat;
        if (testpad == -2)
        {
            const char *e = getenv("GE_COOP_TESTPAD");
            if (e == NULL || sscanf(e, "%d:%d", &testpad, &padat) != 2)
            {
                testpad = -1;
            }
        }
        if (testpad >= 0 && sMissionTicks >= (u32) padat * 60 && g_CurrentSetup.pads != NULL)
        {   /* once: a slow frame steps the ticks past the exact one */
            PadRecord *pad = &g_CurrentSetup.pads[testpad];
            coord3d pos = pad->pos;

            coopRespawnPlayerAt(&pos, pad->stan, 0.0f, sLastWeapon);
            portConLogf("ge: coop: testpad -> pad %d at %.0f %.0f %.0f\n", testpad, pos.f[0], pos.f[1], pos.f[2]);
            testpad = -1;
        }
    }
    if (coopNetIsHost() && coopPartnerHere() && coopInMission())
    {   /* GE_COOP_TESTFLAG=<hex>:<s>: <s> seconds into the mission, the host sets those
         * stage flags, as a script would (harness only) */
        static s32 testflag = -2;
        static u32 flagbits, flagat;
        if (testflag == -2)
        {
            const char *e = getenv("GE_COOP_TESTFLAG");
            testflag = (e != NULL && sscanf(e, "%x:%u", &flagbits, &flagat) == 2) ? 1 : -1;
        }
        if (testflag == 1 && sMissionTicks >= flagat * 60)
        {
            chrSetStageFlags(NULL, flagbits);
            portConLogf("ge: coop: testflag %08x\n", flagbits);
            testflag = -1;
        }
    }
    if (coopInMission())
    {   /* GE_COOP_TESTHOLD=<item>:<s>: <s> seconds into the mission, switch to that
         * item (1 the fists, holstered; harness only) */
        static s32 testhold = -2, holdat;
        if (testhold == -2)
        {
            const char *e = getenv("GE_COOP_TESTHOLD");
            if (e == NULL || sscanf(e, "%d:%d", &testhold, &holdat) != 2)
            {
                testhold = -1;
            }
        }
        if (testhold >= 0 && sMissionTicks >= (u32) holdat * 60)
        {
            bondinvAddInvItem(testhold);
            gunRequestHandWeaponChange(GUNRIGHT, testhold, 0);
            portConLogf("ge: coop: testhold -> item %d\n", testhold);
            testhold = -1;
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
    if (coopIsMirroringClient() && coopInMission() && !sLateJoin && g_CurrentPlayer->bondhealth > 0.0f)
    {   /* GE_COOP_TESTHUNT=<s>: from <s> s into the mission, stand 3 m in front of the nearest
         * living guard and keep its chest in the sights (GE_COOP_TESTFIRE fires); the next one
         * once the host says it is down, or after 8 s (harness only) */
        static s32 testhunt = -2;
        static s16 target = -1;
        static u32 since, downs, tries;
        PropRecord *bond = g_CurrentPlayer->prop;
        ChrRecord *c = (target >= 0) ? chrFindByLiteralId(target) : NULL;
        s32 slot = (c != NULL) ? (s32) (c - g_ChrSlots) : -1;

        if (testhunt == -2)
        {
            const char *e = getenv("GE_COOP_TESTHUNT");
            testhunt = (e != NULL) ? atoi(e) * 60 : -1;
        }
        if (testhunt >= 0 && (s32) sMissionTicks >= testhunt && bond != NULL)
        {
            if (c != NULL && slot >= 0 && slot < COOP_MAX_CHRS && sChrHostDying[slot])
            {
                downs++;
                portConLogf("ge: coop: testhunt: guard %d down on the host after %.1f s (%u of %u)\n",
                            target, (sTick - since) / 60.0f, downs, tries);
                c = NULL;
            }
            else if (c != NULL && sTick - since > 480)
            {
                portConLogf("ge: coop: testhunt: guard %d still up after 8 s (%u of %u)\n", target, downs, tries);
                c = NULL;
            }
            if (c == NULL || c->prop == NULL)
            {
                f32 best = 0.0f;
                s32 i;

                for (i = 0, c = NULL; i < g_NumChrSlots && i < COOP_MAX_CHRS; i++)
                {
                    ChrRecord *g = &g_ChrSlots[i];
                    f32 dx, dz;

                    if (g->model == NULL || g->prop == NULL || g->prop->stan == NULL || coopIsPlayerChr(g)
                        || sChrHostDying[i] || sChrLastSync[i] == 0 || (g->chrflags & CHRFLAG_HIDDEN)
                        || g->actiontype == ACT_DIE || g->actiontype == ACT_DEAD || g->chrnum == target)
                    {
                        continue;
                    }
                    dx = g->prop->pos.f[0] - bond->pos.f[0];
                    dz = g->prop->pos.f[2] - bond->pos.f[2];
                    if (c == NULL || dx * dx + dz * dz < best)
                    {
                        c = g;
                        best = dx * dx + dz * dz;
                    }
                }
                target = (c != NULL) ? c->chrnum : -1;
                since = sTick;
                if (c != NULL)
                {
                    f32 a = getsubroty(c->model);
                    coord3d to;
                    StandTile *stan;

                    tries++;
                    to.f[0] = c->prop->pos.f[0] + 300.0f * sinf(a);
                    to.f[1] = c->prop->pos.f[1];
                    to.f[2] = c->prop->pos.f[2] + 300.0f * cosf(a);
                    stan = coopFindStan(&to, c->prop->stan, &c->prop->pos);   /* walked from the guard's own floor */
                    if (stan == NULL)
                    {
                        to = c->prop->pos;
                        stan = c->prop->stan;
                    }
                    coopRespawnPlayerAt(&to, stan, 0.0f, sLastWeapon);
                    portConLogf("ge: coop: testhunt: after guard %d at %.0f %.0f %.0f\n", target,
                                c->prop->pos.f[0], c->prop->pos.f[1], c->prop->pos.f[2]);
                }
            }
            if (c != NULL && c->prop != NULL)
            {
                f32 dx = c->prop->pos.f[0] - bond->pos.f[0];
                f32 dy = c->prop->pos.f[1] - 20.0f - bond->pos.f[1];   /* low: auto-aim lifts it onto the body */
                f32 dz = c->prop->pos.f[2] - bond->pos.f[2];
                f32 d = sqrtf(dx * dx + dz * dz);

                g_CurrentPlayer->vv_theta = atan2f(-dx, dz) * 57.29578f;
                if (g_CurrentPlayer->vv_theta < 0.0f)
                {
                    g_CurrentPlayer->vv_theta += 360.0f;
                }
                g_CurrentPlayer->vv_verta = atan2f(dy, (d > 1.0f) ? d : 1.0f) * 57.29578f;
            }
        }
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
    {   /* GE_COOP_TESTSPAWN=<chrnum>: once, at 15 s, spawn a copy of that chr next to Bond;
         * <chrnum>,unarmed leaves it without a gun, like Trevelyan's Statue ambush; <chrnum>,late
         * gives it the gun 2 s later, as a guard's own guard_try_spawning_item does (harness only) */
        static s32 testspawn = -2;
        static s32 testspawnarmed = 1;
        static s32 testspawnlate = -1;
        if (testspawn == -2)
        {
            const char *e = getenv("GE_COOP_TESTSPAWN");
            testspawn = (e != NULL) ? atoi(e) : -1;
            testspawnarmed = (e == NULL || (strstr(e, "unarmed") == NULL && strstr(e, "late") == NULL));
            testspawnlate = (e != NULL && strstr(e, "late") != NULL) ? 0 : -1;
        }
        if (testspawnlate > 0 && sTick >= 1020)
        {
            ChrRecord *late = chrFindByLiteralId(testspawnlate);
            if (late != NULL && late->prop != NULL)
            {
                chrGiveWeapon(late, PROP_CHRKALASH, ITEM_AK47, 0);
                portConLogf("ge: coop: testspawn: chr %d armed late\n", testspawnlate);
            }
            testspawnlate = -1;
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
                if (np != NULL && testspawnarmed)
                {
                    chrGiveWeapon(np->chr, PROP_CHRKALASH, ITEM_AK47, 0);
                }
                if (np != NULL && testspawnlate == 0)
                {
                    testspawnlate = np->chr->chrnum;
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

/* inside a guard's tick: "Bond" is player t from here on, seen and shot at
 * where they are now rather than where they are drawn (lag compensation).
 * Player 0 (our own Bond), or chr NULL, ends any switch. */
static void coopChrUseTarget(ChrRecord *chr, s32 t)
{
    if (sAimSwapped != NULL)
    {
        sAimSwapped->pos = sAimSavedPos;
        sAimSwapped->stan = sAimSavedStan;
        sAimSwapped = NULL;
    }
    sTickChr = NULL;
    g_CoopPropOverride = NULL;
    if (chr == NULL || t == COOPTGT_LOCAL || sR[t].puppet == NULL)
    {
        return;
    }
    sTickChr = chr;
    g_CoopPropOverride = sR[t].puppet;
    if (sR[t].aimStan != NULL)
    {
        sAimSwapped = sR[t].puppet;
        sAimSavedPos = sAimSwapped->pos;
        sAimSavedStan = sAimSwapped->stan;
        sAimSwapped->pos = sR[t].aimPos;
        sAimSwapped->stan = sR[t].aimStan;
    }
}

s32 coopChrTickBegin(ChrRecord *chr)
{
    s32 t;

    sTickChr = NULL;
    g_CoopPropOverride = NULL;

    if (!sEnabled || coopIsPlayerChr(chr))
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
        coopChrUseTarget(chr, t);
    }
    return 1;
}

void coopChrTickEnd(void)
{
    coopChrUseTarget(NULL, COOPTGT_LOCAL);
    sAltChr = NULL;
}

/* host, a guard's script check about "Bond" failed for the player it is on:
 * coopChrAltBegin(chr, k) makes the k-th other living player Bond (1), or
 * says there is none left (0). After the check, coopChrAltKeep makes that
 * player the guard's target (the rest of its script is about them), or
 * coopChrAltEnd puts its own target back. A guard watching the host no
 * longer ignores a joiner walking up to it, and the other way round. */
s32 coopChrAltBegin(ChrRecord *chr, s32 k)
{
    s32 t;

    if (sEnabled <= 0 || !coopNetIsHost() || chr == NULL || chr->prop == NULL || coopIsPlayerChr(chr)
        || !coopPartnerHere() || coopVersus())
    {
        return 0;
    }
    if (k == 0)
    {
        sAltChr = chr;
        sAltFrom = (g_CoopPropOverride != NULL) ? coopPlayerOfProp(g_CoopPropOverride) : COOPTGT_LOCAL;
        if (sAltFrom < 0)
        {   /* a level script's partner check (coopAltBondBegin) is running */
            sAltChr = NULL;
            return 0;
        }
    }
    else if (sAltChr != chr)
    {
        return 0;
    }
    for (t = 0; t < COOPTGT_COUNT; t++)
    {
        if (t != sAltFrom && coopTargetable(t) && k-- == 0)
        {
            coopChrUseTarget(chr, t);
            return 1;
        }
    }
    return 0;
}

void coopChrAltEnd(ChrRecord *chr)
{
    if (sAltChr == chr && sAltFrom >= 0)
    {
        coopChrUseTarget(chr, sAltFrom);
    }
}

void coopChrAltKeep(ChrRecord *chr, s32 stimulus)
{
    CoopChrTarget *st = coopChrState(chr);
    s32 t = (g_CoopPropOverride != NULL) ? coopPlayerOfProp(g_CoopPropOverride) : COOPTGT_LOCAL;

    if (st == NULL || t < 0)
    {
        return;
    }
    if (st->target != t)
    {
        portConLogf("ge: coop: chr %d turns to player %d (its script checked them)\n", chr->chrnum, t + 1);
    }
    st->target = t;
    st->lastupdate60 = g_GlobalTimer;
    if (stimulus)
    {
        st->laststimulus60 = g_GlobalTimer;
    }
    sAltFrom = t;
}

/* host, in a guard's tick: the player that is "Bond" for it, 0 our own
 * (coop_ai.c counts the guards on each player) */
s32 coopChrCurrentTarget(void)
{
    s32 t = (g_CoopPropOverride != NULL) ? coopPlayerOfProp(g_CoopPropOverride) : COOPTGT_LOCAL;

    return (t < 0) ? COOPTGT_LOCAL : t;
}

s32 coopTargetablePlayer(s32 t)
{
    return sEnabled > 0 && t >= 0 && t < COOPTGT_COUNT && coopTargetable(t);
}

/* host: "Bond is aiming at me" (sub_GAME_7F0333F8) for whoever Bond is in
 * this guard's tick. That test reads our own crosshair, so for a partner it
 * is their facing against the chr's body, with the same line of sight. */
s32 coopChrAimedAtByBond(ChrRecord *chr)
{
    s32 p = (g_CoopPropOverride != NULL) ? coopPlayerOfProp(g_CoopPropOverride) : COOPTGT_LOCAL;
    PropRecord *from;
    f32 dx, dz, d, want, diff, half;

    if (p <= COOPTGT_LOCAL || !coopNetIsHost())
    {
        return sub_GAME_7F0333F8(chr);
    }
    if (chr->model == NULL || !coopRemoteAliveHere(p) || sR[p].st.look.weapon[GUNRIGHT] == ITEM_UNARMED
        || !chrlvCurrentPlayerCall7F0B0E24(chr))
    {
        return 0;
    }
    from = g_CoopPropOverride;
    dx = chr->prop->pos.f[0] - from->pos.f[0];
    dz = chr->prop->pos.f[2] - from->pos.f[2];
    d = sqrtf(dx * dx + dz * dz);
    if (d < 1.0f)
    {
        return 1;
    }
    want = atan2f(dx, dz);
    diff = want - sR[p].st.yaw;
    if (!(diff > -20.0f && diff < 20.0f))
    {
        return 0;   /* not a yaw (a bad packet): the wraps below would never end */
    }
    while (diff < 0.0f)
    {   /* no fmodf in the game's math.h */
        diff += 6.2831855f;
    }
    while (diff >= 6.2831855f)
    {
        diff -= 6.2831855f;
    }
    if (diff > 3.1415927f)
    {
        diff = 6.2831855f - diff;
    }
    half = atan2f(getinstsize(chr->model) * 0.8f, d);  /* the body's sphere, as the stock test */
    return diff < half;
}

/* host, "if I'm on screen" (chrai.c): the host's screen, or a partner's. The
 * escorts (Natalya in Bunker 2, Control and the Train, the Control
 * technician) teleport up behind Bond when the check fails, and the host's
 * screen alone let one pop up in front of a joiner who was watching her. A
 * partner sees the chr when it is within their view's half width (their STATE
 * yaw, about 48 degrees, plus the body) and 120 m, with a clear line. */
s32 coopChrOnPartnerScreen(ChrRecord *chr)
{
    PropRecord *saved = g_CoopPropOverride;
    PropRecord *from;
    f32 dx, dz, d, diff;
    s32 p, seen = 0;

    if (sEnabled <= 0 || !coopNetIsHost() || chr == NULL || chr->prop == NULL || chr->model == NULL || coopVersus())
    {
        return 0;
    }
    for (p = 1; p < COOP_MAX_PLAYERS && !seen; p++)
    {
        if (!coopRemoteAliveHere(p))
        {
            continue;
        }
        from = sR[p].puppet;
        dx = chr->prop->pos.f[0] - from->pos.f[0];
        dz = chr->prop->pos.f[2] - from->pos.f[2];
        d = sqrtf(dx * dx + dz * dz);
        if (d > 12000.0f)
        {
            continue;
        }
        if (d > 1.0f)
        {
            diff = fmodf(atan2f(dx, dz) - sR[p].st.yaw, 6.2831855f);
            if (diff < 0.0f)
            {
                diff += 6.2831855f;
            }
            if (diff > 3.1415927f)
            {
                diff = 6.2831855f - diff;
            }
            if (diff > 0.84f + atan2f(getinstsize(chr->model), d))
            {
                continue;
            }
        }
        g_CoopPropOverride = from;
        seen = chrlvCurrentPlayerCall7F0B0E24(chr);
        g_CoopPropOverride = saved;
        if (seen)
        {
            static ChrRecord *logged;

            if (logged != chr)
            {
                logged = chr;
                portConLogf("ge: coop: chr %d is on player %d's screen\n", chr->chrnum, p + 1);
            }
        }
    }
    return seen;
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
    s32 p;

    if (g_CoopPropOverride != NULL)
    {
        p = coopPlayerOfProp(g_CoopPropOverride);

        return p < 0 || sR[p].st.dead;
    }
    if (!g_CurrentPlayer->bonddead || sEnabled <= 0)
    {
        return g_CurrentPlayer->bonddead;
    }
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {   /* a level script's "Bond is dead" stops it for good (Bunker 2's exit): only the whole team down counts */
        if (p != coopSelf() && coopRemoteAliveHere(p))
        {
            return 0;
        }
    }
    return 1;
}

/* bondviewGetPlayerYawRadians for whoever is Bond in this guard's tick: a
 * partner's is the facing their STATE says (the same convention), not ours.
 * "Bond's facing" picks a pad beside or behind Bond (Streets' and Jungle's
 * civilians) and the angle a chr is at from Bond's view. */
f32 coopTargetYaw(void)
{
    if (g_CoopPropOverride != NULL)
    {
        s32 p = coopPlayerOfProp(g_CoopPropOverride);

        if (p >= 0)
        {
            return sR[p].st.yaw;
        }
    }
    return bondviewGetPlayerYawRadians();
}

/* host: one of a partner's bullets, from their gun to where it stopped (the
 * tracer coop_look.c draws). Our own shots flag every guard whose bounds they
 * cross as a near miss (chrTestHit), which the standard guard script answers
 * with "Bond missed me": it startles and attacks. A partner's misses never
 * reach chrTestHit here, so a guard they shot past stood still. The same
 * bounds test along their tracer, and the guard turns to that player. */
void coopPartnerShotNearMiss(ChrRecord *shooter, coord3d *from, coord3d *end)
{
    PropRecord *savedoverride;
    coord3d dir;
    f32 len;
    s32 p, i, h;

    if (sEnabled <= 0 || !coopNetIsHost() || coopVersus() || shooter == NULL || shooter->prop == NULL
        || g_ChrSlots == NULL)
    {
        return;
    }
    p = coopPlayerOfProp(shooter->prop);
    if (p < 0 || !coopRemoteAliveHere(p))
    {
        return;
    }
    dir.f[0] = end->f[0] - from->f[0];
    dir.f[1] = end->f[1] - from->f[1];
    dir.f[2] = end->f[2] - from->f[2];
    len = sqrtf(dir.f[0] * dir.f[0] + dir.f[1] * dir.f[1] + dir.f[2] * dir.f[2]);
    if (!(len >= 1.0f))
    {
        return;
    }
    dir.f[0] /= len;
    dir.f[1] /= len;
    dir.f[2] /= len;
    for (i = 0; i < g_NumChrSlots; i++)
    {
        ChrRecord *chr = &g_ChrSlots[i];
        coord3d centre, off;
        f32 t, size, held, d2;

        if (chr->model == NULL || chr->prop == NULL || chr->actiontype == ACT_DEAD || coopIsPlayerChr(chr)
            || (chr->chrflags & CHRFLAG_HIDDEN))
        {
            continue;
        }
        getsuboffset(chr->model, &centre);
        off.f[0] = centre.f[0] - from->f[0];
        off.f[1] = centre.f[1] - from->f[1];
        off.f[2] = centre.f[2] - from->f[2];
        t = off.f[0] * dir.f[0] + off.f[1] * dir.f[1] + off.f[2] * dir.f[2];
        if (t < 0.0f || t > len)
        {   /* behind the gun, or past where the bullet stopped (a wall) */
            continue;
        }
        held = 0.0f;
        for (h = 0; h < 2; h++)
        {   /* as chrTestHit: the bounds grow by the biggest gun held */
            if (chr->weapons_held[h] != NULL && chr->weapons_held[h]->weapon != NULL
                && chr->weapons_held[h]->weapon->model != NULL)
            {
                f32 gun = chr->model->scale * getinstsize(chr->weapons_held[h]->weapon->model);

                if (gun > held)
                {
                    held = gun;
                }
            }
        }
        size = getinstsize(chr->model) + held;
        d2 = off.f[0] * off.f[0] + off.f[1] * off.f[1] + off.f[2] * off.f[2] - t * t;
        if (d2 >= size * size)
        {
            continue;
        }
        chr->chrflags |= CHRFLAG_NEAR_MISS;
        chr->numclosearghs++;
        savedoverride = g_CoopPropOverride;
        g_CoopPropOverride = shooter->prop;   /* the stimulus is that player's */
        coopChrStimulus(chr);
        g_CoopPropOverride = savedoverride;
        portConLogf("ge: coop: player %d's shot went past chr %d (near miss)\n", p + 1, chr->chrnum);
    }
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
    ev.u.hit.chrnum = (sTickChr != NULL) ? (s16) sTickChr->chrnum : -1;
    coopQueueEventTo(&ev, coopPlayerOfProp(g_CoopPropOverride));
    portConLogf("ge: coop: guard %d hit player %d: %.3f\n", sTickChr ? sTickChr->chrnum : -1,
                coopPlayerOfProp(g_CoopPropOverride) + 1, damage);
    return 1;
}


/* ---- guard mirroring (host sends, client applies) ----------------------- */

/* Not every guard in every snapshot: a body that has finished falling goes
 * once a second, a guard more than COOP_GUARD_FAR from every player every
 * other snapshot (10 a second, still drawn smoothly that far off); the rest,
 * and anyone firing or dying, every time. Late in a mission most chrs are
 * bodies, and the guards were most of what the host sent each joiner (about
 * 23 KB/s of 45 on Dam), which a far-away player's line felt first. Joiners
 * free a chr missing from the snapshots for COOP_GUARD_GONE ticks. */
#define COOP_GUARD_FAR   6000.0f
#define COOP_GUARD_GONE  300

static s32 coopGuardDue(ChrRecord *chr, s32 i, s32 firing)
{
    f32 best = -1.0f;
    s32 p;

    if (firing || chr->actiontype == ACT_DIE)
    {
        return 1;
    }
    if (chr->actiontype == ACT_DEAD)
    {
        return ((sGuardSnap + i) % 20) == 0;
    }
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {   /* the nearest player, us included (a spectating joiner watches one of us) */
        const f32 *at = (p != coopSelf()) ? (sR[p].valid ? sR[p].st.pos : NULL)
                        : (g_CurrentPlayer->prop != NULL) ? g_CurrentPlayer->prop->pos.f : NULL;
        f32 dx, dz;

        if (at == NULL)
        {
            continue;
        }
        dx = chr->prop->pos.f[0] - at[0];
        dz = chr->prop->pos.f[2] - at[2];
        if (best < 0.0f || dx * dx + dz * dz < best)
        {
            best = dx * dx + dz * dz;
        }
    }
    return best < COOP_GUARD_FAR * COOP_GUARD_FAR || ((sGuardSnap + i) & 1) == 0;
}

static void coopSendGuards(void)
{
    CoopGuardsPkt pkt;
    s32 i;
    s32 index = 0;
    s32 hdr = (s32) ((u8 *) &pkt.ent[0] - (u8 *) &pkt);
    s32 sent = 0;

    sGuardSnap++;
    memset(&pkt, 0, hdr);
    pkt.magic = COOP_MAGIC;
    pkt.version = COOP_VERSION;
    pkt.type = COOPPKT_GUARDS;
    pkt.snap = sGuardSnap;
    pkt.ms = coopNowMs();

    for (i = 0; i < g_NumChrSlots; i++)
    {
        ChrRecord *chr = &g_ChrSlots[i];
        CoopGuardEnt *e;
        ModelAnimation *anim;

        u8 fire = 0;

        if (chr->model == NULL || chr->prop == NULL || coopIsPlayerChr(chr))
        {
            continue;
        }
        if (i < COOP_MAX_CHRS)
        {   /* firecount ticks on every fire tick, shot or not */
            if (chr->firecount[GUNRIGHT] != sChrFireSeen[i][GUNRIGHT])
            {
                fire |= GSF_FIRE_R;
            }
            if (chr->firecount[GUNLEFT] != sChrFireSeen[i][GUNLEFT])
            {
                fire |= GSF_FIRE_L;
            }
            sChrFireSeen[i][GUNRIGHT] = chr->firecount[GUNRIGHT];
            sChrFireSeen[i][GUNLEFT] = chr->firecount[GUNLEFT];
        }
        if (!coopGuardDue(chr, i, fire != 0 || (i < COOP_MAX_CHRS && sChrFiring[i] != 0)))
        {
            continue;
        }
        if (i < COOP_MAX_CHRS)
        {
            sChrFiring[i] = fire;   /* host: whether it fired since the last snapshot (a stop goes out too) */
        }

        if (pkt.count == 0)
        {
            pkt.first = (u16) index;
        }
        e = &pkt.ent[pkt.count++];
        index++;
        sent++;

        /* pkt is on the stack: an unarmed chr would otherwise send junk as its gun */
        memset(e, 0, sizeof(*e));
        e->chrnum = chr->chrnum;
        e->flags = fire;
        e->body = (u8) chr->bodynum;
        e->head = (u8) chr->headnum;
        {
            PropRecord *gun = chrGetEquippedWeaponProp(chr, GUNRIGHT);
            if (gun != NULL && gun->type == PROP_TYPE_WEAPON && gun->weapon != NULL)
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
        if (chr->actiontype == ACT_DIE || chr->actiontype == ACT_DEAD)
        {
            e->flags |= GSF_DYING;
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
    /* an out-of-range item or model would put some other prop in its hand */
    if (e->gunitem > 0 && e->gunitem < ITEM_IDS_MAX && e->gunmodel >= 0 && e->gunmodel < PROP_MAX)
    {
        chrGiveWeapon(chr, e->gunmodel, e->gunitem, 0);
    }
    portConLogf("ge: coop: mirrored host chr %d (body %d, gun %d) at %.0f %.0f %.0f\n",
        e->chrnum, e->body, e->gunitem, pos.f[0], pos.f[1], pos.f[2]);
    return chr;
}

/* Client: a mirrored guard the host says is dying (or our shot's predicted death).
 * Its actions never tick here, so only the actiontype changes: chrUpdateCollisionBounds
 * lets Bond walk over a dying or dead chr, and auto-aim skips it. */
static void coopMirrorDying(ChrRecord *chr)
{
    chr->actiontype = ACT_DIE;
    chr->act_die.timeextra = 0.0f;
    chr->act_die.elapseextra = 0.0f;
    portConLogf("ge: coop: guard %d is dying here: its body no longer blocks\n", chr->chrnum);
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
    if (chr == NULL || chr->prop == NULL || coopIsPlayerChr(chr))
    {
        return;
    }
    prop = chr->prop;
    slot = chr - g_ChrSlots;
    if (slot >= 0 && slot < COOP_MAX_CHRS && sGuardTrackNum[slot] == chr->chrnum && sGuardTrack[slot].n > 0
        && (s32) (sGuardTrack[slot].s[sGuardTrack[slot].n - 1].ms - sGuardPktMs) > 0)
    {   /* an older snapshot than one we applied (jitter reorders them): its spot goes in the
         * track, but its animation, flags and firing would take the guard back in time */
        CoopSample smp;

        smp.ms = sGuardPktMs;
        smp.pos[0] = e->pos[0];
        smp.pos[1] = e->pos[1];
        smp.pos[2] = e->pos[2];
        smp.yaw = e->yaw;
        coopTrackPush(&sGuardTrack[slot], &smp);
        sChrLastSync[slot] = sTick;
        sGuardStale++;
        return;
    }
    if ((e->flags & GSF_DYING) && chr->actiontype != ACT_DIE && chr->actiontype != ACT_DEAD)
    {
        coopMirrorDying(chr);   /* the body stops blocking, as the host's does */
    }
    if (slot >= 0 && slot < COOP_MAX_CHRS)
    {
        sChrHostDying[slot] = (e->flags & GSF_DYING) != 0;
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
    if (e->gunitem > 0 && e->gunitem < ITEM_IDS_MAX && e->gunmodel >= 0 && e->gunmodel < PROP_MAX
        && !(e->flags & GSF_DYING) && chrGetEquippedWeaponProp(chr, GUNRIGHT) == NULL)
    {   /* a gun the host's guard got after we first saw it (its own list's
         * guard_try_spawning_item, as Statue's ambush and Alec's men do):
         * our copy never runs that, so it fired with nothing in its hand */
        chrGiveWeapon(chr, e->gunmodel, e->gunitem, 0);
        portConLogf("ge: coop: host chr %d drew gun %d\n", e->chrnum, e->gunitem);
    }

    pos.f[0] = e->pos[0];
    pos.f[1] = e->pos[1];
    pos.f[2] = e->pos[2];
    if (slot >= 0 && slot < COOP_MAX_CHRS && !sNoInterp)
    {   /* drawn between samples each tick (coopGuardsInterpTick); placed here only the first time */
        CoopSample smp;
        s32 first = sGuardTrackNum[slot] != chr->chrnum || sGuardTrack[slot].n == 0;

        if (sGuardTrackNum[slot] != chr->chrnum)
        {
            coopTrackReset(&sGuardTrack[slot]);
            sGuardTrackNum[slot] = chr->chrnum;
        }
        smp.ms = sGuardPktMs;
        smp.pos[0] = e->pos[0];
        smp.pos[1] = e->pos[1];
        smp.pos[2] = e->pos[2];
        smp.yaw = e->yaw;
        coopTrackPush(&sGuardTrack[slot], &smp);
        if (!first)
        {
            goto placed;
        }
    }
    stan = coopFindStan(&pos, prop->stan, &prop->pos);
    if (stan != NULL)
    {
        prop->stan = stan;
        prop->pos = pos;
        setsuboffset(chr->model, &pos);
        chrDetectRooms(chr);
    }
    setsubroty(chr->model, e->yaw);
placed:
    chr->chrflags |= CHRFLAG_INIT;

    if (e->animoff >= 0 && slot >= 0 && slot < COOP_MAX_CHRS
        && coopPredictHolds(slot, chr, (ModelAnimation *) ((u8 *) ptr_animation_table + e->animoff), (e->flags & GSF_DYING) != 0))
    {
        return;     /* our own shot's reaction is playing; the host's catches up (prediction section) */
    }
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

/* Client: each mirrored chr where the host's was a moment ago, between two
 * snapshots (coop_interp.c), so guards move smoothly between packets */
static void coopGuardsInterpTick(void)
{
    u32 rt = coopClockRenderMs(&sClock[0], coopNowMs());
    s32 i;

    for (i = 0; i < g_NumChrSlots && i < COOP_MAX_CHRS; i++)
    {
        ChrRecord *chr = &g_ChrSlots[i];
        CoopSample at;
        coord3d pos;
        StandTile *stan;

        if (sGuardTrack[i].n == 0 || chr->model == NULL || chr->prop == NULL || chr->chrnum != sGuardTrackNum[i]
            || coopIsPuppet(chr->prop) || coopTrackAt(&sGuardTrack[i], rt, &at) < 0)
        {
            continue;
        }
        pos.f[0] = at.pos[0];
        pos.f[1] = at.pos[1];
        pos.f[2] = at.pos[2];
        stan = coopFindStan(&pos, chr->prop->stan, &chr->prop->pos);
        if (stan != NULL)
        {
            chr->prop->stan = stan;
            chr->prop->pos = pos;
            setsuboffset(chr->model, &pos);
            chrDetectRooms(chr);
        }
        setsubroty(chr->model, at.yaw);
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

        if (chr->model == NULL || chr->prop == NULL || coopIsPlayerChr(chr))
        {
            continue;
        }
        if (sChrLastSync[i] == 0)
        {
            sChrLastSync[i] = sTick; /* start the clock when we first see it */
        }
        else if (sTick - sChrLastSync[i] > COOP_GUARD_GONE && !(chr->hidden & CHRHIDDEN_REMOVE))
        {
            portConLogf("ge: coop: chr %d gone on the host, removing\n", chr->chrnum);
            chr->hidden |= CHRHIDDEN_REMOVE;
        }
    }
}

/* ---- prediction: our own shots on the host's guards ---------------------------
 * A joiner's shot on a guard is the host's to apply, so the guard used to
 * react a round trip (and the drawing delay) later. Now the shooter's game
 * plays the reaction at once: the yelp, a flinch animation, or, when the hit
 * is lethal by the guard's health in the last AI sync less what we have taken
 * since, a death animation. The shot tells the host which animation we chose
 * (CoopEvent shot.predicted / predidx), and the host's triggered_on_shot_hit
 * uses the same one when its own roll lands on the same kind of reaction
 * (coopReactionPick), so the snapshots that follow show the same animation.
 * While a prediction holds, snapshots don't change that guard's animation;
 * a death the host confirms (GSF_DYING) keeps ours to the end. One the host
 * did not confirm in time (the guard had more health than we knew, another
 * player's shot, a deferred reaction) is dropped and the host's animation
 * comes back: "predict: guard N survived". */

enum { COOPPRED_NONE, COOPPRED_FLINCH, COOPPRED_DEATH };

static struct
{
    s16 chrnum;
    u8 kind;        /* COOPPRED_* */
    u8 confirmed;
    u32 untilMs;
    ModelAnimation *anim;
    f32 damage;     /* what we took off since the last AI sync */
    u32 syncSeen;   /* sAiSyncGot when damage was last reset */
    s32 prevact;    /* a predicted death: the actiontype before it (ACT_DIE drops the body's collision), -1 none */
} sPred[COOP_MAX_CHRS];
static u32 sAiSyncGot[COOP_MAX_CHRS];  /* AI syncs received per slot */
static s32 sPredDeaths, sPredConfirmed, sPredWrong;
static ChrRecord *sForceChr;    /* host: applying a shot whose shooter predicted ... */
static u8 sForceKind, sForceIdx;

static void coopPredictReset(void)
{
    memset(sPred, 0, sizeof(sPred));
    memset(sAiSyncGot, 0, sizeof(sAiSyncGot));
}

static s32 coopPredictWindowMs(void)
{
    s32 w = coopNetRttMs(0) + coopClockDelay(&sClock[0]) + 200;

    return w < 300 ? 300 : (w > 1200 ? 1200 : w);
}

/* host's triggered_on_shot_hit (chraction.c): its roll for a death or flinch
 * animation, or the one the shooter's game already played */
s32 coopReactionPick(ChrRecord *self, s32 death, s32 rolled, s32 count)
{
    if (self == sForceChr && sForceKind == (death ? COOPPRED_DEATH : COOPPRED_FLINCH) && sForceIdx < count)
    {
        return sForceIdx;
    }
    return rolled;
}

/* host: the shooter predicted a reaction for this chr: skip the rare variants it can't have known */
s32 coopReactionForced(ChrRecord *self)
{
    return self == sForceChr && sForceKind != COOPPRED_NONE;
}

static struct ChrHitReaction *coopHitReaction(s32 hitpart)
{
    s32 i;

    for (i = 0; g_HitReactionTable[i].hitpart != -1; i++)
    {
        if (g_HitReactionTable[i].hitpart == hitpart)
        {
            return &g_HitReactionTable[i];
        }
    }
    return &g_HitReactionTable[0];
}

static void coopPredictShot(ChrRecord *chr, s32 hitpart, coord3d *vector, s32 weaponid, u8 *kind, u8 *idx)
{
    s32 slot = chr - g_ChrSlots;
    struct ChrHitReaction *hr;
    struct StruckAnim *sa;
    f32 dmg;
    s32 lethal = 0, n;
    static s32 off = -1;

    *kind = COOPPRED_NONE;
    *idx = 0;
    if (off < 0)
    {
        const char *e = getenv("GE_COOP_NOPREDICT");

        off = (e != NULL && atoi(e) != 0);
    }
    if (off || slot < 0 || slot >= COOP_MAX_CHRS || hitpart == HIT_GUN || hitpart == HIT_HAT || weaponid == ITEM_FIST
        || chr->model == NULL || sChrHostDying[slot] || (sPred[slot].chrnum == chr->chrnum && sPred[slot].kind == COOPPRED_DEATH))
    {
        return;
    }
    if (sPred[slot].chrnum != chr->chrnum || sPred[slot].syncSeen != sAiSyncGot[slot])
    {   /* a fresh AI sync has what the host applied so far */
        sPred[slot].chrnum = chr->chrnum;
        sPred[slot].damage = 0.0f;
        sPred[slot].syncSeen = sAiSyncGot[slot];
        sPred[slot].kind = COOPPRED_NONE;
        sPred[slot].confirmed = 0;
    }
    /* the host's arithmetic (handles_shot_actors) */
    dmg = gunItemGetDestructionAmount(weaponid) * g_AiHealthModifier;
    if (hitpart == HIT_GENERAL || hitpart == HIT_GENERALHALF)
    {
        dmg *= (hitpart == HIT_GENERAL) ? 0.5f : 0.25f;
        hitpart = HIT_CHEST;
    }
    if (hitpart == HIT_HAT && chr->handle_positiondata_hat != NULL && get_hat_model(chr->handle_positiondata_hat) == HATTYPE_MOON)
    {
        hitpart = HIT_HEAD;     /* as handles_shot_actors: the moon hat counts as the head */
    }
    if (hitpart == HIT_HEAD)
    {
        dmg *= 4.0f;
    }
    else if (hitpart == HIT_CHEST)
    {
        dmg *= 2.0f;
    }
    else if (hitpart == HIT_GUN || hitpart == HIT_HAT)
    {
        dmg = 0.0f;             /* the host's does no damage for these: predicting a death there was wrong */
    }
    if (weaponid == ITEM_FIST)
    {
        dmg = 0.0f;             /* the host scales a punch by the angle and the guard's action: never predict a death from one */
    }
    sPred[slot].damage += dmg;
    if (sAiSyncHave[slot] && sAiSync[slot].chrnum == chr->chrnum)
    {   /* the host's numbers, a second old at most, and ours since */
        lethal = !(sAiSync[slot].chrflags & CHRFLAG_INVINCIBLE) && sAiSync[slot].damage + sPred[slot].damage >= sAiSync[slot].maxdamage;
    }
    else
    {   /* no AI sync yet: our copy's setup health, as the host's difficulty scales it */
        lethal = !(chr->chrflags & CHRFLAG_INVINCIBLE) && sPred[slot].damage >= chr->maxdamage * coopDifficultyGuardHealthMul();
    }
    play_sound_for_shot_actor(chr);
    hr = coopHitReaction(hitpart);
    if (lethal && hr->deathAnims != NULL && hr->deathAnimCount > 0)
    {
        *kind = COOPPRED_DEATH;
        n = hr->deathAnimCount;
        *idx = (u8) (randomGetNext() % (u32) n);
        sa = &hr->deathAnims[*idx];
        sPredDeaths++;
    }
    else if (!lethal && hr->flinchAnims != NULL && hr->flinchAnimCount > 0)
    {
        if (hr == &g_HitReactionTable[9] && chrGetEquippedWeaponProp(chr, GUNLEFT) != NULL)
        {
            hr = &g_HitReactionTable[10];
        }
        else if (hr == &g_HitReactionTable[12] && chrGetEquippedWeaponProp(chr, GUNRIGHT) != NULL)
        {
            hr = &g_HitReactionTable[13];
        }
        *kind = COOPPRED_FLINCH;
        n = hr->flinchAnimCount;
        *idx = (u8) (randomGetNext() % (u32) n);
        sa = &hr->flinchAnims[*idx];
    }
    else
    {
        return;
    }
    modelSetAnimationWithMerge(chr->model, sa->struck_anim, sa->flip, 0.0f, sa->speed, 16.0f, TRUE);
    if (sa->endframe >= 0.0f)
    {
        modelSetAnimEndFrame(chr->model, sa->endframe);
    }
    sPred[slot].kind = *kind;
    sPred[slot].confirmed = 0;
    sPred[slot].anim = sa->struck_anim;
    sPred[slot].prevact = -1;
    if (*kind == COOPPRED_DEATH && chr->actiontype != ACT_DIE && chr->actiontype != ACT_DEAD)
    {   /* a joiner never ticks the host's guards' actions: the body would block us until it fades */
        sPred[slot].prevact = chr->actiontype;
        coopMirrorDying(chr);
    }
    sPred[slot].untilMs = coopNowMs() + (u32) coopPredictWindowMs() * (*kind == COOPPRED_DEATH ? 2 : 1);
    portConLogf("ge: coop: predict: guard %d %s (anim %d, damage %.2f, %s)\n", chr->chrnum, *kind == COOPPRED_DEATH ? "dies" : "flinches",
                *idx, sPred[slot].damage, sAiSyncHave[slot] ? "after the AI sync" : "setup health");
    (void) vector;
}

/* a snapshot for a guard we predicted for: 1 to leave its animation alone */
static s32 coopPredictHolds(s32 slot, ChrRecord *chr, ModelAnimation *hostanim, s32 hostdying)
{
    if (sPred[slot].chrnum != chr->chrnum || sPred[slot].kind == COOPPRED_NONE)
    {
        return 0;
    }
    if (sPred[slot].kind == COOPPRED_DEATH)
    {
        if (hostdying)
        {
            if (!sPred[slot].confirmed)
            {
                sPred[slot].confirmed = 1;
                sPredConfirmed++;
                portConLogf("ge: coop: predict: guard %d death confirmed (%s animation)\n", chr->chrnum,
                            hostanim == sPred[slot].anim ? "same" : "other");
            }
            if (hostanim == sPred[slot].anim)
            {
                return 1;   /* ours plays to the end */
            }
            sPred[slot].kind = COOPPRED_NONE;   /* the host rolled another: show its */
            return 0;
        }
        if ((s32) (coopNowMs() - sPred[slot].untilMs) < 0)
        {
            return 1;
        }
        sPredWrong++;
        portConLogf("ge: coop: predict: guard %d survived, back to the host's animation\n", chr->chrnum);
        sPred[slot].kind = COOPPRED_NONE;
        if (sPred[slot].prevact >= 0 && chr->actiontype == ACT_DIE)
        {
            chr->actiontype = sPred[slot].prevact;
        }
        sPred[slot].prevact = -1;
        return 0;
    }
    /* a flinch: until the host's reaction has had time to come, or it shows the same one */
    if (hostanim == sPred[slot].anim || hostdying || (s32) (coopNowMs() - sPred[slot].untilMs) >= 0)
    {
        sPred[slot].kind = COOPPRED_NONE;
        return hostanim == sPred[slot].anim;
    }
    return 1;
}

s32 coopPredictStats(s32 *confirmed, s32 *wrong)
{
    *confirmed = sPredConfirmed;
    *wrong = sPredWrong;
    return sPredDeaths;
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
    coopPredictShot(chr, hitpart, vector, weaponid, &ev.u.shot.predicted, &ev.u.shot.predidx);
    coopQueueEventTo(&ev, 0);
    portConLogf("ge: coop: shot guard %d (part %d, weapon %d), sent to host\n", chr->chrnum, hitpart, weaponid);
    return 1;
}

/* A shot on a partner's puppet never touches the puppet: it is that player's
 * Bond, and a local death frees its model under the puppet code (the crash
 * in setsuboffset). With friendly fire on, the hit goes to that player's own
 * game, scaled as a guard's hit on Bond; that game applies the host's Friendly
 * Fire level to it (coopApplyEvent), so the host's setting decides even when
 * the two copies disagree for a moment. Returns 1 for any puppet. */
s32 coopShotPuppet(ChrRecord *chr, s32 hitpart, coord3d *vector, s32 weaponid, s32 isPlayer)
{
    CoopEvent ev;
    s32 p;
    f32 damage;

    if (!sEnabled || chr == NULL || chr->prop == NULL || (p = coopPlayerOfProp(chr->prop)) < 0)
    {
        return 0;
    }
    if (!isPlayer || coopFriendlyFireScale() <= 0.0f || !coopRemoteAliveHere(p) || !coopInMission())
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
    ev.u.hit.chrnum = -1;   /* a partner's shot: theirs to aim */
    ev.u.hit.friendly = 1;  /* their game scales it by the host's Friendly Fire, or drops it */
    coopQueueEventTo(&ev, p);
    coopStatsNote(COOPSTAT_FRIENDLY);
    portConLogf("ge: coop: friendly fire on player %d (part %d, weapon %d): %.3f before the level's scale\n", p + 1, hitpart, weaponid, damage);
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
    sForceChr = chr;                          /* the reaction their game already played, if it can be the same */
    sForceKind = ev->u.shot.predicted;
    sForceIdx = ev->u.shot.predidx;
    handles_shot_actors(chr, ev->u.shot.hitpart, &vec, ev->u.shot.weaponid, TRUE);
    sForceChr = NULL;
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

/* New events go out at once, once. Unacked ones are sent again when the
 * oldest has waited a round trip and a bit (coopEventResendMs): sending the
 * whole unacked window every few ticks, and with every ack, sent each event
 * five or six times on a 400 ms line and flooded it. The receiver keeps
 * events that arrive ahead of a missing one, so a loss costs one resend.
 * A busy moment goes as up to COOP_EVENT_BURST packets at once. */
#define COOP_EVENT_BURST 4

static u32 coopEventResendMs(s32 c)
{
    u32 ms = (u32) (sRtt[c] * 1.25f + sRttDev[c] * 2.0f) + 50;

    if (sRtt[c] <= 0.0f)
    {
        ms = 250;   /* not measured yet */
    }
    return (ms < 80) ? 80 : (ms > 1200) ? 1200 : ms;
}

static void coopFlushChan(s32 c)
{
    CoopChan *ch = &sChan[c];
    CoopEventsPkt pkt;
    s32 hdr = (s32) ((u8 *) &pkt.ev[0] - (u8 *) &pkt);
    u32 now = coopNowMs();
    u32 seq;
    s32 burst;
    s32 pending = (s32) (ch->outNext - 1 - ch->outAcked);
    s32 fresh = (s32) (ch->outNext - 1 - ch->outSentUpTo) > 0;
    s32 resend = pending > 0 && (s32) (ch->outSentUpTo - ch->outAcked) > 0
                 && now - ch->outSentMs[(ch->outAcked + 1) % COOP_EVENT_RING] >= coopEventResendMs(c);

    if (!coopNetPeerConnected(c) || (!ch->ackDirty && !fresh && !resend))
    {
        return;
    }

    if ((s32) (ch->outSentUpTo - ch->outAcked) < 0)
    {
        ch->outSentUpTo = ch->outAcked;     /* a reset channel */
    }
    seq = resend ? ch->outAcked + 1 : fresh ? ch->outSentUpTo + 1 : ch->outNext;
    for (burst = 0; burst < COOP_EVENT_BURST && (burst == 0 || seq < ch->outNext); burst++)
    {
        memset(&pkt, 0, hdr);
        pkt.magic = COOP_MAGIC;
        pkt.version = COOP_VERSION;
        pkt.type = COOPPKT_EVENTS;
        pkt.ack = ch->inDone;
        pkt.session = coopSelfSession();
        pkt.firstseq = seq;
        for (; seq < ch->outNext && pkt.count < COOP_EVENTS_PER_PKT; seq++)
        {
            pkt.ev[pkt.count++] = ch->out[seq % COOP_EVENT_RING];
            ch->outSentMs[seq % COOP_EVENT_RING] = now;
        }
        coopNetSendTo(c, &pkt, hdr + (s32) (pkt.count * sizeof(CoopEvent)));
    }
    if ((s32) (seq - 1 - ch->outSentUpTo) > 0)
    {
        ch->outSentUpTo = seq - 1;
    }
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
            if (coopInMission() && !g_CurrentPlayer->bonddead && !coopHitBehindCover(ev))
            {
                f32 damage = ev->u.hit.damage;

                if (ev->u.hit.friendly)
                {   /* a partner's shot: the host's Friendly Fire decides, here on the victim's game */
                    damage *= coopFriendlyFireScale();
                    if (damage <= 0.0f)
                    {
                        portConLogf("ge: coop: friendly fire from player %d dropped (Friendly Fire Off)\n", ev->from + 1);
                        break;
                    }
                    portConLogf("ge: coop: friendly fire from player %d: %.3f (%s)\n", ev->from + 1, damage,
                                coopFriendlyFireName(coopSetting(COOPSET_FRIENDLYFIRE)));
                }
                else
                {
                    portConLogf("ge: coop: hit from player %d: %.3f\n", ev->from + 1, damage);
                }
                bondviewCallRecordDamageKills(damage, ev->u.hit.angle, -1, 1);
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
            if (ev->kind >= COOPEV_DESIGN_FIRST && ev->kind <= COOPEV_DESIGN_LAST)
            {
                coopDesignApply(ev->kind, ev->u.raw, ev->from);
            }
            else if (ev->kind >= COOPEV_VSHIT)
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

/* ---- revive, cutscenes and the results (coop_revive.c, coop_cutscene.c,
 * coop_stats.c): their events, COOPEV_DESIGN_FIRST..LAST ------------------ */

void coopDesignSend(s32 kind, s32 to, const void *payload, s32 len)
{
    CoopEvent ev;

    if (sEnabled <= 0 || !coopNetHasPeer() || kind < COOPEV_DESIGN_FIRST || kind > COOPEV_DESIGN_LAST)
    {
        return;
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = (u8) kind;
    memcpy(ev.u.raw, payload, (len < (s32) sizeof(ev.u.raw)) ? len : (s32) sizeof(ev.u.raw));
    coopQueueEventTo(&ev, to);
}

s32 coopDesignSelf(void)
{
    return (sEnabled > 0) ? coopSelf() : 0;
}

s32 coopDesignIsHost(void)
{
    return sEnabled > 0 && coopNetIsHost();
}

/* host: run script commands (AI_EndList ended) as a level script would, on
 * any chr, so the presentation ones are relayed as usual (harness only) */
void coopRunHostScript(u8 *list)
{
    ChrRecord *chr = NULL;
    AIRecord *savedlist;
    s32 savedofs;
    s32 i;

    for (i = 0; chr == NULL && i < g_NumChrSlots; i++)
    {
        if (g_ChrSlots[i].model != NULL && g_ChrSlots[i].prop != NULL && coopPlayerOfProp(g_ChrSlots[i].prop) < 0)
        {
            chr = &g_ChrSlots[i];
        }
    }
    if (chr == NULL || !coopNetIsHost())
    {
        return;
    }
    savedlist = chr->ailist;
    savedofs = chr->aioffset;
    chr->ailist = (AIRecord *) list;
    chr->aioffset = 0;
    ai((PropDefHeaderRecord *) chr, PROP_TYPE_CHR);
    chr->ailist = savedlist;
    chr->aioffset = savedofs;
}

/* a scripted cinema puts us beside its star (coop_cutscene.c): if we were
 * down, we stand up for it */
void coopCutsceneStandUp(coord3d *pos, StandTile *stan, f32 yaw)
{
    if (g_CurrentPlayer->bonddead)
    {
        coopRespawnPlayerAt(pos, stan, yaw, sLastWeapon);
        sLastHealth = g_CurrentPlayer->bondhealth;
        sDeadSince = 0;
        sBledOut = 0;
        coopSpectateStop();
        portConLogf("ge: coop: cutscene: up again for it\n");
        return;
    }
    coopTeleportPlayerTo(pos, stan, yaw);
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
    CoopSample at;

    if (!coopRemoteHere(p))
    {
        return 0;
    }
    r = &sR[p];
    out->pos[0] = r->st.pos[0];
    out->pos[1] = r->st.pos[1];
    out->pos[2] = r->st.pos[2];
    out->yaw = r->st.yaw;
    if (!sNoInterp && coopTrackAt(&r->track, coopClockRenderMs(&sClock[p], coopNowMs()), &at) >= 0)
    {   /* drawn between two of their samples, as a co-op puppet is (coopPuppetUpdate) */
        out->pos[0] = at.pos[0];
        out->pos[1] = at.pos[1];
        out->pos[2] = at.pos[2];
        out->yaw = at.yaw;
    }
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
static ObjectRecord *sGadgetObj;    /* the last object a partner used a gadget on, and the item */
static s32 sGadgetItem;
static s32 sGadgetCheck;            /* a script just found that gadget use: the item, for its next check */
static void *sGadgetScript;

/* chrai.c AI_IFBondUsedGadgetOnObject passed for obj in script: was it a partner's? */
void coopGadgetCheck(ObjectRecord *obj, void *script)
{
    sGadgetCheck = 0;
    if (obj != NULL && obj == sGadgetObj)
    {
        sGadgetCheck = sGadgetItem;
        sGadgetScript = script;
        sGadgetObj = NULL;
    }
}

/* chrai.c AI_IFBondHasItemEquipped in that script: the partner's item, -1 ours */
s32 coopGadgetItem(void *script)
{
    s32 item = (script == sGadgetScript) ? sGadgetCheck : 0;

    if (script == sGadgetScript)
    {
        sGadgetCheck = 0;
    }
    return (item > 0) ? item : -1;
}

static void coopSendObjUse(ObjectRecord *obj, s32 gadget);
static void coopNoteActivated(void *obj, s32 player);

void coopObjUsed(ObjectRecord *obj)
{
    coopSendObjUse(obj, 0);
}

/* The same for a gadget (bomb defuser, data thief, door decoder...) used on
 * an object: gunfire.c marks it PROPSTATE_ACTIVATED here, and the host's
 * script (Frigate's bombs) checks it was the right item. */
void coopObjGadgetUsed(ObjectRecord *obj, s32 item)
{
    coopSendObjUse(obj, item);
}

static void coopSendObjUse(ObjectRecord *obj, s32 gadget)
{
    CoopEvent ev;
    s32 ofs;

    if (sEnabled > 0 && !sApplyingUse && !coopVersus() && coopNetIsHost() && coopPartnerHere())
    {   /* the host's own press: an objective it finishes is ours */
        coopNoteActivated(obj, coopSelf());
        coopObjNoteUse(coopSelf());
    }
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
    ev.u.use.gadget = (s16) gadget;
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
    if (ev->u.use.gadget > 0)
    {   /* the joiner's gadget: its item stands in for ours when the script asks what "Bond" holds */
        obj->state |= PROPSTATE_ACTIVATED;
        sGadgetObj = obj;
        sGadgetItem = ev->u.use.gadget;
        coopNoteActivated(obj, ev->from);
        coopObjNoteUse(ev->from);
        portConLogf("ge: coop: player %d used item %d on obj %d\n", ev->from + 1, ev->u.use.gadget, ev->u.use.objofs);
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
    /* no door-switch link here (sub_GAME_7F03E6A0): the joiner's press already ran it and its
     * DOOR events carry the doors; running it again toggled the gate back shut on the joiner */
    coopNoteActivated(obj, ev->from);
    coopObjNoteUse(ev->from);   /* an objective the press completes, now or a little later, is theirs (coop_objective.c) */
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

/* ---- the mission countdown -------------------------------------------------
 * Statue's helicopter, Silo's and Bunker 2's self-destructs, and the other
 * level timers (Dam, Streets, Surface 2, Train, Aztec, Cradle). Every game ran its own copy of the level
 * script's countdown: a player who dropped in got a fresh full timer (or none,
 * where it starts after the intro swirl), and the blast at zero
 * (AI_TriggerExplosionsAroundBond) went off only in the game whose own timer
 * ran out. Now the host's timer is the team's: its value rides the WORLD
 * packet, a joiner's own script commands for the countdown and the blast are
 * stepped over (coopAiSkip), and the host's blast is relayed with the other
 * presentation commands, so it goes off around every player at once. */
/* sClockSet (declared above): host, a level script has set or started the countdown this mission */

static void coopApplyClock(CoopWorldPkt *p)
{
    s32 running = (p->clockflags & COOP_CLOCK_RUNNING) != 0;
    s32 shown = (p->clockflags & COOP_CLOCK_SHOWN) != 0;
    f32 off = countdownTimerGetValue() - p->clock;

    if (!(p->clockflags & COOP_CLOCK_SET))
    {
        return;
    }
    if (running != (countdownTimerIsRunning() != 0) || shown != !(clock_drawn_flag & 1))
    {
        portConLogf("ge: coop: countdown %s and %s, %.1f s left (host)\n", running ? "running" : "stopped",
                    shown ? "shown" : "hidden", p->clock / 60.0f);
    }
    if (off > 30.0f || off < -30.0f || running != (countdownTimerIsRunning() != 0))
    {   /* half a second out, or started or stopped: the host's */
        countdownTimerSetValue(p->clock);
    }
    countdownTimerSetRunning(running);
    countdownTimerSetVisible(1, shown);
}

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
    if (coopNetIsHost() && sClockSet)
    {
        p.clock = countdownTimerGetValue();
        p.clockflags = COOP_CLOCK_SET | (countdownTimerIsRunning() ? COOP_CLOCK_RUNNING : 0)
                     | ((clock_drawn_flag & 1) ? 0 : COOP_CLOCK_SHOWN);
    }
    coopNetSend(&p, sizeof(p));
}

/* A joiner runs the level scripts too, and its own copy of one can set a
 * flag a moment before the host's does (the joiner planted the mine): the
 * host's older flags would take it away again for a packet or two, and the
 * objective would read complete, incomplete, complete. Flags our scripts set
 * are kept while the host catches up: a second, plus two round trips on a
 * long line (what made us set it has to reach the host, and its WORLD come
 * back). */
static u32 sWorldOwnMs;

static void coopApplyWorld(CoopWorldPkt *p)
{
    struct criteria_roomentered *r;
    struct criteria_deposit *d;
    struct criteria_picture *ph;
    s32 i;

    if (!coopNetIsHost())
    {
        u32 own = (u32) objectiveregisters1 & ~sWorldHostFlags & ~sWorldOwnFlags;
        u32 flags;

        if (own != 0)
        {
            sWorldOwnFlags |= own;
            sWorldOwnMs = coopNowMs();
        }
        sWorldOwnFlags &= ~p->stageflags;
        if (coopNowMs() - sWorldOwnMs > 1000u + 2u * (u32) coopNetRttMs(0))
        {
            sWorldOwnFlags = 0;
        }
        sWorldHostFlags = p->stageflags;
        flags = p->stageflags | sWorldOwnFlags;
        if ((u32) objectiveregisters1 != flags)
        {
            portConLogf("ge: coop: stage flags %08x -> %08x\n", (u32) objectiveregisters1, flags);
            objectiveregisters1 = (s32) flags;
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
        coopApplyClock(p);
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
    if (!coopInMission() || g_ClockTimer <= 0 || (g_CurrentPlayer->bonddead && !coopNetIsHost()))
    {   /* later; the host takes it even while down: its inventory is what the
         * collect objectives check (Statue's flight recorder, a partner's
         * pickup while the host waits to respawn) */
        return 0;
    }
    sApplyingRemotePick = 1;
    {   /* the stock pickup refuses a dead Bond: the host down, stood up for it, quietly */
        const s32 down = g_CurrentPlayer->bonddead;

        g_CurrentPlayer->bonddead = 0;
        op = propPickupByPlayer(prop, !down);
        if (op != TICKOP_NONE)
        {
            propExecuteTickOperation(prop, op);
        }
        g_CurrentPlayer->bonddead = down;
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

/* A level script handing "Bond" an item (Facility's Dr. Doak and his door
 * decoder, AI_BondCollectObject) gives it to the host's Bond, and the stock
 * pickup refuses a dead one: with the host down or out of lives while a
 * partner talked to Doak, the decoder went nowhere. Stand the host up for it,
 * as for a partner's pickup; coopPickedUp then shares it with the team. */
s32 coopScriptGiveBegin(void)
{
    const s32 down = g_CurrentPlayer->bonddead;

    if (!down || sEnabled <= 0 || !coopNetIsHost() || !coopInMission())
    {
        return -1;
    }
    g_CurrentPlayer->bonddead = 0;
    portConLogf("ge: coop: a script gives Bond an item while the host is down: taken for the team\n");
    return down;
}

void coopScriptGiveEnd(s32 down)
{
    if (down >= 0)
    {
        g_CurrentPlayer->bonddead = down;
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

/* Team rules for the debrief, the unlocks and the cheats: the game marks
 * Bond killed in action (g_isBondKIA) at any death and fails the mission on
 * it, but in co-op the mission fails only when everyone is down. So if we
 * stand at the end, or a partner does, or a partner's game ended it, our
 * earlier fall is forgiven; objectives and the team's time still decide. */
static void coopTeamKiaRule(void)
{
    CoopHudPartner p;
    s32 i, standing;

    if (!g_isBondKIA || sEnabled <= 0 || !coopNetHasPeer() || coopVersus())
    {
        return;
    }
    standing = sApplyingRemoteEnd || !g_CurrentPlayer->bonddead;
    for (i = 0; !standing && coopHudPartnerAt(i, &p); i++)
    {
        standing = !p.dead;
    }
    if (standing)
    {
        g_isBondKIA = FALSE;
        portConLogf("ge: coop: the team got through, our fall doesn't fail the mission\n");
    }
}

void coopMissionComplete(void)
{
    CoopEvent ev;

    coopTeamTimeFreeze();     /* the team's time as the mission ends (coop_teamtime.c) */
    coopTeamKiaRule();        /* a fall the team got over doesn't fail it */
    coopCampaignMissionEnd(); /* a win together goes in coop_campaign.txt */
    if (sEnabled <= 0 || sApplyingRemoteEnd || !coopNetHasPeer() || !coopPartnerHere())
    {
        return;
    }
    if (sHostEndingHere && !coopNetIsHost())
    {   /* our copy of the host's ending ran out first: the host's own ends it for everyone */
        portConLogf("ge: coop: our copy of the ending finished; the host's ends the mission\n");
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
 * of combat for a few seconds, or after 20 s regardless (the lobby's Respawn
 * can make it a fixed wait instead). With no living partner to come back to,
 * the death is the normal mission failure. Each down costs a life when the
 * lobby set Lives; on the last one you are out until the mission ends. */

#define COOP_CALM_CAP        3600
#define COOP_RESPAWN_MIN     180   /* 3 s after the death fade */
#define COOP_RESPAWN_CALM    240   /* partner out of combat for 4 s */
#define COOP_RESPAWN_MAX     1200  /* 20 s */

/* the lobby's choices: COOPSET_LIVES, COOPSET_BLEEDOUT and COOPSET_RESPAWN
 * index these, 0 being the old fixed behaviour */
static const u8 sLivesChoices[] = { 0, 1, 2, 3, 5, 10 };                       /* 0 unlimited */
static const u8 sBleedOutChoices[] = { 60, 90, 120, 180, 15, 20, 30, 45 };     /* seconds */
static const u8 sRespawnChoices[] = { 0, 5, 10, 15, 30, 45, 60, 90 };          /* seconds, 0 the calm rule */
static const char *const sLivesNames[] = { "Unlimited\n", "1\n", "2\n", "3\n", "5\n", "10\n" };
static const char *const sBleedOutNames[] = { "60 s\n", "90 s\n", "2 min\n", "3 min\n", "15 s\n", "20 s\n", "30 s\n", "45 s\n" };
static const char *const sRespawnNames[] = { "3-20 s\n", "5 s\n", "10 s\n", "15 s\n", "30 s\n", "45 s\n", "60 s\n", "90 s\n" };
#define COOP_CHOICE(arr, i) ((arr)[((i) >= 0 && (i) < (s32) ARRAYCOUNT(arr)) ? (i) : 0])

s32 coopLivesCount(void)
{
    return ARRAYCOUNT(sLivesChoices);
}

const char *coopLivesName(s32 i)
{
    return COOP_CHOICE(sLivesNames, i);
}

s32 coopBleedOutCount(void)
{
    return ARRAYCOUNT(sBleedOutChoices);
}

const char *coopBleedOutName(s32 i)
{
    return COOP_CHOICE(sBleedOutNames, i);
}

s32 coopRespawnCount(void)
{
    return ARRAYCOUNT(sRespawnChoices);
}

const char *coopRespawnName(s32 i)
{
    return COOP_CHOICE(sRespawnNames, i);
}

static s32 coopLivesStart(void)
{
    s32 lives = COOP_CHOICE(sLivesChoices, sSettings[COOPSET_LIVES]);

    return (lives == 0) ? -1 : lives;
}

s32 coopBleedOutTicks(void)
{
    return COOP_CHOICE(sBleedOutChoices, sSettings[COOPSET_BLEEDOUT]) * 60;
}

/* the timer's fixed wait in ticks, 0 for the calm rule (COOP_RESPAWN_MIN..MAX) */
static s32 coopRespawnFixed(void)
{
    return COOP_CHOICE(sRespawnChoices, sSettings[COOPSET_RESPAWN]) * 60;
}

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

/* Friendly Fire, the host's lobby row: 0 off, 1 full, 2 reduced (append only) */
static const char *const sFriendlyFireNames[] = { "Off", "Full", "Reduced" };

const char *coopFriendlyFireName(s32 level)
{
    return (level >= 0 && level <= COOPFF_REDUCED) ? sFriendlyFireNames[level] : "?";
}

/* the lobby row steps Off, Reduced, Full */
s32 coopFriendlyFireNext(s32 level, s32 dir)
{
    static const s8 order[] = { COOPFF_OFF, COOPFF_REDUCED, COOPFF_FULL };
    s32 i;

    for (i = 0; i < 3 && order[i] != level; i++)
    {
    }
    i = (i == 3) ? 0 : i;
    return order[(i + dir + 3) % 3];
}

/* what a partner's shot or blast does here, as a share of a guard's */
f32 coopFriendlyFireScale(void)
{
    switch (coopSetting(COOPSET_FRIENDLYFIRE))
    {
        case COOPFF_FULL: return 1.0f;
        case COOPFF_REDUCED: return 0.5f;
        default: return 0.0f;
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
        sReviveFirst = 0;
        sBledOut = 0;
        sDeathStan = (bond != NULL) ? bond->stan : NULL;
        if (bond != NULL)
        {
            sDeathPos = bond->pos;
        }
        sDeathYaw = 6.2831855f - bondviewGetPlayerYawRadians();
        if (sLivesLeft > 0)
        {
            sLivesLeft--;
        }
        if (sLivesLeft == 0)
        {
            sBledOut = COOPOUT_LIVES;
            hudmsgBottomShow("No lives left. You are out until the mission ends.\n");
        }
        else
        {
            char text[96];
            s32 n = snprintf(text, sizeof(text), "%s",
                             sDownRule == COOPDOWN_TIMER ? "Waiting for your partners to get clear..." : "Down! A partner can hold Use by you to revive you.");

            if (sLivesLeft == 1)
            {
                snprintf(text + n, sizeof(text) - n, " Last life.");
            }
            else if (sLivesLeft > 1)
            {
                snprintf(text + n, sizeof(text) - n, " %d lives left.", (int) sLivesLeft);
            }
            strcat(text, "\n");
            hudmsgBottomShow(text);
        }
        portConLogf("ge: coop: dead, rule %d, lives left %d\n", sDownRule, sLivesLeft);
    }
    dead = sTick - sDeadSince;

    /* revive: a partner holds Use beside where we fell (their STATE names us, coop_revive.c) */
    if (sDownRule != COOPDOWN_TIMER && !sBledOut && sDeathStan != NULL)
    {
        s32 over = -1;

        for (p = 0; p < COOP_MAX_PLAYERS && over < 0; p++)
        {
            f32 dx = sR[p].st.pos[0] - sDeathPos.f[0];
            f32 dz = sR[p].st.pos[2] - sDeathPos.f[2];
            f32 dy = sR[p].st.pos[1] - sDeathPos.f[1];

            if (coopRemoteAliveHere(p) && sR[p].st.revive == coopSelf() + 1
                && dx * dx + dz * dz < COOP_REVIVE_DIST * COOP_REVIVE_DIST && dy < 250.0f && dy > -250.0f)
            {
                over = p;
            }
        }
        if (over >= 0)
        {
            if (sReviveHold == 0 && sReviveFirst == 0)
            {
                sReviveFirst = sTick;   /* the first hold over us since we fell */
            }
            if (++sReviveHold >= COOP_REVIVE_HOLD)
            {
                portConLogf("ge: coop: revived by player %d after %.1f s (%.1f s from the first hold)\n", over + 1, dead / 60.0f,
                            (sTick - sReviveFirst) / 60.0f);
                sReviveFirst = 0;
                sR[over].puppetGhost = 1;
                coopRespawnPlayerAt(&sDeathPos, sDeathStan, sDeathYaw, sLastWeapon);
                g_CurrentPlayer->bondhealth = 0.5f;     /* back on your feet, not fresh */
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
        if (!sBledOut && dead >= (u32) coopBleedOutTicks())
        {
            sBledOut = COOPOUT_BLEDOUT;
            hudmsgBottomShow("You bled out. You are out until the mission ends.\n");
            portConLogf("ge: coop: bled out\n");
        }
        coopSpectateTick();
        return 1;   /* no timer: down until revived, or out */
    }

    if (sBledOut)
    {
        coopSpectateTick();
        return 1;   /* no lives left: out until the mission ends */
    }
    if (coopRespawnFixed() > 0 ? dead >= (u32) coopRespawnFixed()
                               : ((dead >= COOP_RESPAWN_MIN && calm >= COOP_RESPAWN_CALM) || dead >= COOP_RESPAWN_MAX))
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
        if (!g_CurrentPlayer->bonddead || sLivesLeft == 1)
        {
            return -1;  /* (on our last life this death is for good) */
        }
        dead = 0;
    }
    else
    {
        dead = sTick - sDeadSince;
    }
    if (sDownRule == COOPDOWN_REVIVE)
    {
        return (coopBleedOutTicks() - (s32) dead > 0) ? coopBleedOutTicks() - (s32) dead : 0;
    }
    if (coopRespawnFixed() > 0)
    {
        return (coopRespawnFixed() - (s32) dead > 0) ? coopRespawnFixed() - (s32) dead : 0;
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

/* netcode (coop_repl.c): 1 once the host's join snapshot is in. Without that
 * module the world is already the host's (doors, objects and AI sync). */
__attribute__((weak)) s32 coopReplSnapshotDone(void)
{
    return 1;
}

#define COOP_DROPIN_MIN     120   /* 2 s of watching first: the puppets and the world settle */
#define COOP_DROPIN_GIVEUP  3600  /* 60 s with nobody standing: start at the level's start */

/* Drop-in: a joiner who followed the host into a mission under way watches
 * a partner until it is safe to come in, by the respawn rules (the calmest
 * partner out of a fight for 4 s, at most 20 s, and the join snapshot in),
 * then arrives beside them. Until then no STATE of ours goes out, so no
 * puppet of us stands at the level's start for the host's guards to shoot. */
static s32 coopDropInMin(void)
{
    static s32 minwait = -1;

    if (minwait < 0)
    {   /* GE_COOP_TESTDROPIN=<s>: watch at least <s> s, for screenshots (harness only) */
        const char *e = getenv("GE_COOP_TESTDROPIN");

        minwait = (e != NULL) ? atoi(e) * 60 : COOP_DROPIN_MIN;
    }
    return minwait;
}

static void coopDropInTick(void)
{
    u32 calm, waited;
    s32 partner;
    CoopRemote *r;

    if (sDropInSince == 0)
    {
        sDropInSince = sTick;
        coopSpectateDropIn(1);
        portConLogf("ge: coop: dropping in, waiting for a quiet moment\n");
    }
    waited = sTick - sDropInSince;
    coopSpectateTick();
    partner = coopRespawnPartner(&calm);
    if (partner < 0)
    {
        if (waited >= COOP_DROPIN_GIVEUP)
        {   /* everyone is down: play from where the level put us */
            sLateJoin = 0;
            coopSpectateDropIn(0);
            coopFeedToast("Joined at the start", -1);
            portConLogf("ge: coop: nobody standing to drop in beside, starting at the level's start\n");
        }
        return;
    }
    if (waited < (u32) coopDropInMin() || (!coopReplSnapshotDone() && waited < COOP_RESPAWN_MAX)
        || (calm < COOP_RESPAWN_CALM && waited < COOP_RESPAWN_MAX))
    {
        return;
    }
    r = &sR[partner];
    sLateJoin = 0;
    coopSpectateDropIn(0);
    r->puppetGhost = 1; /* we arrive inside the partner */
    coopRespawnPlayerAt(&r->puppet->pos, r->puppet->stan, 6.2831855f - r->st.yaw, sLastWeapon); /* view yaw runs the other way */
    sLastHealth = g_CurrentPlayer->bondhealth;
    {
        char text[48];

        snprintf(text, sizeof(text), "Dropped in beside %s", coopPlayerName(partner));
        coopFeedToast(text, partner);
    }
    portConLogf("ge: coop: dropped in beside player %d after %.1f s (calm %.1f s)\n",
                partner + 1, waited / 60.0f, calm / 60.0f);
}

/* the HUD's drop-in line: who we will come in beside and roughly when */
static s32 coopDropInBanner(char *line1, char *line2, s32 cap)
{
    u32 calm, waited;
    s32 partner, eta;

    if (!sLateJoin || sDropInSince == 0 || !coopInMission())
    {
        return 0;
    }
    waited = sTick - sDropInSince;
    partner = coopRespawnPartner(&calm);
    if (partner < 0)
    {
        snprintf(line1, cap, "DROPPING IN");
        snprintf(line2, cap, "WAITING FOR A PARTNER TO GET UP");
        return 1;
    }
    /* the calm still needed (if they stay calm from now), capped by the
     * maximum, and never before the minimum watch */
    eta = (s32) COOP_RESPAWN_CALM - (s32) calm;
    if ((s32) COOP_RESPAWN_MAX - (s32) waited < eta)
    {
        eta = (s32) COOP_RESPAWN_MAX - (s32) waited;
    }
    if (coopDropInMin() - (s32) waited > eta)
    {
        eta = coopDropInMin() - (s32) waited;
    }
    snprintf(line1, cap, "DROPPING IN BESIDE %s", coopPlayerName(partner));
    snprintf(line2, cap, calm < COOP_RESPAWN_CALM ? "WHEN IT GOES QUIET %d" : "IN %d", (int) ((eta < 0 ? 0 : eta) + 59) / 60);
    return 1;
}

/* ---- HUD (coop_hud.c) ------------------------------------------------------- */

/* 1 when there is an i-th partner (from 0) in this mission; fills what the
 * HUD shows of them */
s32 coopHudPartnerAt(s32 i, CoopHudPartner *out)
{
    s32 p;
    CoopStatePkt *st;

    if (sEnabled <= 0 || !coopInStage())
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
    out->revivable = st->dead && !st->out && st->downrule != COOPDOWN_TIMER && (st->downrule != COOPDOWN_REVIVE || st->respawn > 0);
    out->revivehold = st->dead ? st->revive : 0;
    out->out = st->dead && st->out;
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

s32 coopHudBleedOut(void)
{
    u32 dead;

    if (sEnabled <= 0 || sDownRule != COOPDOWN_REVIVE || sBledOut || !g_CurrentPlayer->bonddead)
    {
        return -1;
    }
    dead = (sDeadSince != 0) ? sTick - sDeadSince : 0;
    return (dead < (u32) coopBleedOutTicks()) ? (s32) (coopBleedOutTicks() - dead) : 0;
}

s32 coopHudBledOut(void)
{
    return (sDeadSince != 0) ? sBledOut : COOPOUT_NONE;
}

s32 coopHudLives(void)
{
    if (sEnabled <= 0 || sLivesLeft < 0)
    {
        return -1;
    }
    return (g_CurrentPlayer->bonddead && sDeadSince == 0 && sLivesLeft > 0) ? sLivesLeft - 1 : sLivesLeft;  /* still falling: this down's life is spent */
}

s32 coopHudRespawnEta(void)
{
    return (sEnabled > 0) ? coopRespawnEta() : -1;
}

/* ---- menus: hello and following the host ------------------------------------ */

static s32 sHelloNow;   /* send this hello whatever the tick (coopSendHelloNow) */

static void coopSendHello(void)
{
    CoopHelloPkt p;

    s32 c, i;

    if ((!sHelloNow && (sTick % (sOnLobbyScreen ? 15 : 60)) != 0) || !coopNetHasPeer())
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
    if (coopNetIsHost())
    {   /* Guard AI is the host's Port Option; the slot only shows it to the joiner */
        sSettings[COOPSET_GUARDAI] = (u8) coopAiSmarterOption();
    }
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
        if (i > 0)
        {
            CoopLink l;

            coopNetLink(i, &l);
            p.linkrtt[i] = (u16) l.rtt;
            p.linkloss[i] = (u8) l.loss;
            p.linkjit[i] = (u8) (l.jitter > 255 ? 255 : l.jitter);
        }
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
            const char *l = getenv("GE_COOP_LIVES");
            const char *b = getenv("GE_COOP_BLEEDOUT");
            const char *r = getenv("GE_COOP_RESPAWN");
            const char *s = getenv("GE_COOP_SCALING");
            testdiff = 0;
            if (s != NULL)
            {   /* GE_COOP_SCALING=<COOPSCALE_*>: 0 auto, 1 off, 2 as 3, 3 as 4 */
                sSettings[COOPSET_SCALING] = (u8) atoi(s);
            }
            if (l != NULL)
            {   /* GE_COOP_LIVES, _BLEEDOUT, _RESPAWN=<index>: the lobby's choices */
                sSettings[COOPSET_LIVES] = (u8) atoi(l);
            }
            if (b != NULL)
            {
                sSettings[COOPSET_BLEEDOUT] = (u8) atoi(b);
            }
            if (r != NULL)
            {
                sSettings[COOPSET_RESPAWN] = (u8) atoi(r);
            }
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
    coopTimeTick();
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
        sLateJoin = sHostHello.missionticks > 600;   /* the host is well into it: drop in beside a partner */
        sDropInSince = 0;
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

/* players in the session, us included (1 alone). By connection, not by a
 * fresh HELLO: a game loading the stage goes quiet for a few seconds. */
s32 coopLobbyPlayerCount(void)
{
    s32 c, p, n = 1;

    if (sEnabled <= 0 || !coopNetHasPeer())
    {
        return 1;
    }
    if (coopNetIsHost())
    {
        for (c = 0; c < COOP_NET_MAX_PEERS; c++)
        {
            n += coopNetPeerConnected(c) && sChan[c].helloSeen;
        }
        return n;
    }
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        n += p != coopSelf() && sHostHello.roster[p].present;
    }
    return n;
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
 * back (a hitch), COOP_HOST_WAIT_LOAD_MS early in a mission or in the menus
 * (a slow load). A host that crashes says goodbye from its crash report
 * (coopCrashBye), so its partners do not wait at all. After that, or at once when the host says
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

#define COOP_SILENT_MS        2000    /* nothing from the host this long: say so */
#define COOP_HOST_WAIT_MS     3500    /* and, mid-mission, wait this much longer for it to come back */
#define COOP_HOST_WAIT_FAR_MS 12000   /* or this long on a long or shaky line of our own (coopHostWaitMs) */
#define COOP_HOST_WAIT_LOAD_MS 12000  /* or this long while it may still be loading (a slower PC) */
#define COOP_NEWHOST_WAIT_MS  8000    /* the player picked to host must answer within this */
#define COOP_LOST_MS          3000    /* host: a joiner quiet this long has lost its connection */
#define COOP_DROP_MS          30000   /* and is gone after this long lost */
#define COOP_REMOTE_GONE_MS   40000   /* joiner: a relayed partner silent this long is gone */
#define COOP_HOSTTITLE_MS     2000    /* joiner: the host back in the menus this long, mid-mission */

enum { HOSTW_NONE, HOSTW_SILENT, HOSTW_NEWHOST };

/* how long a silent host gets: short once we both play the same mission
 * (it was there a moment ago: a crash or a lost connection, not a load).
 * On a long or shaky line the silence is more likely ours: a far-away
 * player gave up on the host after 5.5 s and, the others still playing
 * with it, went on alone as a host of its own. A host that really goes says
 * so (BYE from its crash report, or Steam sees it leave), so only a pulled
 * cable waits the longer time. */
static u32 coopHostWaitMs(void)
{
    if (coopInMission() && sMissionTicks > 600 && sHostHello.stage == bossGetStageNum())
    {
        return sHostWaitFar ? COOP_HOST_WAIT_FAR_MS : COOP_HOST_WAIT_MS;
    }
    return COOP_HOST_WAIT_LOAD_MS;
}

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

/* port_report.c, the game crashed or stalled (its report's text is written):
 * the same goodbye, without the AI sync, which may be what broke. Once. */
void coopCrashBye(void)
{
    static s32 done;

    if (done || sEnabled <= 0 || !coopNetIsOpen() || !coopNetHasPeer())
    {
        return;
    }
    done = 1;
    coopSendBye(coopNetIsHost() ? COOPBYE_HANDOFF : COOPBYE_LEAVE, coopNetIsHost() ? coopNextHost() : 0);
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

/* a hello now, off its period. Not by setting sTick to 0 for the call: the
 * roster's "here" test (a hello within 180 ticks) then read every joiner as
 * gone, and a host lost just after such a hello left each joiner hosting
 * alone. */
static void coopSendHelloNow(void)
{
    sHelloNow = 1;
    coopSendHello();
    sHelloNow = 0;
}

/* while paused (the watch) or in a cutscene coopTick does not run: keep
 * saying hello so nobody takes the quiet for a dropped game */
static void coopKeepAlive(void)
{
    static u32 last;
    u32 now = coopNowMs();

    if (sEnabled <= 0 || !coopNetHasPeer() || now - last < 500)
    {
        return;
    }
    last = now;
    coopSendHelloNow();
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
    sHold = 0;
    portConLogf("ge: coop: mission card down after %d ms (%s)\n", (int) (coopNowMs() - sHoldMs), why);
    if (coopNetIsHost())
    {   /* tell them at once, not at the next hello */
        coopSendHelloNow();
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
        coopSendHelloNow();
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

        if (chr->model == NULL || chr->prop == NULL || coopIsPlayerChr(chr))
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

    if (!coopInStage() || g_ChrSlots == NULL)
    {
        return;
    }
    for (k = 0; k < p->count; k++)
    {
        ChrRecord *chr = chrFindByLiteralId(p->ent[k].chrnum);
        s32 slot;

        if (chr == NULL || chr->prop == NULL || coopIsPlayerChr(chr))
        {
            continue;
        }
        slot = chr - g_ChrSlots;
        if (slot >= 0 && slot < COOP_MAX_CHRS)
        {
            sAiSync[slot] = p->ent[k];
            sAiSyncHave[slot] = 1;
            sAiSyncGot[slot]++;
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
        if (!sAiSyncHave[i] || chr->model == NULL || chr->prop == NULL || coopIsPlayerChr(chr) || chr->chrnum != e->chrnum)
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
    {   /* each player's clock goes with them: a joiner's tracks were timed by it */
        CoopClock oldclk[COOP_MAX_PLAYERS];

        memcpy(oldclk, sClock, sizeof(oldclk));
        memset(sClock, 0, sizeof(sClock));
        for (p = 0; p < COOP_MAX_PLAYERS; p++)
        {
            if (p != sMigSelf && newof[p] >= 0)
            {
                sClock[newof[p]] = oldclk[p];
            }
        }
    }
    for (p = 0; p < COOP_MAX_CHRS; p++)
    {   /* the guards' tracks were timed by the old host's clock */
        coopTrackReset(&sGuardTrack[p]);
        sGuardTrackNum[p] = -1;
    }
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
    coopVoiceRenumber(newof);
    coopVoteRenumber(newof);
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
    coopReplMigrated();     /* the new host's world is the one everyone follows now */
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
            const unsigned long long id = coopNetPeerConnected(c) ? coopNetPeerId(c) : 0;

            if (id != sChan[c].peerId)
            {   /* someone new in the slot (or the one who left it, back): its silence starts now,
                 * not when the last one went quiet, or it is freed before its first packet */
                sChan[c].peerId = id;
                sChan[c].heardMs = (id != 0) ? now : 0;
            }
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
                sHostWaitFar = sHostHiccups > 0 || sRtt[0] >= COOP_LINK_POOR_RTT || sRttDev[0] >= COOP_LINK_POOR_JITTER;
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
                sHostHiccups++;
                portConLogf("ge: coop: the host is back\n");
                coopBanner("THE HOST IS BACK", "");
            }
            else if (coopNetHostLeft())
            {
                coopHostGone(COOPBYE_LEAVE);
            }
            else if (now - sHostWaitMs > coopHostWaitMs())
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
    if (sHostWaiting == HOSTW_NONE && coopDropInBanner(line1, line2, cap))
    {
        return 1;
    }
    if (sHostWaiting == HOSTW_SILENT)
    {
        s32 left = (s32) (coopHostWaitMs() - (now - sHostWaitMs) + 999) / 1000;

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
    return (rule == 1) ? "Back to Lobby\n" : "New Host\n";
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

/* A level script's room, pad and height checks look at our own Bond first,
 * then the living partners. The host's Bond lies where it fell while the
 * host waits to be revived or watches with no lives left: that body counted
 * as Bond standing there (in Cradle's fall shaft, at a pad Trevelyan runs
 * from, in an objective room). While a partner stands, a down host's body
 * is nobody. */
s32 coopOwnBondCounts(void)
{
    s32 p;

    if (sEnabled <= 0 || !coopNetIsHost() || g_CoopPropOverride != NULL || !g_CurrentPlayer->bonddead)
    {
        return 1;
    }
    for (p = 1; p < COOP_MAX_PLAYERS; p++)
    {
        if (coopRemoteAliveHere(p))
        {
            return 0;
        }
    }
    return 1;
}

/* Cradle: falling off the dish into its shaft (pad 147's room, below y -2550
 * then -4070) runs the level script's "Bond fell" ending, and with partners
 * that list (0x041c) ended the mission for everybody when any one of them
 * fell. In co-op those two checks never pass (chrai.c) and each game kills
 * its own Bond at the bottom of the shaft instead: an ordinary death, with
 * the lives, bleed-out and team-out rules any other death has. */
#define COOP_CRADLE_FALL_PAD 147
#define COOP_CRADLE_FALL_Y   -4070.0f

s32 coopLevelFallCheckOff(f32 y)
{
    return sEnabled > 0 && !coopVersus() && bossGetStageNum() == LEVELID_CRADLE
        && (y == -2550.0f || y == COOP_CRADLE_FALL_Y);
}

static void coopCradleFallTick(void)
{
    if (!coopLevelFallCheckOff(COOP_CRADLE_FALL_Y) || !coopInMission()
        || g_CurrentPlayer->bonddead || g_PlayerInvincible || g_CoopPropOverride != NULL)
    {
        return;
    }
    if (getCurrentPlayerProp()->pos.y < COOP_CRADLE_FALL_Y && check_if_actor_is_at_preset(NULL, COOP_CRADLE_FALL_PAD))
    {
        portConLogf("ge: coop: fell into the Cradle shaft: a death of our own\n");
        bondviewCallRecordDamageKills(10.0f, 0.0f, -1, 1);
    }
}

/* What "Bond" holds and how hurt they are, for whoever is Bond right now (a
 * guard's target, a coopAltBondBegin / coopChrAltBegin try): a partner's from
 * their STATE, else our own. The game's checks read our own player struct,
 * so on the host a joiner Valentin was talking to was asked about the host's
 * gun (Statue), and a taunt about Bond's wounds was about the host's. */
static s32 coopBondIsPartner(void)
{
    s32 p = (g_CoopPropOverride != NULL) ? coopPlayerOfProp(g_CoopPropOverride) : -1;

    return (p > 0 && p < COOP_MAX_PLAYERS && coopRemoteHere(p)) ? p : -1;
}

s32 coopBondHasItem(s32 item)
{
    s32 p = coopBondIsPartner();

    if (p < 0)
    {
        return item == getCurrentPlayerWeaponId(GUNRIGHT) || item == getCurrentPlayerWeaponId(GUNLEFT);
    }
    return item == sR[p].st.look.weapon[GUNRIGHT] || item == sR[p].st.look.weapon[GUNLEFT];
}

f32 coopBondHealth(void)
{
    s32 p = coopBondIsPartner();

    return (p < 0) ? currentPlayerGetHealth() : sR[p].st.health;
}

/* a script's "Bond has this item" check passed for a partner */
void coopBondItemLog(s32 item)
{
    s32 p = coopBondIsPartner();
    static s32 lastp = -1, lastitem = -1;

    if (p > 0 && (p != lastp || item != lastitem))
    {
        lastp = p;
        lastitem = item;
        portConLogf("ge: coop: script found item %d in player %d's hand\n", item, p + 1);
    }
}

/* Security cameras (propobj.c) look for every player on the host. A joiner's
 * cameras don't count: its alarm follows the host's, which turned an alarm
 * raised here back off. */
s32 coopCctvWatches(void)
{
    return !coopIsMirroringClient();
}

/* a camera raised the alarm on seeing this Bond */
void coopCctvAlarm(PropRecord *seen)
{
    if (sEnabled > 0)
    {
        s32 p = coopPlayerOfProp(seen);

        portConLogf("ge: coop: camera saw player %d, alarm on\n", (p >= 0) ? p + 1 : coopSelf() + 1);
    }
}

/* A level script's "either player" check (chrai.c) passed: k 0 for our own
 * Bond, else the (k-1)-th living partner. The script's last passer is kept, so
 * when it then sets stage flags (an objective's trigger: the elevator, a
 * room) the credit goes to the player its check was about, not to whoever a
 * different script's check (run every tick) last saw. */

/* Who last pressed Use (or a gadget) on an object, for the script that later
 * finds it activated (chrai.c if_object_was_activated): Dam's mainframe
 * finishes its objective after a download, long after the press. */
#define COOP_ACTIVATED 16
static struct
{
    void *obj;
    s8 player;
} sActivated[COOP_ACTIVATED];
static s32 sActivatedNext;

static void coopNoteActivated(void *obj, s32 player)
{
    s32 i;

    for (i = 0; i < COOP_ACTIVATED && sActivated[i].obj != obj; i++)
    {
    }
    if (i == COOP_ACTIVATED)
    {
        i = sActivatedNext;
        sActivatedNext = (sActivatedNext + 1) % COOP_ACTIVATED;
    }
    sActivated[i].obj = obj;
    sActivated[i].player = (s8) player;
}

static void coopScriptPassBy(void *script, s32 who)
{
    s32 i;

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

/* chrai.c: script found obj activated (or a gadget used on it); its next
 * stage flags are the presser's */
void coopObjActivatedPass(void *script, void *obj)
{
    s32 i;

    if (sEnabled <= 0 || !coopNetIsHost())
    {
        return;
    }
    for (i = 0; i < COOP_ACTIVATED; i++)
    {
        if (sActivated[i].obj == obj && obj != NULL)
        {
            portConLogf("ge: coop: a script found player %d's press\n", sActivated[i].player + 1);
            coopScriptPassBy(script, sActivated[i].player);
            return;
        }
    }
    coopScriptPassBy(script, coopSelf());   /* nobody pressed it from afar: the host did */
}

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
    coopScriptPassBy(script, who);
}

void coopScriptSetsFlags(void *script, void *chr)
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
    if (chr != NULL && sEnabled > 0 && coopNetIsHost())
    {   /* a chr's own script (Valentin, Janus, a scientist): the player it is
         * dealing with, the one its "Bond" is right now */
        s32 p = (sTickChr == chr && g_CoopPropOverride != NULL) ? coopPlayerOfProp(g_CoopPropOverride) : -1;

        coopObjNoteActor((p >= 0) ? p : coopSelf(), 0);
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
static s32 sApplyingBoomOwner;   /* joiner: whose the host says it is, 1 + player, 0 not a player's */

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
            ev.u.boom.owner = (u8) (coopSelf() + 1);
            coopQueueEventTo(&ev, 0);
            portConLogf("ge: coop: our explosion (type %d), sent to host\n", type);
        }
        return;
    }
    if (sEnabled > 0 && coopNetIsHost() && coopNetHasPeer() && coopInMission())
    {   /* the host's (its guards' grenades, a launcher round, a joiner's passed on): seen and felt everywhere */
        s32 p;

        ev.u.boom.owner = (u8) (sApplyingRemoteBoom ? sApplyingRemoteBoom + 1 : 0);

        for (p = 1; p < COOP_MAX_PLAYERS; p++)
        {
            if (p != sApplyingRemoteBoom && coopRemoteHere(p))
            {
                coopQueueEventTo(&ev, p);
            }
        }
    }
}

/* Friendly fire from blasts. Each explosion made here remembers whose it is:
 * a thrown grenade or mine its thrower's (every game sets off its own copy,
 * coop_items.c), a partner's launcher round the sender's (the host plays it,
 * and passes the owner on with it to the others), our own ours. Guards',
 * barrels' and anything else's are nobody's and hurt in full. When a blast
 * reaches our Bond, explosion.c scales a partner's by Friendly Fire. */
#define COOP_BOOM_OWNERS 16
static void *sBoomOwnerOf[COOP_BOOM_OWNERS];
static s8 sBoomOwner[COOP_BOOM_OWNERS];     /* the player, -1 nobody's */
static u8 sBoomCounted[COOP_BOOM_OWNERS];   /* our blast already counted as a friendly hit */
static s32 sBoomNext;

static s32 coopExplosionOwnerNow(void)
{
    if (coopItemsExploding())
    {
        return coopItemsExplodingOwner() ? coopItemsExplodingOwner() - 1 : coopSelf();
    }
    if (sApplyingRemoteBoom)
    {
        return sApplyingRemoteBoom;
    }
    if (sApplyingHostBoom)
    {
        return sApplyingBoomOwner - 1;
    }
    return coopIsMirroringClient() ? coopSelf() : -1;   /* the joiner runs no guards: its own; the host's: not known */
}

/* explosion.c explosionCreate */
void coopExplosionNoteOwner(void *explosion)
{
    s32 i;

    for (i = 0; i < COOP_BOOM_OWNERS; i++)
    {
        if (sBoomOwnerOf[i] == explosion)
        {
            sBoomOwnerOf[i] = NULL;     /* the slot is reused */
        }
    }
    if (sEnabled <= 0 || !coopInMission() || coopVersus())
    {
        return;
    }
    i = sBoomNext;
    sBoomNext = (sBoomNext + 1) % COOP_BOOM_OWNERS;
    sBoomOwnerOf[i] = explosion;
    sBoomOwner[i] = (s8) coopExplosionOwnerNow();
    sBoomCounted[i] = 0;
}

static s32 coopExplosionSlot(void *explosion)
{
    s32 i;

    for (i = 0; explosion != NULL && i < COOP_BOOM_OWNERS; i++)
    {
        if (sBoomOwnerOf[i] == explosion)
        {
            return i;
        }
    }
    return -1;
}

/* explosion.c: the share of this blast's damage our Bond takes */
f32 coopExplosionDamageScale(void *explosion)
{
    const s32 i = coopExplosionSlot(explosion);
    f32 scale;

    if (i < 0 || sBoomOwner[i] < 0 || sBoomOwner[i] == coopSelf() || !coopInMission() || coopVersus())
    {
        return 1.0f;    /* nobody's, or our own */
    }
    scale = coopFriendlyFireScale();
    if (sBoomCounted[i] != 2)
    {
        sBoomCounted[i] = 2;
        portConLogf("ge: coop: player %d's blast reaches us: x%.1f (Friendly Fire %s)\n", sBoomOwner[i] + 1, scale,
                    coopFriendlyFireName(coopSetting(COOPSET_FRIENDLYFIRE)));
    }
    return scale;
}

/* explosion.c: a blast reached a chr; our own reaching a partner's Bond is a friendly hit (counted once) */
void coopExplosionHitChr(void *explosion, ChrRecord *chr)
{
    const s32 i = coopExplosionSlot(explosion);

    if (i < 0 || sBoomCounted[i] || sBoomOwner[i] != coopSelf() || chr == NULL || chr->prop == NULL
        || coopPlayerOfProp(chr->prop) < 0 || coopFriendlyFireScale() <= 0.0f || !coopRemoteAliveHere(coopPlayerOfProp(chr->prop)))
    {
        return;
    }
    sBoomCounted[i] = 1;
    coopStatsNote(COOPSTAT_FRIENDLY);
    portConLogf("ge: coop: our blast reaches player %d\n", coopPlayerOfProp(chr->prop) + 1);
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
        sApplyingBoomOwner = ev->u.boom.owner;
        explosionCreate(NULL, &pos, stan, ev->u.boom.type, 0, 0, ev->u.boom.rooms, 0);
        sApplyingHostBoom = 0;
        sApplyingBoomOwner = 0;
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
        case AI_BondDisableDamageAndPickups:   /* only endings use it: no one is shot or picks up during the host's */
        case AI_TextPrintBottom:
        case AI_TextPrintTop:
        case AI_MusicPlaySlot:
        case AI_MusicStopSlot:
        case AI_HideAllChrs:
        case AI_ShowAllChrs:
        case AI_MoveObject:     /* the Runway plane to its take-off pad */
        case AI_TriggerFadeAndExitLevelOnButtonPress:   /* an ending anyone may vote to skip (coop_vote.c) */
        case AI_TriggerExplosionsAroundBond:   /* a self-destruct's blast at zero: around every player */
            return 1;
        default:
            return 0;
    }
}

static s32 sApplyingRemoteAi;

/* a joiner's own copy of the level scripts leaves these to the host: the
 * countdown (WORLD's clock) and the blast when it runs out (relayed) */
static s32 coopHostOwnsCmd(u8 cmd)
{
    switch (cmd)
    {
        case AI_HudCountdownShow:
        case AI_HudCountdownHide:
        case AI_HudCountdownSet:
        case AI_HudCountdownStop:
        case AI_HudCountdownStart:
        case AI_TriggerExplosionsAroundBond:
            return sEnabled > 0 && !coopVersus() && coopInMission();
        default:
            return 0;
    }
}

void coopAiCommand(u8 *list, s32 offset, ChrRecord *self)
{
    CoopEvent ev;
    s32 len;
    u8 cmd = list[offset];

    if ((cmd == AI_HudCountdownSet || cmd == AI_HudCountdownStart || cmd == AI_HudCountdownShow)
        && sEnabled > 0 && coopNetIsHost())
    {
        sClockSet = 1;   /* from now on WORLD carries it (coopSendWorld) */
    }

    if (cmd == AI_SetChrAiList && list[offset + 1] == (u8) CHR_BOND_CINEMA)
    {
        /* an ending puts the player's own third-person body through a list
         * (the Dam bungee jump, Runway): each game runs it for its own Bond,
         * from the host's moment rather than when its own copy of the level
         * script gets there */
    }
    else if (!coopIsPresentationCmd(cmd))
    {
        return;
    }
    if (sApplyingRemoteAi || sEnabled <= 0 || !coopNetIsHost()
        || !coopNetHasPeer() || !coopPartnerHere())
    {
        return;
    }
    if (self != NULL && g_CurrentPlayer->prop != NULL && self == g_CurrentPlayer->prop->chr)
    {   /* our own body's list (an ending's walk and jump, its cameras and
         * fades): every game's level script gives it to that game's own body,
         * in the character that player picked */
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

/* joiner: an ending's list for our own third-person body, waiting for the
 * camera switch to make that body (it can lag the command by a few ticks) */
static s32 sPendingCinemaWait;

static void coopPendingCinemaTick(void)
{
    ChrRecord *bc;

    if (sPendingCinemaList < 0)
    {
        if (g_CameraMode == CAMERAMODE_FP)
        {
            sHostEndingHere = 0;    /* the intro's list is over, no ending yet */
        }
        return;
    }
    bc = (g_CurrentPlayer->prop != NULL) ? g_CurrentPlayer->prop->chr : NULL;
    if (bc != NULL)
    {
        bc->ailist = ailistFindById(sPendingCinemaList);
        bc->aioffset = 0;
        bc->sleep = 0;
        portConLogf("ge: coop: our Bond runs the ending's list %04x\n", sPendingCinemaList);
        sPendingCinemaList = -1;
        sHostEndingHere = 1;
    }
    else if (--sPendingCinemaWait <= 0)
    {
        portConLogf("ge: coop: no third-person Bond for the ending's list %04x\n", sPendingCinemaList);
        sPendingCinemaList = -1;
    }
}

/* joiner: while our body plays the host's ending, the host's camera, fade and
 * text commands reach us from the host (coopApplyAiCommand) and our body's
 * list runs its own; our copy of the level script gets to the same ending a
 * few seconds late, and its copies of them would black out the jump (chrai.c
 * steps over them) */
s32 coopAiSkip(u8 *list, s32 offset, ChrRecord *self)
{
    if (sEnabled > 0 && !coopNetIsHost() && !sApplyingRemoteAi && coopHostOwnsCmd(list[offset]))
    {
        return 1;   /* the host's countdown and blast (coopApplyClock, coopApplyAiCommand) */
    }
    return sEnabled > 0 && !coopNetIsHost() && sHostEndingHere && !sApplyingRemoteAi
        && g_CameraMode == CAMERAMODE_POSEND && coopIsPresentationCmd(list[offset])
        && (self == NULL || g_CurrentPlayer->prop == NULL || self != g_CurrentPlayer->prop->chr);
}

/* joiner: our own copy of the level script gives our body the list the host's
 * already started here: keep it going rather than start it over (chrai.c) */
s32 coopKeepBodyList(ChrRecord *chr, AIRecord *list)
{
    return sEnabled > 0 && !coopNetIsHost() && sHostEndingHere && g_CurrentPlayer->prop != NULL
        && chr == g_CurrentPlayer->prop->chr && chr->ailist == list;
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
    if (list[0] == AI_SetChrAiList && list[1] == (u8) CHR_BOND_CINEMA)
    {
        u16 raw;

        memcpy(&raw, &list[2], sizeof(raw));
        sPendingCinemaList = ntohs(raw);   /* as the interpreter reads it */
        sPendingCinemaWait = 300;
        coopPendingCinemaTick();
        return;
    }
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

s32 coopExtSetupOfs(void *rec)
{
    return coopSetupOfs(rec);
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

/* ---- crash and bug reports ---------------------------------------------- */

/* The co-op state for a crash or bug report (port/src/port_report.c): who we
 * are, the session, each partner and each channel. It only reads this file's
 * own copies (no props), so it is safe to call from the crash handler. */
s32 coopDebugDump(char *buf, s32 cap)
{
    u32 now = coopNowMs();
    s32 n = 0;
    s32 i;

#define DUMP(...) do { if (n < cap) { n += snprintf(buf + n, cap - n, __VA_ARGS__); } } while (0)
    DUMP("co-op protocol %d, %s\n", COOP_VERSION, sEnabled > 0 ? "on" : "off");
    if (sEnabled > 0)
    {
        DUMP("transport: %s, %s, %s\n", coopNetIsSteam() ? "steam" : "udp",
             coopNetIsOpen() ? "open" : "closed", coopNetIsHost() ? "host" : "joiner");
        DUMP("self: player %d (%s), session %08x\n", (int) coopSelf(), coopLobbySelfName(), (unsigned) sSelfSession);
        DUMP("stage %d, tick %u, mission ticks %u, late join %d, lobby screen %d\n",
             (int) bossGetStageNum(), (unsigned) sTick, (unsigned) sMissionTicks, (int) sLateJoin, (int) sOnLobbyScreen);
        DUMP("host hello: stage %d, difficulty %d, mission %u; our serial %u, followed %u\n",
             (int) sHostHello.stage, (int) sHostHello.difficulty, (unsigned) sHostHello.mission,
             (unsigned) sMissionSerial, (unsigned) sFollowedMission);
        DUMP("host waiting %d, migrated %d, pending end %d, down rule %d, settings",
             (int) sHostWaiting, (int) sMigrated, (int) sPendingEnd, (int) sDownRule);
        for (i = 0; i < COOPSET_MAX; i++)
        {
            DUMP(" %d", (int) sSettings[i]);
        }
        DUMP("\n");
        for (i = 0; i < COOP_MAX_PLAYERS; i++)
        {
            CoopRemote *r = &sR[i];

            if (i == coopSelf() || !r->valid)
            {
                continue;
            }
            DUMP("player %d %s: stage %d, pos %.0f %.0f %.0f, health %.2f, dead %d, lost %d, last heard %u ms ago, puppet %s\n",
                 i, coopPlayerName(i), (int) r->st.stage, r->st.pos[0], r->st.pos[1], r->st.pos[2],
                 r->st.health, (int) r->st.dead, (int) r->lost, (unsigned) (now - r->seenMs),
                 r->puppet != NULL ? "yes" : "no");
        }
        for (i = 0; i < COOP_NET_MAX_PEERS; i++)
        {
            CoopChan *c = &sChan[i];

            if (!coopNetPeerConnected(i) && c->session == 0)
            {
                continue;
            }
            DUMP("channel %d (%s): connected %d, session %08x, out %u acked %u, in %u, heard %u ms ago\n",
                 i, coopNetPeerName(i), coopNetPeerConnected(i), (unsigned) c->session,
                 (unsigned) c->outNext, (unsigned) c->outAcked, (unsigned) c->inDone,
                 (unsigned) (now - c->heardMs));
        }
    }
#undef DUMP
    return n < cap ? n : cap - 1;
}


/* ---- object replication (coop_repl.c) ------------------------------------- */

s32 coopReplPlayerHere(s32 p)
{
    return p >= 0 && p < COOP_MAX_PLAYERS && p != coopSelf() && coopRemoteHere(p) && sR[p].puppet != NULL;
}

s32 coopReplHostHere(void)
{
    return sEnabled > 0 && !coopNetIsHost() && coopInMission() && coopRemoteHere(0);
}

void coopReplDoorSet(DoorRecord *door, s32 state)
{
    sApplyingRemoteDoor = 1;
    doorSetOpenState(door, state);
    sApplyingRemoteDoor = 0;
}

void coopReplDestroy(ObjectRecord *obj)
{
    f32 damage = ((obj->damage - obj->maxdamage) + 1) / 250.0f;   /* as AI_DestroyObject */

    sApplyingRemoteObj = 1;
    objApplyDamage(obj, damage, &obj->runtime_pos, 0x1D, -1);
    sApplyingRemoteObj = 0;
}

void coopReplTeamPick(ObjectRecord *obj)
{
    s32 ofs = coopSetupOfs(obj);
    s32 i;

    if (ofs < 0)
    {
        return;
    }
    i = coopTeamPickAdd(ofs, 0, 0);
    if (i >= 0 && coopTeamPickTry(i))
    {
        sTeamPickDone[i] = 1;
    }
}
