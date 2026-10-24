#ifndef COOP_NET_H
#define COOP_NET_H

/* Co-op transport: one peer, unreliable datagrams, over Steam (lobbies and
 * Valve's relay) or plain UDP for local tests. See coop_net.c for GE_COOP. */

int  coopNetInit(void);                 /* 1 if co-op is configured and open */
int  coopNetStart(const char *spec);    /* open with a GE_COOP-style spec, closing any session first */
int  coopNetIsOpen(void);
int  coopNetIsSteam(void);
int  coopNetIsHost(void);
int  coopNetHasPeer(void);              /* host: a client has sent something */
void coopNetSend(const void *buf, int len);
int  coopNetRecv(void *buf, int cap);   /* bytes, or 0 when nothing is queued */
void coopNetShutdown(void);

/* Steam backend (coop_steam.c), used by coop_net.c when GE_COOP starts with "steam" */
int  coopSteamInit(const char *spec);
int  coopSteamIsHost(void);
int  coopSteamHasPeer(void);
void coopSteamSend(const void *buf, int len);
int  coopSteamRecv(void *buf, int cap);
void coopSteamShutdown(void);

/* the lobby screen (front.c through src/game/coop.c) */
int  coopSteamLobbyState(void);         /* 0 none, 1 creating or looking, 2 in a lobby */
const char *coopSteamSelfName(void);
const char *coopSteamPeerName(void);
unsigned long long coopSteamPeerId(void);  /* 0 until known */
int  coopSteamFriendsRefresh(void);     /* online friends, hosting ones first */
const char *coopSteamFriendName(int i);
int  coopSteamFriendHosting(int i);
int  coopSteamFriendIsPeer(int i);
void coopSteamFriendInvite(int i);
unsigned long long coopSteamFriendLobby(int i);
void coopSteamInviteOverlay(void);

#endif
