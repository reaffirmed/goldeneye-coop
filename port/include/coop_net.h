#ifndef COOP_NET_H
#define COOP_NET_H

/* Co-op transport: unreliable datagrams, over Steam (lobbies and Valve's
 * relay) or plain UDP for local tests. See coop_net.c for GE_COOP.
 *
 * Up to four players in a star: the host has a peer slot for each joiner,
 * a joiner has the host in slot 0. Slots keep their number while the peer
 * stays connected. */

#define COOP_NET_MAX_PEERS 3

int  coopNetInit(void);                 /* 1 if co-op is configured and open */
int  coopNetStart(const char *spec);    /* open with a GE_COOP-style spec, closing any session first */
int  coopNetIsOpen(void);
int  coopNetIsSteam(void);
int  coopNetIsHost(void);
int  coopNetHasPeer(void);              /* host: a client has sent something */
int  coopNetPeerConnected(int peer);
void coopNetDropPeer(int peer);         /* the game gave up on it: free the slot */
const char *coopNetPeerName(int peer);  /* Steam name, or ip:port */
unsigned long long coopNetPeerId(int peer); /* SteamID, or address and port; 0 when free */
void coopNetSend(const void *buf, int len);             /* to every peer */
void coopNetSendTo(int peer, const void *buf, int len);
int  coopNetRecv(void *buf, int cap);   /* bytes, or 0 when nothing is queued */
int  coopNetRecvFrom(void *buf, int cap, int *peer);    /* ...and which slot sent it */
void coopNetShutdown(void);

/* Steam backend (coop_steam.c), used by coop_net.c when GE_COOP starts with "steam" */
int  coopSteamInit(const char *spec);
int  coopSteamIsHost(void);
int  coopSteamHasPeer(void);
int  coopSteamPeerConnected(int peer);
void coopSteamDropPeer(int peer);
void coopSteamSend(const void *buf, int len);
void coopSteamSendTo(int peer, const void *buf, int len);
int  coopSteamRecv(void *buf, int cap);
int  coopSteamRecvFrom(void *buf, int cap, int *peer);
void coopSteamShutdown(void);

/* the lobby screen (front.c through src/game/coop.c) */
int  coopSteamLobbyState(void);         /* 0 none, 1 creating or looking, 2 in a lobby */
const char *coopSteamSelfName(void);
const char *coopSteamPeerName(void);
const char *coopSteamPeerNameAt(int peer);
unsigned long long coopSteamPeerIdAt(int peer);
unsigned long long coopSteamPeerId(void);  /* 0 until known */
int  coopSteamFriendsRefresh(void);     /* online friends, hosting ones first */
const char *coopSteamFriendName(int i);
int  coopSteamFriendHosting(int i);
int  coopSteamFriendIsPeer(int i);
void coopSteamFriendInvite(int i);
unsigned long long coopSteamFriendLobby(int i);
void coopSteamInviteOverlay(void);

#endif
