#ifndef _COOP_H_
#define _COOP_H_
#include <ultra64.h>
#include <bondtypes.h>

void coopStageStart(void);
void coopTick(void);
s32  coopPropIsGhost(PropRecord *prop);

extern PropRecord *g_CoopPropOverride;
s32  coopChrTickBegin(ChrRecord *chr); /* 0: skip (client mirrors the host) */
void coopChrTickEnd(void);
void coopChrStimulus(ChrRecord *chr);
s32  coopTargetIsRemote(void);
void coopGuardTankFlags(s32 flag);
f32  coopTargetDuckingHeight(void);
s32  coopTargetDamageShowTime(void);
s32  coopTargetIsDead(void);
s32  coopTargetRecordDamage(f32 damage, f32 angle);
void coopDoorStateChanged(DoorRecord *door, s32 state);
s32  coopObjDamage(ObjectRecord *obj, f32 damage, coord3d *pos, s32 item, s32 player); /* 1: skip */
s32  coopSkipExplosionDamage(ChrRecord *chr);
void coopPickedUp(ObjectRecord *obj, s32 kept);
s32  coopPeerHolds(ObjectRecord *obj);
void coopMissionComplete(void);
s32  coopFrontTick(void);
s32  coopAltBondBegin(s32 k); /* 1: "Bond" is now the k-th living partner (from 0), call coopAltBondEnd */
void coopAltBondEnd(void);
void coopAltBondCredit(s32 k); /* the check passed for that partner */
void coopAiCommand(u8 *list, s32 offset, ChrRecord *self);
s32  coopDeadTick(void); /* 1: co-op owns this death (waiting or respawned) */
void coopRespawnPlayerAt(coord3d *pos, StandTile *stan, f32 yaw, s32 weapon);
s32  coopClientForwardShot(ChrRecord *chr, s32 hitpart, coord3d *vector, s32 weaponid);
s32  coopShotPuppet(ChrRecord *chr, s32 hitpart, coord3d *vector, s32 weaponid, s32 isPlayer);

#define COOP_MAX_PLAYERS 4   /* the host and up to three joiners */

/* the HUD (coop_hud.c) */
typedef struct
{
    coord3d pos;
    s32 dead;
    f32 health;
    f32 armour;
    s32 respawn;        /* ticks, -1 = not waiting */
    s32 revivable;      /* down, and walking over them brings them back */
    const char *name;
    s32 player;         /* their player number, 0 the host */
} CoopHudPartner;
s32  coopHudPartner(CoopHudPartner *out);           /* the first partner */
s32  coopHudPartnerAt(s32 i, CoopHudPartner *out);  /* the i-th, from 0 */
s32  coopHudRespawnEta(void);
s32  coopHudDownRule(void);
s32  coopHudReviveHold(void);   /* ticks of COOP_REVIVE_HOLD */
#define COOP_REVIVE_HOLD 45        /* the partner stands where we fell for 3/4 s */
s32  coopHudBledOut(void);

/* Lobby settings the host picks and the joiner's game copies (coopSetting).
 * Append only: 0 must stay each one's default. */
enum
{
    COOPSET_DIFFICULTY,     /* co-op difficulty: 0 normal (coop_difficulty.c) */
    COOPSET_FRIENDLYFIRE,   /* 0 off: shots pass partners by; 1 on: they hurt */
    COOPSET_SPARE2,
    COOPSET_SPARE3,
    COOPSET_SPARE4,
    COOPSET_SPARE5,
    COOPSET_SPARE6,
    COOPSET_HOSTLEAVES,     /* 0: the next player hosts and the mission goes on; 1: everyone back to the lobby */
    COOPSET_MAX = 8
};
s32  coopSetting(s32 which);
void coopSetSetting(s32 which, s32 value);

/* co-op difficulty (coop_difficulty.c): COOPSET_DIFFICULTY's modes */
s32  coopDifficultyCount(void);
const char *coopDifficultyName(s32 mode);
f32  coopDifficultyHealthMul(void);
s32  coopDifficultyExtraSlots(void);
void coopDifficultyStageStart(void);
void coopDifficultyTick(void);

/* what happens when a player goes down (the host picks in the lobby) */
enum { COOPDOWN_BOTH, COOPDOWN_TIMER, COOPDOWN_REVIVE };
s32  coopDownRule(void);
void coopSetDownRule(s32 rule);
Gfx *coopHudRender(Gfx *gdl);

/* giving ammo and guns to the partner (coop_give.c). A give and its answer
 * ride the reliable events (COOPEV_GIVE, COOPEV_GIVEBACK). */
typedef struct
{
    s16 weapon;     /* the giver's gun, ITEM_NOTHING for ammo only */
    s16 ammotype;
    s32 amount;     /* give: rounds sent; answer: rounds taken */
    s32 stage;      /* the giver's stage: a stale give is sent back */
    s32 refund;     /* answer: rounds that did not fit, back to the giver */
    u8 gotweapon;   /* answer: the gun was new to the receiver */
    u8 pad[3];
} CoopGive;
void coopGivePress(void);       /* the give_partner key (bondview2.c) */
void coopGiveSend(s32 answer, s32 to, CoopGive *give);
void coopGiveReceive(CoopGive *give, s32 from);
void coopGiveReturned(CoopGive *give, s32 from);
void coopGiveTestDraw(s32 weapon);
void coopGiveTestPress(void);

/* pings (coop_ping.c): the ping key marks the spot, or the guard, under the
 * crosshair for every player, with the pinger's name, for COOP_PING_LIFE
 * ticks. One ping per player; a new one replaces it. Rides the reliable
 * events (COOPEV_PING) to everyone. */
#define COOP_PING_LIFE 600   /* 10 s */
enum { COOPPING_SPOT, COOPPING_ENEMY };
typedef struct
{
    f32 pos[3];     /* where the aim met a wall, floor or the guard */
    s16 room;
    s16 chrnum;     /* COOPPING_ENEMY: the guard, followed while alive; else -1 */
    s32 stage;      /* the pinger's stage: a ping from another one is dropped */
    u8 kind;        /* COOPPING_* */
    u8 pad[3];
} CoopPing;
typedef struct
{
    coord3d pos;
    s32 kind;
    s32 player;     /* who pinged (0 the host) */
    s32 age;        /* ticks since the ping */
    const char *name;
} CoopPingMark;
void coopPingPress(void);       /* the ping key (bondview2.c) */
void coopPingSend(CoopPing *ping);
void coopPingReceive(CoopPing *ping, s32 from);
void coopPingStageStart(void);
void coopPingTick(void);
s32  coopPingAt(s32 i, CoopPingMark *out);  /* the i-th live ping, from 0 */
void coopPingTestPress(s32 pitch);          /* harness: GE_COOP_TESTPING */

/* the weapon menu (weapon_menu.c) */
s32  weapmenuInput(s32 step);
u16  weapmenuFilterButtons(u16 buttons, u16 oldbuttons);
Gfx *weapmenuRender(Gfx *gdl);
s32  weapmenuIsOpen(void);

/* the lobby screen (front.c MENU_COOP_LOBBY) */
enum { COOPLOBBY_OFF, COOPLOBBY_IDLE, COOPLOBBY_BUSY, COOPLOBBY_HOSTING, COOPLOBBY_JOINED };
s32  coopLobbyOpen(void);   /* 1: co-op usable */
void coopLobbyClose(void);
s32  coopLobbyIsSteam(void);
s32  coopLobbyState(void);
s32  coopLobbyIsHost(void);
s32  coopLobbyHasPartner(void);
s32  coopLobbyPartnerReady(void);
s32  coopLobbyPartnerOnScreen(void);
s32  coopLobbySelfReady(void);
void coopLobbySetReady(s32 ready);
const char *coopLobbySelfName(void);
const char *coopLobbyPartnerName(void);     /* the first partner's */
s32  coopLobbyPlayerHere(s32 p);            /* player p (0 the host, up to COOP_MAX_PLAYERS) is in the session */
s32  coopLobbyPlayerReady(s32 p);
s32  coopLobbySelfPlayer(void);
const char *coopPlayerName(s32 p);
unsigned long long coopPlayerId(s32 p);     /* SteamID, 0 off Steam */
void coopLobbyHost(void);
void coopLobbyJoinAny(void);
void coopLobbyJoinFriend(s32 i);
void coopLobbyLeave(void);
s32  coopLobbyFriends(void); /* refreshes the list; how many */
const char *coopLobbyFriendName(s32 i);
s32  coopLobbyFriendHosting(s32 i);
s32  coopLobbyFriendIsPartner(s32 i);
void coopLobbyInvite(s32 i);
void coopLobbyInviteOverlay(void);
s32  coopSessionActive(void);

/* when the host or a joiner leaves (coop.c, host migration) */
s32  coopHostAbortAsks(void);           /* the watch's Abort, confirmed by a host with partners: ask first */
const char *coopHostAbortNextName(void);
void coopHostAbort(s32 handoff);        /* 1: hand the mission over and leave; 0: end it for everyone */
s32  coopHudSessionBanner(char *line1, char *line2, s32 cap);   /* "host not responding" and the like */
s32  coopHudPlayerLost(s32 p);          /* player p's connection dropped: its Bond stands idle */
const char *coopLobbyNotice(void);      /* a line for the lobby screen, NULL when none */
s32  coopHostLeavesCount(void);         /* COOPSET_HOSTLEAVES's rules */
const char *coopHostLeavesName(s32 rule);

/* end-of-mission stats and the campaign record (coop_stats.c) */
enum { COOPSTAT_KILLS, COOPSTAT_SHOTS, COOPSTAT_HITS, COOPSTAT_HEAD, COOPSTAT_DEATHS, COOPSTAT_REVIVED, COOPSTAT_RESPAWNS, COOPSTAT_COUNT };
void coopStatsMissionStart(void);
void coopStatsMissionTick(s32 partnerhere);
void coopStatsFrontTick(void);
void coopStatsDown(void);
void coopStatsNote(s32 stat);
void coopStatsBlameBegin(s32 player); /* host: kills and hits until End are that player's (1..3) */
void coopStatsBlameEnd(void);
void coopStatsExplosionMade(void *explosion);
s32  coopStatsExplosionIsPartner(void *explosion);
void coopStatsApply(const void *payload, s32 from);
void coopStatsRemap(const s32 *newof);  /* host migration: newof[old player] = new number, -1 gone */
s32  coopStatsShown(void);              /* 1: the debrief shows the co-op columns */
s32  coopStatsColumns(void);            /* the debrief's columns (players), up to COOP_MAX_PLAYERS */
s32  coopStatsGet(s32 col, s32 stat);    /* -1 not in yet */
const char *coopStatsName(s32 col);
void coopCampaignMissionEnd(void);
s32  coopCampaignNext(s32 *stage, s32 *difficulty, char *label, s32 labelcap);  /* the host's campaign (cross-save) */
void coopCampaignTick(void);
void coopCampaignApply(const u8 *raw, s32 from);

/* public games and the lobby password (coop_browse.c) */
enum { COOPBROWSE_TYPE_HOST = 1, COOPBROWSE_TYPE_JOIN = 2 };
void coopBrowseTick(s32 inmission);
s32  coopBrowsePublic(void);
void coopBrowseTogglePublic(void);
s32  coopBrowseHasPassword(void);
s32  coopBrowseCount(void);             /* -1 still looking */
const char *coopBrowseName(s32 i);
s32  coopBrowseMembers(s32 i);
s32  coopBrowseLocked(s32 i);
void coopBrowseJoin(s32 i);
s32  coopBrowseChecking(void);
s32  coopBrowseRefused(void);
void coopBrowseTypeStart(s32 kind, s32 forgame);
s32  coopBrowseTyping(void);
s32  coopBrowseTypingFor(void);
void coopBrowseTypedMask(char *out, s32 cap);
s32  coopBrowseHarnessJoin(void);
s32  coopLobbyOnScreen(void);
void coopLobbyJoinLobbyId(unsigned long long lobby);
s32  coopExtVersion(void);

/* photo mode (coop_photo.c) */
void coopPhotoTick(f32 lookdx, f32 lookdy, s32 direct, f32 yawdeg, f32 pitchdeg);
void coopPhotoStop(void);
s32  coopPhotoActive(void);
s32  coopPhotoCount(void);
s32  coopPhotoCamera(struct coord3d *pos, struct coord3d *look, struct coord3d *up);
s32  coopPhotoRoom(u8 *room);
struct coord3d *coopPhotoPos(s32 three);
Gfx *coopPhotoRender(Gfx *gdl);

/* watching a partner while we are down (coop_spectate.c) */
void coopSpectateTick(void);
void coopSpectateStop(void);
s32  coopSpectateTarget(void);
s32  coopSpectateCamera(struct coord3d *pos, struct coord3d *look, struct coord3d *up);
s32  coopSpectateRoom(u8 *room);
struct coord3d *coopSpectatePos(s32 three);

/* how partners look (coop_look.c): the character they picked in the lobby,
 * the guns in their hands with the multiplayer body animations, and their
 * shots (muzzle flash, gun sound, tracer, impact) on everyone else's game.
 * Each game describes its own Bond in a CoopLook carried by every STATE. */
#define COOP_SHOTS_KEPT 4   /* the newest shots in each STATE: 4 ticks of loss before one is missed */
enum { COOPSHOT_MISS, COOPSHOT_BG, COOPSHOT_CHR, COOPSHOT_OBJ };
#define COOPSHOT_LEFT   0x80    /* CoopShot.kind: the left hand's gun */
#define COOPSHOTF_SPARK 1       /* CoopShot.flags: a spark and ricochet where it hit */
#define COOPSHOTF_GLASS 2       /* the glass did not break: the glass sound */
typedef struct
{
    f32 pos[3];     /* where the bullet stopped, on the surface */
    s8 normal[3];   /* that surface's (x 127), for the bullet hole */
    u8 kind;        /* COOPSHOT_*, | COOPSHOT_LEFT */
    s16 room;
    s16 arg;        /* BG / OBJ: the texture, -1 none; CHR: the chrnum hit */
    s16 impact;     /* BG: the bullet hole's impact type, -1 none */
    s8 weapon;
    u8 flags;       /* COOPSHOTF_* */
} CoopShot;

typedef struct
{
    s8 weapon[2];       /* each hand's ITEM_IDS (right, left), ITEM_UNARMED when empty */
    u8 flash;           /* the muzzle flash shows: bit 0 right, bit 1 left */
    u8 crouch;          /* playerGetCrouchPos: 0 squat, 1 crouched, 2 standing */
    f32 speedforwards;  /* the player's own movement, for the MP body animations */
    f32 speedsideways;
    f32 speedtheta;
    f32 aimpitch;       /* the gun's aim up and down (field_2A08), radians */
    f32 aimyaw;         /* and across (field_2A0C) */
    u8 headanim;        /* the player's run flag (headanim): run animation past 0.4 */
    u8 shotseq;         /* the number of shot[0], the newest; shot[i] is shotseq - i */
    u8 character;       /* the lobby's pick (coopCharacter) */
    u8 pad;
    CoopShot shot[COOP_SHOTS_KEPT];
} CoopLook;

/* what a puppet shows: guns given, shots played, the MP animation state */
typedef struct
{
    s8 held[2];         /* the item each hand's gun is, ITEM_UNARMED none */
    u8 shotseq;         /* the newest shot already played */
    u8 seqvalid;
    s32 anim;           /* the animation set (as bondview2.c players_cur_animation) */
    f32 rate;
    f32 lean;           /* the sidestep lean (bondview2.c field_1280) */
} CoopLookPuppet;

void coopLookFill(CoopLook *out);
void coopLookReset(CoopLookPuppet *lp);
void coopLookPuppetTick(CoopLookPuppet *lp, ChrRecord *chr, const CoopLook *look, s32 dead, s32 self);
void coopLookShotBegin(s32 hand, s32 weapon, coord3d *from, coord3d *dir); /* chrprop.c chraiDefaultWeaponFireHandler */
void coopLookShotBg(coord3d *pos, coord3d *normal, s32 impact, s32 room, s32 texture, s32 spark);
void coopLookShotChr(PropRecord *prop, coord3d *pos);           /* chr.c chrHandleBulletHit */
void coopLookShotObj(PropRecord *prop, coord3d *pos, s32 room, s32 texture, s32 glass); /* propobj.c objHit */
void coopLookShotEnd(void);
s32  coopPropIsPartner(PropRecord *prop);
void coopLookTestTick(s32 missionticks);   /* harness: GE_COOP_TESTFIRE, GE_COOP_TESTLOOK */
u16  coopLookTestButtons(u16 buttons);

/* the lobby's character pick: 0 the mission's own Bond, else 1 + an
 * mp_chr_setup index (the multiplayer roster) */
s32  coopCharacter(void);
void coopSetCharacter(s32 c);
s32  coopCharacterStep(s32 c, s32 dir);     /* the next (dir 1) or previous (-1) in the MP pickers' order */
void coopCharacterName(s32 c, char *out, s32 cap);  /* with "\n" */
s32  coopCharacterBody(s32 c, s32 *head);   /* the body and head, -1 for the mission's Bond */
s32  coopPlayerCharacter(s32 p);            /* player p's pick, as the lobby knows it */

/* ---- the feature batch: chat, spectating, objective credit, team times,
 * cross-save (coop_ext.c dispatches; coop.c carries the events) ---------- */
#define COOPEV_EXT_FIRST 20
#define COOPEV_EXT_LAST  31
enum
{
    COOPEV_CHAT = 20,       /* a part of a chat line (coop_chat.c) */
    /* 21 spare */
    COOPEV_OBJDONE = 22,    /* host -> all: objective done, and who gets the credit */
    COOPEV_TEAMTIME = 23,   /* host -> all: the team's time for the mission just won */
    COOPEV_CAMPAIGN = 24    /* host -> joiner: a line of the host's campaign record */
};
typedef struct
{
    coord3d pos;        /* their eyes */
    f32 yaw;            /* radians, model convention (as CoopStatePkt) */
    f32 pitch;          /* the gun's aim up and down, radians */
    s32 dead;
    PropRecord *prop;   /* their puppet, NULL before it is built */
    StandTile *stan;
    s32 weapon;         /* right hand ITEM_IDS */
    f32 health;
} CoopExtView;
void coopExtSend(s32 kind, s32 to, const void *payload, s32 len);
s32  coopExtSelf(void);
s32  coopExtIsHost(void);
u32  coopExtMissionTicks(void);
u32  coopExtHostMissionTicks(void);
s32  coopExtRemoteView(s32 p, CoopExtView *out);
void coopExtTick(s32 inmission);
void coopExtApply(s32 kind, const u8 *raw, s32 from);
/* the features' own unreliable packets: u32 magic, u16 version, u16 type first, as coop.c's */
#define COOPPKT_EXT_FIRST 10
#define COOPPKT_EXT_LAST  11
enum { COOPPKT_TEAMTIME = 10 };
void coopExtSendPacket(void *pkt, s32 len, s32 to);    /* fills magic and version */
void coopExtPacket(const void *pkt, s32 len, s32 from);

/* team mission times (coop_teamtime.c): in co-op the mission's time is the
 * host's, for the best time, the target time cheats and the debrief */
s32  coopTeamTimer(void);           /* 60 Hz ticks: the team's time now, or as the mission ended */
void coopTeamTimeFreeze(void);      /* coopMissionComplete: the mission is ending */
void coopTeamTimeSend(void);        /* host: to everyone, before MISSIONEND */
void coopTeamTimeApply(const u8 *raw, s32 from);
void coopTeamTimeTick(s32 inmission);
void coopTeamTimeUsed(s32 folder, s32 stage, s32 difficulty, s32 timer); /* file.c: the debrief saved with this time */

/* chat (coop_chat.c): the chat key opens a line, Return sends it to everyone,
 * Escape drops it; lines and toasts fade out over the HUD and the lobby */
void coopChatPress(void);
s32  coopChatTyping(void);
s32  coopChatEatEscape(void);   /* 1: this Escape was the chat's, not the pause's */
void coopChatTick(s32 inmission);
void coopChatApply(const u8 *raw, s32 from);
void coopFeedToast(const char *text, s32 player);   /* centred, the first line in the player's colour (-1 white); '|' starts a second line */
Gfx *coopFeedRender(Gfx *gdl);                       /* in play (coop_hud.c) */
s32  coopFeedLines(s32 maxlines, char lines[][80], s32 *players);  /* the newest chat lines as typed, for the lobby */
void coopChatTypedLine(char *out, s32 cap);         /* the line being typed */

/* objective credit (coop_objective.c): the host names who completed each
 * objective and every game shows a toast for a partner's */
void coopObjNoteActor(s32 player, s32 damage);  /* host: something player did just changed the world */
void coopObjStatusChanged(s32 index, s32 letter, s32 status);  /* objective_status.c */
void coopObjTick(s32 inmission);
void coopObjApply(s32 kind, const u8 *raw, s32 from);

#endif
