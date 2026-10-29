#ifndef _COOP_H_
#define _COOP_H_
#include <ultra64.h>
#include <bondtypes.h>

void coopStageStart(void);
void coopTick(void);
s32  coopPropIsGhost(PropRecord *prop);
s32  coopChrNearPartner(ChrRecord *chr);     /* host: keep animating a guard a partner may see */

extern PropRecord *g_CoopPropOverride;
s32  coopChrTickBegin(ChrRecord *chr); /* 0: skip (client mirrors the host) */
void coopChrTickEnd(void);
void coopChrStimulus(ChrRecord *chr);
s32  coopChrAltBegin(ChrRecord *chr, s32 k);     /* host: a failed "Bond" check, try the k-th other player (1: they are Bond now) */
void coopChrAltEnd(ChrRecord *chr);              /* that try failed too: the guard's own target again */
void coopChrAltKeep(ChrRecord *chr, s32 stimulus);  /* it passed: that player is the guard's target */
s32  coopChrAimedAtByBond(ChrRecord *chr);       /* sub_GAME_7F0333F8 for whoever is Bond in this tick */
s32  coopChrOnPartnerScreen(ChrRecord *chr);     /* host: a partner can see this chr (chrai.c "if I'm on screen") */
void coopCollideMover(PropRecord *prop, s32 on); /* chrSetMoving: prop is being moved or tested from */
s32  coopCollideSkip(PropRecord *prop);          /* 1: prop blocks nothing right now (down, or inside the mover) */
s32  coopTargetIsRemote(void);
void coopGuardTankFlags(s32 flag);
f32  coopTargetDuckingHeight(void);
s32  coopTargetDamageShowTime(void);
s32  coopTargetIsDead(void);
f32  coopTargetYaw(void);                        /* bondviewGetPlayerYawRadians for whoever is Bond in this tick */
void coopPartnerShotNearMiss(ChrRecord *shooter, coord3d *from, coord3d *end); /* host: a partner's bullet passed close to a guard */
s32  coopTargetRecordDamage(f32 damage, f32 angle);
void coopDoorStateChanged(DoorRecord *door, s32 state);
s32  coopObjDamage(ObjectRecord *obj, f32 damage, coord3d *pos, s32 item, s32 player); /* 1: skip */
s32  coopSkipExplosionDamage(ChrRecord *chr);
void coopPickedUp(ObjectRecord *obj, s32 kept);
s32  coopScriptGiveBegin(void);        /* chrai.c AI_BondCollectObject: stand a down host up for it; pass the result to ...End */
void coopScriptGiveEnd(s32 down);
s32  coopPeerHolds(ObjectRecord *obj);
void coopMissionComplete(void);
s32  coopFrontTick(void);
s32  coopAltBondBegin(s32 k); /* 1: "Bond" is now the k-th living partner (from 0), call coopAltBondEnd */
void coopAltBondEnd(void);
PropRecord *coopAutogunTarget(PropRecord *gun, f32 aimdist, f32 rot, f32 arcmin, f32 arcmax); /* propobj.c: who a drone gun aims at */
s32  coopOwnBondCounts(void);         /* 0: a level script's own-Bond check skips the host's body (down, a partner standing) */
s32  coopLevelFallCheckOff(f32 y);   /* 1: a level script's fall check that co-op leaves to each player's own game */
s32  coopBondHasItem(s32 item);          /* chrai.c: whoever is Bond now holds this item (a partner's from STATE) */
f32  coopBondHealth(void);               /* and their health, 0..1 */
void coopBondItemLog(s32 item);          /* log: that check passed for a partner */
s32  coopCctvWatches(void);              /* propobj.c: 0 on a joiner, the host's cameras raise the alarm */
void coopCctvAlarm(PropRecord *seen);    /* log: the Bond a camera raised the alarm on */
void coopAltBondCredit(void *script, s32 k);   /* a script's "either player" check passed: k 0 for our Bond, else partner k-1 */
void coopObjActivatedPass(void *script, void *obj);   /* that script found obj activated: its flags are the presser's */
void coopScriptSetsFlags(void *script, void *chr);     /* that script (a chr's, or chr NULL) sets stage flags: it was whoever passed its last check */
void coopAiCommand(u8 *list, s32 offset, ChrRecord *self);
s32  coopAiSkip(u8 *list, s32 offset, ChrRecord *self);   /* joiner: step over our late copy of the host's ending commands */
s32  coopDeadTick(void); /* 1: co-op owns this death (waiting or respawned) */
void coopRespawnPlayerAt(coord3d *pos, StandTile *stan, f32 yaw, s32 weapon);
void coopTeleportPlayerTo(coord3d *pos, StandTile *stan, f32 yaw);    /* bondview2.c */
s32  coopCinemaFocus(coord3d *out);                                     /* bondview2.c: a scripted cinema's pad */
s32  coopKeepBodyList(ChrRecord *chr, AIRecord *list);  /* joiner: the host's ending list already runs on our body */
s32  coopScriptSpawnsLeftToHost(void);  /* joiner: a level script's spawn is the host's, mirrored from its snapshots */
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
    s32 revivable;      /* down, and holding Use beside them brings them back */
    s32 revivehold;     /* down: ticks of COOP_REVIVE_HOLD someone has held Use over them */
    s32 out;            /* down for good this mission: no lives left, or bled out */
    const char *name;
    s32 player;         /* their player number, 0 the host */
} CoopHudPartner;
s32  coopHudPartner(CoopHudPartner *out);           /* the first partner */
s32  coopHudPartnerAt(s32 i, CoopHudPartner *out);  /* the i-th, from 0 */
s32  coopHudRespawnEta(void);
s32  coopHudDownRule(void);
s32  coopHudReviveHold(void);   /* ticks of COOP_REVIVE_HOLD */
#define COOP_REVIVE_HOLD 150       /* a partner holds Use beside where we fell for 2.5 s */
#define COOP_REVIVE_DIST 160.0f    /* ... within this of us (xz), and 250 up or down */
s32  coopBleedOutTicks(void);    /* revive only: this long down and you are out of this mission (COOPSET_BLEEDOUT) */
s32  coopHudBledOut(void);      /* out until the mission ends: COOPOUT_BLEDOUT or COOPOUT_LIVES, 0 not */
s32  coopHudBleedOut(void);     /* revive only: ticks left before we bleed out, -1 none */
s32  coopHudLives(void);        /* our lives left this mission, -1 unlimited */
enum { COOPOUT_NONE, COOPOUT_BLEDOUT, COOPOUT_LIVES };

/* revive, cutscenes and the results screen: their own events (coop.c carries
 * them; coopDesignApply in coop_cutscene.c hands them out) */
#define COOPEV_DESIGN_FIRST 56
#define COOPEV_DESIGN_LAST  63
enum
{
    COOPEV_CINEMA = 56,     /* host -> all: CoopCinemaMsg, the host's intro is over */
    COOPEV_VOTE = 57,       /* to the host: a vote; host -> all: the count (coop_vote.c) */
    COOPEV_LIVES = 58,      /* to the host: our lives this mission; host -> a returning joiner: what it had left (coop.c) */
    COOPEV_SEEN = 59        /* to the host: guards our screen has drawn (CHRFLAG_HAS_BEEN_ON_SCREEN, coop.c) */
};
#define COOP_ALL_PLAYERS 0xFF   /* coopDesignSend's `to`: everyone */
void coopDesignSend(s32 kind, s32 to, const void *payload, s32 len);
void coopRunHostScript(u8 *list);   /* host: run script commands as the level would (relayed) */
void coopCutsceneStandUp(coord3d *pos, StandTile *stan, f32 yaw);
void coopDesignApply(s32 kind, const u8 *raw, s32 from);
s32  coopDesignSelf(void);
s32  coopDesignIsHost(void);

/* cutscenes (coop_cutscene.c) */
s32  coopCutsceneTick(void);    /* every tick: 1 while an intro or a scripted cinema runs */
void coopCutsceneStageStart(void);
s32  coopCutsceneActive(void);  /* intro or scripted cinema: partners hidden */
void coopCutscenePuppetAnim(ChrRecord *chr);
void coopCutsceneApply(const u8 *raw, s32 from);
s32  coopIntroHostDone(void);   /* bondview2.c: end our intro, the host's is over */

/* votes after a mission and for skipping a cutscene (coop_vote.c) */
enum { COOPVOTE_REPLAY, COOPVOTE_NEXT, COOPVOTE_LOBBY, COOPVOTE_COUNT };
enum { COOPSKIP_INTRO = 1, COOPSKIP_ENDING };
void coopVoteTick(s32 inmission);
void coopVoteApply(const u8 *raw, s32 from);
s32  coopVoteShown(void);           /* the debrief's WHAT NEXT page is up */
void coopVotePick(s32 choice);
s32  coopVoteOf(s32 player);        /* COOPVOTE_*, 0xFF none */
s32  coopVoteOutcome(void);         /* -1 open */
s32  coopVoteNext(void);            /* the next mission's stage, -1 none */
s32  coopVoteLeft(void);            /* ticks until it is decided anyway, -1 no clock */
const char *coopVoteName(s32 choice);
s32  coopSkipPress(s32 kind);       /* 1: skip now; 0: counted as a vote */
s32  coopSkipPassed(s32 kind);
s32  coopSkipVotes(s32 *need, s32 *mine);
s32  coopSkipWaitingFor(s32 i);
s32  coopSkipTestPress(void);
void coopVoteRenumber(const s32 *newof);  /* coop.c: host migration's renumbering */
s32  coopVoteThisStage(void);       /* front.c: the mission whose debrief is up, -1 none */
s32  coopVoteNextStage(void);       /* front.c: the mission after it, -1 none (failed, or the last) */
void coopVoteStartMission(s32 stage);   /* front.c: host, start it; the others follow */
void coopVoteToLobby(void);         /* front.c */
Gfx *coopHudSkipVote(Gfx *gdl);     /* coop_hud.c: the count in a cutscene */

/* reviving a downed partner (coop_revive.c) */
void coopReviveTick(s32 missionticks);
s32  coopReviveTarget(void);    /* STATE: the player we hold Use over, + 1; 0 none */
s32  coopReviveHud(CoopHudPartner *out, s32 *hold);

/* Lobby settings the host picks and the joiner's game copies (coopSetting).
 * Append only: 0 must stay each one's default. */
enum
{
    COOPSET_DIFFICULTY,     /* co-op difficulty: 0 normal (coop_difficulty.c) */
    COOPSET_FRIENDLYFIRE,   /* 0 off: shots and blasts pass partners by; 1 full; 2 reduced (half) */
    COOPSET_VSMODE,         /* bit 0: versus (coop_versus.c) instead of co-op; bits 4-7: who played the last match */
    COOPSET_VSARENA,        /* versus: the arena, a multi_stage_setups index (0 random) */
    COOPSET_VSSCENARIO,     /* versus: the scenario (SCENARIO_*) */
    COOPSET_VSLENGTH,       /* versus: Game Length, a multi_game_lengths index */
    COOPSET_VSWEAPONS,      /* versus: the weapon set (setMPWeaponSet) */
    COOPSET_HOSTLEAVES,     /* 0: the next player hosts and the mission goes on; 1: everyone back to the lobby */
    COOPSET_LIVES,          /* lives each player starts a mission with, a coopLivesName index (0 unlimited) */
    COOPSET_BLEEDOUT,       /* revive only: time down before bleeding out, a coopBleedOutName index (0 60 s) */
    COOPSET_RESPAWN,        /* the timer's wait, a coopRespawnName index (0 3-20 s, sooner when it is quiet) */
    COOPSET_SCALING,        /* guards by player count: COOPSCALE_* (0 auto) */
    COOPSET_GUARDAI,        /* the guards' AI: COOPAI_CLASSIC (0) or COOPAI_SMARTER (coop_ai.c) */
    COOPSET_MAX = 16
};
s32  coopSetting(s32 which);
void coopSetSetting(s32 which, s32 value);

/* smarter guards (coop_ai.c): COOPSET_GUARDAI in co-op, the guard_ai Port Option in solo */
enum { COOPAI_CLASSIC = 0, COOPAI_SMARTER = 1 };
s32  coopAiSmarter(void); s32 coopAiSmarterOption(void); void coopAiSetSmarterOption(s32 mode);                   /* this mission's guards are the smarter ones */
s32  coopAiTick(ChrRecord *chr);            /* chrai.c ai(): 1 when the smarter AI ran the chr's tick */
f32  coopAiHearScale(ChrRecord *chr, f32 dist, f32 range);  /* chr.c: hearing through walls */
s32  coopGuardAiCount(void);
const char *coopGuardAiName(s32 mode);
s32  coopChrCurrentTarget(void);            /* host, in a guard's tick: the player that is Bond, 0 ours */
s32  coopTargetablePlayer(s32 t);           /* host: player t is in the mission and alive */
PropRecord *coopTargetablePlayerProp(s32 t); /* host: that player's body while coopTargetablePlayer, else NULL */

/* co-op difficulty (coop_difficulty.c): COOPSET_DIFFICULTY's modes */
s32  coopLobbyPlayerCount(void);          /* players in the session, us included */
s32  coopDifficultyCount(void);
const char *coopDifficultyName(s32 mode);
f32  coopDifficultyHealthMul(void);
s32  coopDifficultyExtraSlots(void);
void coopDifficultyStageStart(void);
void coopDifficultyTick(void);
f32  coopDifficultyGuardHealthMul(void);  /* the host's guard health multiplier, on any game */
enum { COOPSCALE_AUTO, COOPSCALE_OFF, COOPSCALE_AS3, COOPSCALE_AS4, COOPSCALE_COUNT };
const char *coopScalingName(s32 mode);
s32  coopScalingNext(s32 mode, s32 dir);  /* the lobby row's order: Off, Auto, 3 Players, 4 Players */

/* friendly fire (coop.c): COOPSET_FRIENDLYFIRE's levels */
enum { COOPFF_OFF, COOPFF_FULL, COOPFF_REDUCED };
f32  coopFriendlyFireScale(void);         /* 0 off, 0.5 reduced, 1 full; always 0 in versus */
const char *coopFriendlyFireName(s32 level);
s32  coopFriendlyFireNext(s32 level, s32 dir);   /* the lobby row's order: Off, Reduced, Full */

/* what happens when a player goes down (the host picks in the lobby) */
enum { COOPDOWN_BOTH, COOPDOWN_TIMER, COOPDOWN_REVIVE };
s32  coopDownRule(void);
void coopSetDownRule(s32 rule);
s32  coopLivesCount(void);              /* COOPSET_LIVES's choices */
const char *coopLivesName(s32 i);
s32  coopBleedOutCount(void);           /* COOPSET_BLEEDOUT's */
const char *coopBleedOutName(s32 i);
s32  coopRespawnCount(void);            /* COOPSET_RESPAWN's */
const char *coopRespawnName(s32 i);
Gfx *coopHudRender(Gfx *gdl);
u32  coopHudColour(s32 player);         /* each player's colour, as their tag and radar dot */

/* co-op screens (coop_screens.c): the mission card while everyone loads in,
 * the reconnecting panel, and the lobby's error panel */
s32  coopHoldWorld(void);               /* coop.c: 1 while the card holds the stage still (lv.c lvlTick) */
s32  coopHoldInfo(u8 *arrived, s32 *secs);  /* coop.c: 1 while it is up; who is in (bits) and the host's seconds left */
Gfx *coopScreensRender(Gfx *gdl);       /* lv.c lvlRender, after the views */
void coopScreenError(const char *title, const char *text);  /* text: up to three lines split at '\n' */
s32  coopScreenErrorUp(void);
s32  coopScreenErrorInput(void);        /* the lobby's input: 1 while the panel takes the keys */
Gfx *coopScreenErrorRender(Gfx *gdl);

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
enum { COOPSTAT_KILLS, COOPSTAT_SHOTS, COOPSTAT_HITS, COOPSTAT_HEAD, COOPSTAT_DEATHS, COOPSTAT_REVIVED, COOPSTAT_RESPAWNS, COOPSTAT_REVIVES, COOPSTAT_OBJECTIVES, COOPSTAT_DAMAGE, COOPSTAT_FRIENDLY, COOPSTAT_COUNT };
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
s32  coopStatsSelf(s32 stat);            /* our own column's number, -1 not in yet */
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
s32  coopBrowseInMission(s32 i);
void coopBrowseJoin(s32 i);
s32  coopBrowseChecking(void);
s32  coopBrowseRefused(void);
const char *coopBrowseRefusedText(void);
const char *coopBrowseRefusedHint(void);
void coopBrowseTypeStart(s32 kind, s32 forgame);
s32  coopBrowseTyping(void);
s32  coopBrowseTypingFor(void);
void coopBrowseTypedMask(char *out, s32 cap);
s32  coopBrowseHarnessJoin(void);
s32  coopLobbyOnScreen(void);
s32  coopEnabled(void);

/* rich presence and outside joins (coop_presence.c; port/src/coop_presence.c
 * talks to Steam and Discord) */
void coopPresenceGameTick(s32 instage);     /* menus (front.c menu_init) and stages (coopTick) */
const char *coopMissionName(s32 stage);     /* coop_stats.c: "Dam", NULL when not a solo mission */
void coopVsPresenceNames(s32 stage, char *arena, s32 arenacap, char *scenario, s32 scenariocap);  /* front.c */
s32  coopLobbyDiscordAsks(void);            /* host: Discord Ask to Join requests waiting */
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
void coopRestoreMusic(void);   /* bondview2.c: the level music after a death */
void coopSpectateDropIn(s32 on);
s32  coopSpectateTarget(void);     /* whose eyes we look out of, -1 none (or the free camera) */
s32  coopSpectateHud(s32 *who);    /* 1 watching *who, 2 the free camera, 0 neither */
void coopSpectateMouse(f32 *dx, f32 *dy, s32 *direct, f32 *yawdeg, f32 *pitchdeg);   /* bondview2.c */
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
    u8 cue;             /* the newest gun sound cue (coop_audio.c): bits 0-4 its number, 5-6 COOPCUE_* (was padding, 0) */
    CoopShot shot[COOP_SHOTS_KEPT];
} CoopLook;

/* what a puppet shows: guns given, shots played, the MP animation state */
typedef struct
{
    s8 held[2];         /* the item each hand's gun is, ITEM_UNARMED none */
    u8 shotseq;         /* the newest shot already played */
    u8 seqvalid;
    u8 cue;             /* the newest sound cue already played (CoopLook.cue) */
    u8 cuevalid;
    s32 anim;           /* the animation set (as bondview2.c players_cur_animation) */
    f32 rate;
    f32 lean;           /* the sidestep lean (bondview2.c field_1280) */
} CoopLookPuppet;

void coopLookFill(CoopLook *out);
void coopLookReset(CoopLookPuppet *lp);
void coopLookPuppetTick(CoopLookPuppet *lp, ChrRecord *chr, const CoopLook *look, s32 dead, s32 self);
void coopLookBodyTick(CoopLookPuppet *lp, ChrRecord *chr, const CoopLook *look, s32 dead, s32 self);   /* versus: Rare's code poses it */
void coopLookShotBegin(s32 hand, s32 weapon, coord3d *from, coord3d *dir); /* chrprop.c chraiDefaultWeaponFireHandler */
void coopLookShotBg(coord3d *pos, coord3d *normal, s32 impact, s32 room, s32 texture, s32 spark);
void coopLookShotChr(PropRecord *prop, coord3d *pos, s32 hitpart);           /* chr.c chrHandleBulletHit */
void coopLookShotObj(PropRecord *prop, coord3d *pos, s32 room, s32 texture, s32 glass); /* propobj.c objHit */
void coopLookShotEnd(void);
s32  coopPropIsPartner(PropRecord *prop);
void coopLookTestTick(s32 missionticks);   /* harness: GE_COOP_TESTFIRE, GE_COOP_TESTLOOK */
u16  coopLookTestButtons(u16 buttons);

/* partners' audio (coop_audio.c): their gun handling played where they are,
 * a ping's click from its direction, and a sting when a partner goes down
 * and a softer one when they are back up. Original sounds only, at the
 * game's own sound and music volumes. */
enum { COOPCUE_NONE, COOPCUE_DRAW, COOPCUE_RELOAD };
void coopAudioCue(s32 kind, s32 item);      /* gunfire.c: our Bond drew a gun or reloaded (the sound it played) */
u8   coopAudioCueByte(void);                /* coop_look.c coopLookFill: CoopLook.cue */
void coopAudioPuppetCue(CoopLookPuppet *lp, ChrRecord *chr, const CoopLook *look);   /* coop_look.c: play a partner's new cue there */
void coopAudioPing(coord3d *pos, s32 self);         /* coop_ping.c: a ping arrived */
void coopAudioPartnerDowned(s32 player);
void coopAudioPartnerRevived(s32 player);
void coopAudioTick(void);                   /* coop.c coopTick, in a mission: the stings */
void coopAudioStageStart(void);

/* the lobby's character pick: 0 the mission's own Bond, else 1 + an
 * mp_chr_setup index (the multiplayer roster) */
s32  coopCharacter(void);
void coopSetCharacter(s32 c);
s32  coopCharacterStep(s32 c, s32 dir);     /* the next (dir 1) or previous (-1) in the MP pickers' order */
s32  coopCharacterPos(s32 c);               /* a pick's place in that order, 0 the mission's Bond */
s32  coopCharacterAt(s32 pos);              /* the pick at a place in it (wraps) */
s32  coopCharacterCount(void);
void coopCharacterName(s32 c, char *out, s32 cap);  /* with "\n" */
s32  coopCharacterBody(s32 c, s32 *head);   /* the body and head, -1 for the mission's Bond */
s32  coopPlayerCharacter(s32 p);            /* player p's pick, as the lobby knows it */

/* the picks as 3D figures for the lobby (coop_charsel.c) */
void coopFigureBeginFrame(void);
Gfx *coopFigureDraw(Gfx *DL, s32 pick, s32 x, s32 y, s32 w, s32 h, f32 yaw, s32 bust);

/* online versus (coop_versus.c): the multiplayer game, one player per PC.
 * Local player indices (g_playerPointers) differ per game; the wire uses the
 * session's player numbers. */
#define COOPPKT_VERSUS 16
#define COOP_ALL 0xFF               /* as coop.c: an event for everyone */
#define COOP_PUPPET_CHRNUM 30000    /* as coop.c: + player number, a player's body's chrnum on the others' games */
enum { COOPEV_VSHIT = 32, COOPEV_VSKILL, COOPEV_VSEND, COOPEV_VSITEM, COOPEV_VSBOOM, COOPEV_VSPICK };
typedef struct
{
    f32 pos[3];
    f32 yaw;
    s32 dead;
    f32 health;
    f32 armour;
    CoopLook look;
} CoopRemoteInfo;
s32  coopRemoteInfo(s32 p, CoopRemoteInfo *out);   /* coop.c: player p's newest STATE in this stage */
void coopSendVersusEvent(s32 kind, s32 to, const void *payload, s32 len);
void coopSendVersusPkt(void *pkt, s32 len);
u32  coopMatchSerial(void);                         /* coop.c: the host's mission number, as both know it */
s32  coopVersus(void);                  /* 1 in an online versus match */
s32  coopVsViewCount(void);             /* 1 in one (one view, ours), else getPlayerCount() */
s32  coopVsRemote(s32 li);              /* local player li is another PC's: no view, no tick */
s32  coopVsNetOfLocal(s32 li);
s32  coopVsLocalOfNet(s32 p);
s32  coopVsPlayers(void);
s32  coopVersusRoster(u32 mask, u32 serial);
void coopVersusClear(void);
void coopVersusRenumber(const s32 *newof);          /* host migration: old session numbers to new (-1 gone) */
u32  coopVersusSerial(void);
void coopVersusStageStart(void);
void coopVersusTestInput(s8 *stick_x, s8 *stick_y, u16 *buttons);   /* bondview2.c MoveBond: GE_VS_WANDER */
s32  coopVersusStartPad(s32 li, s32 count);
s32  coopVersusDamage(f32 damage, f32 vx, f32 vz, s32 attacker, s32 armour);   /* 1: not ours to deal */
void coopVersusBlast(s32 on);
void coopVersusKilled(s32 victim, s32 killer, s32 gg);
void coopVersusExplosion(PropRecord *source, coord3d *pos, s32 type, u8 *rooms);
void coopVersusPickedUp(ObjectRecord *obj);   /* coop.c coopPickedUp: we took something in a versus arena */
void coopVersusApplyEvent(s32 kind, s32 from, const void *raw);
void coopVersusMatchOver(void);
s32  coopVersusPktSize(void);
void coopVersusRecvPkt(void *buf, s32 player);
void coopVersusTick(void);
void coopVsStartMatch(s32 stage);          /* front.c: this PC's half of the match, then into the arena */

/* ---- the feature batch: chat, spectating, objective credit, team times,
 * cross-save (coop_ext.c dispatches; coop.c carries the events) ---------- */
#define COOPEV_EXT_FIRST 20
#define COOPEV_EXT_LAST  31
enum
{
    COOPEV_CHAT = 20,       /* a part of a chat line (coop_chat.c) */
    COOPEV_DROPREST = 21,   /* host -> all: where a guard's drop came to rest (coop_items.c) */
    COOPEV_OBJDONE = 22,    /* host -> all: objective done, and who gets the credit */
    COOPEV_TEAMTIME = 23,   /* host -> all: the team's time for the mission just won */
    COOPEV_CAMPAIGN = 24,   /* host -> joiner: a line of the host's campaign record */
    COOPEV_THROW = 25,      /* to all: we threw something (coop_items.c), first half */
    COOPEV_THROWMORE = 26,  /* its second half */
    COOPEV_ITEMGONE = 27,   /* to all: a thrown or dropped item went off or was picked up */
    COOPEV_DROP = 28,       /* host -> all: a guard dropped its gun */
    COOPEV_ITEMSTUCK = 29   /* to all: where our thrown item stuck or landed (coop_items.c) */
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
s32  coopExtSetupOfs(void *rec);         /* a setup object's byte offset in the prop defs, -1 not one */
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
enum { COOPPKT_TEAMTIME = 10, COOPPKT_TANK = 11 };
void coopExtSendPacket(void *pkt, s32 len, s32 to);    /* fills magic and version */
void coopExtPacket(const void *pkt, s32 len, s32 from);

/* the Runway tank (coop_tank.c): the driver's game sends where it is */
void coopTankTick(s32 inmission);
void coopTankPacket(const void *raw, s32 len, s32 from);
s32  coopTankDriving(s32 player);           /* a partner is in a tank */
s32  coopTankTaken(PropRecord *prop);       /* a partner is driving this tank */
s32  coopTankTestTap(void);                 /* GE_COOP_TESTTANK's B press */

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
enum { COOPKEYS_NONE, COOPKEYS_STEAM, COOPKEYS_GRID };
s32  coopPadKeysOpen(const char *title, s32 maxlen);   /* a pad player's keyboard for a text box (coop_chat.c) */
void coopPadKeysClose(s32 keys);
s32  coopChatOnGrid(void);
s32  coopBrowseOnGrid(void);    /* the password box is on the on-screen keyboard */      /* the chat line is on the on-screen keyboard, which has the pad */
Gfx *coopChatGridRender(Gfx *gdl);
s32  coopChatTyping(void);
s32  coopChatEatEscape(void);   /* 1: this Escape was the chat's, not the pause's */
void coopChatTick(s32 inmission);
void coopChatTickStill(void);   /* the open line while the stage clock is stopped */
void coopChatApply(const u8 *raw, s32 from);
#define COOPTOAST_ALERT (-2)    /* a toast's "player" for session news (host left, ...): red */
void coopFeedToast(const char *text, s32 player);   /* bottom left, the lead in the player's colour (-1 white); '|' starts the rest */
void coopFeedClock(void);   /* the feed's clock while co-op is off */
Gfx *coopFeedRender(Gfx *gdl);                       /* in play (coop_hud.c) */
#define COOP_FEED_TEXT 144     /* a chat line: a name, ": " and the most a message holds */
s32  coopFeedLines(s32 maxlines, char lines[][COOP_FEED_TEXT], s32 *players);  /* the newest chat lines as typed, for the lobby */
void coopChatTypedLine(char *out, s32 cap);         /* the line being typed */

/* thrown and dropped items in every world (coop_items.c) */
void coopItemsStageStart(void);
void coopItemsTick(s32 missionticks);                  /* harness: GE_COOP_TESTAIM */
void coopItemsApply(s32 kind, const u8 *raw, s32 from);
void coopItemsThrown(ObjectRecord *obj, coord3d *pos, Mtxf *rot, coord3d *vel, Mtxf *spin);    /* gun.c */
s32  coopItemsChrDropping(ChrRecord *chr);              /* chr.c: 1 keeps a joiner's guard from dropping on its own */
void coopItemsChrDropped(ChrRecord *chr, PropRecord *prop);
void coopItemsExplodeBegin(PropRecord *prop);           /* propobj.c propExplode */
void coopItemsExplodeEnd(void);
s32  coopItemsExploding(void);
s32  coopItemsExplodingOwner(void);
s32  coopItemsExplodingOurRound(void);               /* a rocket or grenade round we fired is going off */
s32  coopItemsIsPartners(ObjectRecord *obj);            /* a partner's remote mine: not our detonator's */
s32  coopItemsEmbedPending(ObjectRecord *obj);          /* propobj.c: stuck by a partner's word, matrix still to work out */
s32  coopItemsUnsettled(PropRecord *prop);             /* propobj.c weaponFindThrown: a partner's throw, still in the air for them */
void coopItemsPickedUp(ObjectRecord *obj);
void coopItemsPlayerDropped(PropRecord *prop);   /* propobj.c drop_inventory: versus, what our death dropped */
s32  coopInMissionNow(void);
void coopObjKnockChildren(s32 haschildren);             /* propobj.c objHit */
void coopObjKnocked(ObjectRecord *obj, s32 bounce, coord3d *viewdir);
void coopObjUsed(ObjectRecord *obj);                     /* propobj.c propobjInteract */
void coopObjGadgetUsed(ObjectRecord *obj, s32 item);       /* gunfire.c: a gadget used on it */
void coopGadgetCheck(ObjectRecord *obj, void *script);      /* chrai.c AI_IFBondUsedGadgetOnObject passed */
s32  coopGadgetItem(void *script);                           /* chrai.c AI_IFBondHasItemEquipped: a partner's gadget, -1 ours */
void coopAsciiName(char *out, s32 cap, const char *in, s32 upper);   /* a Steam name for the game's fonts */

/* voice chat (coop_voice.c): Steam Voice in COOPPKT_VOICE packets, heard from
 * where the talker stands; push to talk (V) or an open mic ([display]
 * voice_mode), per-player mute on this game alone */
#define COOPPKT_VOICE 30    /* 30..31 kept for voice */
void coopVoiceTick(s32 inmission);
void coopVoiceReceive(const void *raw, s32 len);
void coopVoiceRenumber(const s32 *newof);
s32  coopVoiceAvailable(void);      /* in a Steam session with voice, and voice_mode not off */
s32  coopVoiceSelfTalking(void);    /* our words are going out now */
s32  coopVoiceSelfOpen(void);       /* the mic is open (push to talk held, or open mic) */
s32  coopVoiceTalking(s32 p);       /* player p's voice is reaching us now */
s32  coopVoiceMuted(s32 p);
void coopVoiceSetMuted(s32 p, s32 muted);
s32  coopVoiceMuteRowPlayer(s32 k); /* the watch's k-th mute row: which player, -1 none */
s32  coopVoiceMuteRowMask(void);

/* objective credit (coop_objective.c): the host names who completed each
 * objective and every game shows a toast for a partner's */
void coopObjNoteActor(s32 player, s32 damage);  /* host: something player did just changed the world */
void coopObjNoteSelf(void);     /* this game's own Bond did it: a photo, a room, a pickup, ... */
void coopObjNoteUse(s32 player);    /* host: player used or planted something; credit for a few seconds */
void coopObjStatusChanged(s32 index, s32 letter, s32 status);  /* objective_status.c */
void coopObjTick(s32 inmission);
void coopObjApply(s32 kind, const u8 *raw, s32 from);

/* crash and bug reports (port/src/port_report.c): the co-op state as text */
s32  coopDebugDump(char *buf, s32 cap);
void coopCrashBye(void);   /* port_report.c: the game crashed, tell the partners */

/* accessibility (coop_access.c) */
enum { COOPCAP_GUNFIRE, COOPCAP_EXPLOSION, COOPCAP_DOOR, COOPCAP_ALARM, COOPCAP_COUNT };
u32  coopPlayerColour(s32 player);              /* RGBA, alpha 0xFF; the colour-blind set when it is on */
f32  coopTextScale(void);                       /* Text Size: 1.0 to 2.0 */
Gfx *coopTextScaleBegin(Gfx *gdl, f32 x, f32 y);   /* co-op text scaled about (x, y); HUD Scale included */
Gfx *coopTextScaleEnd(Gfx *gdl);
s32  coopScreenShakeOn(void);
void coopCaption(s32 kind, coord3d *pos);       /* a sound worth a caption happened at pos */
void coopCaptionChrGunfire(ChrRecord *chr);     /* chraction.c: a chr's gun went off */
Gfx *coopCaptionsRender(Gfx *gdl);
void coopCaptionHarness(void);


/* netcode (coop_repl.c and the netcode thread's later files): COOPPKT 20..23, COOPEV 48..55 */
#define COOPPKT_REPL 20     /* records */
#define COOPPKT_REPLSUM 21  /* the desync checksum */
#define COOPPKT_REPLREQ 22  /* send me these blocks */
#define COOPPKT_TIME 23     /* round trip pings */
s32  coopNetRttMs(s32 player);          /* round trip to that player in ms, 0 when not known yet */
/* prediction of our own shots' reactions (coop.c): the host's side, from chraction.c */
s32  coopReactionPick(ChrRecord *self, s32 death, s32 rolled, s32 count);
s32  coopReactionForced(ChrRecord *self);
s32  coopPredictStats(s32 *confirmed, s32 *wrong);  /* joiner: deaths predicted, confirmed, not */
f32  coopDifficultyGuardHealthMul(void);    /* the lobby difficulty's guard health, on any game */

/* connection quality (coop.c coopNetLink): a player's line to the host */
enum { COOPLINK_UNKNOWN, COOPLINK_GOOD, COOPLINK_FAIR, COOPLINK_POOR };
#define COOP_LINK_FAIR_RTT    120   /* ms round trip */
#define COOP_LINK_POOR_RTT    250
#define COOP_LINK_FAIR_LOSS   2     /* % */
#define COOP_LINK_POOR_LOSS   8
#define COOP_LINK_FAIR_JITTER 30    /* ms */
#define COOP_LINK_POOR_JITTER 80
typedef struct
{
    s32 rtt;        /* ms */
    s32 loss;       /* %, one way */
    s32 jitter;     /* ms */
    s32 quality;    /* COOPLINK_* */
} CoopLink;
s32  coopNetLink(s32 p, CoopLink *out);     /* 0 when not measured (yet) */

/* smooth movement (coop_interp.c): timed samples drawn a moment behind */
#define COOPINTERP_SAMPLES 32    /* half a second of a 60 fps sender: more than the longest drawing delay (coop/ci/interp_test.c) */
typedef struct
{
    u32 ms;         /* the sender's clock */
    f32 pos[3];
    f32 yaw;
} CoopSample;
typedef struct
{
    CoopSample s[COOPINTERP_SAMPLES];   /* oldest first */
    s32 n;
} CoopTrack;
typedef struct
{
    s32 valid;
    s32 off;        /* our clock minus the sender's, for its quickest packet */
    f32 creep;
    u32 lastSend;
    f32 interval;   /* ms between its samples, smoothed */
    f32 jitter;     /* ms its packets come later than the quickest, smoothed */
} CoopClock;
void coopClockReset(CoopClock *c);
void coopClockSample(CoopClock *c, u32 sendms, u32 arrivems);
s32  coopClockDelay(const CoopClock *c);
u32  coopClockRenderMs(const CoopClock *c, u32 nowms);
void coopTrackReset(CoopTrack *t);
void coopTrackPush(CoopTrack *t, const CoopSample *s);
s32  coopTrackAt(const CoopTrack *t, u32 rt, CoopSample *out);
void coopReplStageStart(void);
void coopReplTick(u32 missionticks);
void coopReplPacket(const void *buf, s32 len, s32 from);
void coopReplSendSnapshot(s32 player);  /* host: every setup object's state to that player, over the next ticks */
s32  coopReplSnapshotDone(void);        /* joiner: this mission's join snapshot is in and applied */
void coopReplMigrated(void);            /* we became the host or follow a new one: snapshot to everyone again */
s32  coopReplDesyncs(void);             /* joiner: blocks found different and corrected this mission */
s32  coopReplUnresolved(void);          /* joiner: differences that outlasted 10 s of corrections */
/* coop.c's side of it */
s32  coopReplPlayerHere(s32 p);         /* host: player p's Bond is in this mission */
s32  coopReplHostHere(void);            /* joiner: we and the host are in this mission */
void coopReplDoorSet(DoorRecord *door, s32 state);  /* doorSetOpenState, sent to no one */
void coopReplDestroy(ObjectRecord *obj);            /* AI_DestroyObject's damage, sent to no one */
void coopReplTeamPick(ObjectRecord *obj);           /* a shared pickup we missed: take it */

#endif
