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
    COOPSET_SPARE7,
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
s32  coopStatsShown(void);              /* 1: the debrief shows the co-op columns */
s32  coopStatsColumns(void);            /* the debrief's columns (players), up to COOP_MAX_PLAYERS */
s32  coopStatsGet(s32 col, s32 stat);    /* -1 not in yet */
const char *coopStatsName(s32 col);
void coopCampaignMissionEnd(void);
s32  coopCampaignNext(s32 *stage, s32 *difficulty, char *label, s32 labelcap);

#endif
