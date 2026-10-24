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
s32  coopAltBondBegin(void); /* 1: "Bond" is now the partner, call coopAltBondEnd */
void coopAltBondEnd(void);
void coopAiCommand(u8 *list, s32 offset, ChrRecord *self);
s32  coopDeadTick(void); /* 1: co-op owns this death (waiting or respawned) */
void coopRespawnPlayerAt(coord3d *pos, StandTile *stan, f32 yaw, s32 weapon);
s32  coopClientForwardShot(ChrRecord *chr, s32 hitpart, coord3d *vector, s32 weaponid);

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
} CoopHudPartner;
s32  coopHudPartner(CoopHudPartner *out);
s32  coopHudRespawnEta(void);
s32  coopHudDownRule(void);
s32  coopHudReviveHold(void);
s32  coopHudBledOut(void);

/* what happens when a player goes down (the host picks in the lobby) */
enum { COOPDOWN_BOTH, COOPDOWN_TIMER, COOPDOWN_REVIVE };
s32  coopDownRule(void);
void coopSetDownRule(s32 rule);
Gfx *coopHudRender(Gfx *gdl);

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
const char *coopLobbyPartnerName(void);
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

#endif
