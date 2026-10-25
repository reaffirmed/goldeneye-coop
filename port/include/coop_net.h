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

/* Host migration: the host is gone and this game takes its place or follows
 * whoever does. ashost: become the host, with ids[0..n-1] (coopNetPeerId
 * values from the old host's roster) reserved in peer slots 0..n-1; else
 * ids[0] is the new host. The socket or the Steam lobby stays open. */
void coopNetMigrate(int ashost, const unsigned long long *ids, int n);
int  coopNetHostLeft(void);             /* joiner: the transport saw the host leave (Steam: it left the lobby) */

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
void coopSteamMigrate(int ashost, const unsigned long long *ids, int n);
int  coopSteamHostLeft(void);

/* the lobby screen (front.c through src/game/coop.c) */
int  coopSteamLobbyState(void);         /* 0 none, 1 creating or looking, 2 in a lobby */
const char *coopSteamSelfName(void);
unsigned long long coopSteamSelfId(void);
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
void coopSteamSetLobbyOptions(int pub, const char *password, int version);  /* host */
void coopSteamBrowseRefresh(void);      /* Public Games: ask Steam again */
int  coopSteamBrowseCount(void);        /* -1 still looking */
const char *coopSteamBrowseName(int i);
int  coopSteamBrowseMembers(int i);
int  coopSteamBrowseLocked(int i);      /* the host set a password */
unsigned long long coopSteamBrowseLobby(int i);
void coopSteamSetJoinPassword(const char *password);
int  coopSteamAdmitWaiting(void);       /* joiner: the host is checking our password */
int  coopSteamJoinRefused(void);        /* once, after a wrong password */

void coopSteamSetJoinPasswordHash(unsigned int hash);  /* a Discord join secret carries the hash, not the password */
int  coopSteamPasswordHash(unsigned int *hash);    /* host: 1 and the lobby's hash when it has a password */
unsigned long long coopSteamLobbyId(void);
int  coopSteamLobbyHasPassword(void);  /* the lobby we are in, 0 none */
int  coopSteamPresenceOpen(void);       /* Steam up for presence and invites, no lobby (once; 1 when up) */
void coopSteamPresenceTick(void);       /* callbacks, when no session is pumping them */
void coopSteamSetPresence(const char *status, int phase, unsigned long long group, int groupsize, int joinable);  /* phase: COOP_PRESENCE_* */

/* Steam Voice (coop_steam.c), for src/game/coop_voice.c */
int  coopSteamVoiceAvailable(void);     /* Steam is up and has the voice calls */
void coopSteamVoiceRecord(int on);      /* the microphone on or off */
int  coopSteamVoiceRead(void *buf, int cap);    /* compressed voice since the last call: bytes, 0 none */
int  coopSteamVoiceDecode(const void *in, int len, short *out, int cap, int rate);  /* mono samples, 0 on a fault */

/* the voice mixer (port/src/voice_pc.c): one stream per player, mixed into
 * the game's output by audio_pc.c */
#define PORT_VOICE_RATE 22050           /* audio_pc.c's output rate */
void portVoicePush(int player, const short *pcm, int n);
void portVoiceSetGain(int player, float left, float right);
void portVoiceDrop(int player);         /* stop and forget what is queued */
void portVoiceMix(short *stereo, int frames);   /* audio_pc.c: add the voices to one output frame */
int  portVoiceLevel(int player);        /* 0..100, how loud they are right now (what is playing) */

/* Rich presence and joins from outside the game (port/src/coop_presence.c):
 * the game says what it is doing, and takes the joins Steam and Discord hand
 * it (an accepted invite, Join Game, Ask to Join). */
enum { COOP_PRESENCE_MENU, COOP_PRESENCE_LOBBY, COOP_PRESENCE_MISSION };
typedef struct
{
    int phase;                  /* COOP_PRESENCE_* */
    const char *status;         /* one line: Steam's status, "Co-op: Dam, 00 Agent (2/4)" */
    const char *details;        /* Discord's first line, "Co-op: Dam (00 Agent)" */
    const char *state;          /* Discord's second, "In a mission" */
    unsigned long long lobby;   /* the Steam lobby, 0 for none */
    int players, maxplayers;
    int joinable;               /* a friend may join now */
} CoopPresence;
void coopPresenceTick(const CoopPresence *p);   /* every frame or so; pushes changes */
void coopPresenceQueueJoin(unsigned long long lobby, int haspw, unsigned int pwhash, const char *from);
/* 1 once per join waiting: the lobby, the password hash (haspw) and who sent it */
int  coopPresenceTakeJoin(unsigned long long *lobby, int *haspw, unsigned int *pwhash, char *from, int cap);
int  coopPresencePeekJoin(char *from, int cap);  /* a join is waiting (it stays) */

/* Discord (port/src/coop_discord.c), used by coop_presence.c and the lobby screen */
void coopDiscordTick(void);
int  coopDiscordConnected(void);
void coopDiscordSetActivity(const char *details, const char *state, long long start,
                            const char *party, int size, int max, const char *secret);
const char *coopDiscordTakeJoin(void);  /* a Join clicked in Discord: its secret, once ("" none) */
int  coopDiscordAskCount(void);         /* host: Ask to Join requests waiting */
const char *coopDiscordAskName(int i);
void coopDiscordRespond(int i, int accept);

#endif
