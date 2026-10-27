/**
 * Co-op over Steam, as AppID 480 "Spacewar" (playbook: coop-anything
 * networking-steam-spacewar.md).
 *
 * steam_api.dll (libsteam_api.so on Linux) is loaded at run time and only
 * its flat C exports are used, so no Steamworks headers or import libs are
 * needed. Struct fields are read at fixed byte offsets, because the DLL is
 * built with 8-byte packing and gcc on i686 aligns 64-bit fields to 4.
 *
 * Host:  creates a friends-only lobby (or a public one, listed in the lobby
 *        screen's Public Games), advertises it in rich presence and opens
 *        the invite overlay. Friends join with Join Game, an invite, or by
 *        starting with GE_COOP=steam:join.
 * Password: a host can set one. Its Steam friends get in without it; anyone
 *        else's game sends it (hashed with the lobby id) as a GEPW message
 *        when it enters the lobby, and the host takes it as a player only
 *        once that matches (GEPA back), or turns it away (GEPR).
 * Join:  GE_COOP=steam:join:<lobbyid>, "+connect_lobby <id>" from Steam, an
 *        accepted invite, or, with no id, the first friend found hosting.
 *        Invites and Join Game (callbacks 333 / 337) go to coop_presence.c,
 *        which the game asks for them from any menu, so they work outside
 *        the lobby screen and while hosting too.
 * Rich presence: coopSteamSetPresence (coop_presence.c says what to show).
 *        Steam is brought up at boot for it (coopSteamPresenceOpen).
 *
 * Up to four players: the host talks to each joiner, a joiner only to the
 * host (the game relays what the joiners need of each other).
 *
 * Game packets go to the other players' SteamIDs through
 * ISteamNetworkingMessages (unreliable, no Nagle, channel 0); the game does
 * its own acks for what must arrive (src/game/coop.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define STEAM_LIB "steam_api.dll"
#define LIB_OPEN(n) ((void *) LoadLibraryA(n))
#define LIB_SYM(h, n) ((void *) GetProcAddress((HMODULE) (h), n))
#else
#include <dlfcn.h>
#define STEAM_LIB "libsteam_api.so"
#define LIB_OPEN(n) dlopen(n, RTLD_NOW)
#define LIB_SYM(h, n) dlsym(h, n)
#endif

#include "coop_net.h"
#include "port_buildinfo.h"
#include "port_update.h"

extern void portConLogf(const char *fmt, ...);
extern const char *portRootPath(const char *name);

typedef unsigned long long u64s;
typedef unsigned char sbool; /* C++ bool in the DLL: only the low byte is set */

#define APPID_SPACEWAR 480
#define LOBBY_KEY "gecoop"
#define LOBBY_KEY_VERSION "gever"   /* the game's co-op protocol: the list shows only ours */
#define LOBBY_KEY_BUILD "gebuild" /* the host's build (commit), for the "update needed" line */
#define LOBBY_KEY_RELEASE "gerel" /* the host's release (v0.2.2), so a joiner knows who has to update */
#define LOBBY_KEY_NAME "host"
#define LOBBY_KEY_MISSION "gemission"   /* "1" while the host is in a mission: Public Games says Drop In */
#define LOBBY_KEY_PASSWORD "pw"

/* the password handshake: transport-level messages, never handed to the game */
#define PW_ASK    0x57504547u   /* "GEPW" + the hash */
#define PW_OK     0x41504547u   /* "GEPA" */
#define PW_NO     0x52504547u   /* "GEPR" */
#define LOBBY_HOST_KEY "gehost"   /* the host's SteamID, once the host is not the lobby's creator */

/* callback ids */
enum
{
    CB_GameLobbyJoinRequested = 333,
    CB_GameRichPresenceJoinRequested = 337,
    CB_LobbyEnter = 504,
    CB_LobbyMatchList = 510,
    CB_LobbyChatUpdate = 506,
    CB_LobbyCreated = 513,
    CB_SteamAPICallCompleted = 703,
    CB_NetMsgSessionRequest = 1251,
    CB_NetMsgSessionFailed = 1252,
};

#define SEND_UNRELIABLE_NONAGLE 1
#define IDENTITY_SIZE 160 /* SteamNetworkingIdentity is 136; KeeperFX's margin */

/* flat API */
static int (*pInitFlat)(char *err);
static sbool (*pIsSteamRunning)(void);
static void (*pShutdown)(void);
static int (*pGetHSteamPipe)(void);
static void (*pMDInit)(void);
static void (*pMDRunFrame)(int pipe);
static sbool (*pMDGetNextCallback)(int pipe, void *msg);
static void (*pMDFreeLastCallback)(int pipe);
static sbool (*pMDGetAPICallResult)(int pipe, u64s call, void *buf, int size, int cbexpected, sbool *failed);
static void *(*pSteamUser)(void);
static void *(*pSteamMatchmaking)(void);
static void *(*pSteamFriends)(void);
static void *(*pSteamNetMsgs)(void);
static u64s (*pUserGetSteamID)(void *self);
static u64s (*pCreateLobby)(void *self, int type, int maxmembers);
static u64s (*pJoinLobby)(void *self, u64s lobby);
static void (*pLeaveLobby)(void *self, u64s lobby);
static sbool (*pSetLobbyData)(void *self, u64s lobby, const char *key, const char *value);
static const char *(*pGetLobbyData)(void *self, u64s lobby, const char *key);
static u64s (*pGetLobbyOwner)(void *self, u64s lobby);
static int (*pGetNumLobbyMembers)(void *self, u64s lobby);
static u64s (*pGetLobbyMemberByIndex)(void *self, u64s lobby, int i);
static sbool (*pSetRichPresence)(void *self, const char *key, const char *value);
static void (*pClearRichPresence)(void *self);
static void (*pInviteDialog)(void *self, u64s lobby);
static int (*pGetFriendCount)(void *self, int flags);
static u64s (*pGetFriendByIndex)(void *self, int i, int flags);
static sbool (*pGetFriendGamePlayed)(void *self, u64s user, void *info);
static const char *(*pGetFriendPersonaName)(void *self, u64s user);
static int (*pGetFriendPersonaState)(void *self, u64s user);
static sbool (*pInviteUserToLobby)(void *self, u64s lobby, u64s user);
static void (*pIdentSetSteamID64)(void *ident, u64s id);
static u64s (*pIdentGetSteamID64)(void *ident);
static int (*pSendMessageToUser)(void *self, const void *ident, const void *data, unsigned int len, int flags, int channel);
static int (*pReceiveMessagesOnChannel)(void *self, int channel, void **out, int max);
static sbool (*pAcceptSessionWithUser)(void *self, const void *ident);
static sbool (*pCloseSessionWithUser)(void *self, const void *ident);
static void (*pMessageRelease)(void *msg);
static u64s (*pRequestLobbyList)(void *self);
static void (*pAddLobbyStringFilter)(void *self, const char *key, const char *value, int cmp);
static void (*pAddLobbyDistanceFilter)(void *self, int filter);
static u64s (*pGetLobbyByIndex)(void *self, int i);
static sbool (*pSetLobbyType)(void *self, u64s lobby, int type);
static int (*pGetFriendRelationship)(void *self, u64s user);
static sbool (*pSetLobbyOwner)(void *self, u64s lobby, u64s owner);   /* optional */
/* Steam Voice (ISteamUser), optional: voice chat is off without them */
static void (*pStartVoice)(void *self);
static void (*pStopVoice)(void *self);
static int (*pGetAvailableVoice)(void *self, unsigned int *compressed, unsigned int *uncompressedDeprecated, unsigned int rateDeprecated);
static int (*pGetVoice)(void *self, sbool wantCompressed, void *dest, unsigned int cap, unsigned int *written,
                        sbool wantUncompressedDeprecated, void *udest, unsigned int ucap, unsigned int *uwritten, unsigned int urate);
static int (*pDecompressVoice)(void *self, const void *in, unsigned int len, void *dest, unsigned int cap, unsigned int *written, unsigned int rate);

static void *sLib;
static void *sUser, *sMM, *sFriends, *sNM;
static int sPipe;
static int sIsHost;
static u64s sSelf;
static u64s sLobby;
/* the other players: a joiner has only the host, in slot 0; a host fills
 * the slots as joiners turn up. 0 = a free slot. */
static u64s sPeers[COOP_NET_MAX_PEERS];
static unsigned char sPeerIdent[COOP_NET_MAX_PEERS][IDENTITY_SIZE];
static u64s sCreateCall, sJoinCall;
static int sJoinWanted;     /* joiner: still looking for a lobby */
static unsigned int sPolls;
static int sSenderOfs;      /* 12 or 16, see coopSteamRecv */
static int sReady;          /* the library is loaded and SteamAPI is up */
static int sOverlay;        /* host: open Steam's invite overlay with the lobby */
static int sHostLeft;       /* joiner: the host left the lobby (the game picks a new one) */
static int sHostTagged;     /* host after a migration: the lobby says who hosts */
static int sAppId = APPID_SPACEWAR; /* steam_appid.txt's: friends playing it are candidate hosts */
static unsigned int sJoinHash;  /* a password hash from a Discord join secret */
static int sJoinHashSet;
static char sPresence[400];     /* what coopSteamSetPresence last sent */
static unsigned int sPumped;    /* pump() calls: coopSteamPresenceTick pumps when a session doesn't */

/* the lobby screen's friend list, see coopSteamFriendsRefresh */
#define MAX_FRIENDS 48
typedef struct
{
    u64s id;
    u64s lobby;     /* playing Spacewar with a lobby open, else 0 */
    char name[32];
} SteamFriend;
static SteamFriend sFriendList[MAX_FRIENDS];
static int sNumFriends;

/* the host's lobby options (coopSteamSetLobbyOptions) */
static int sPublic;
static char sPassword[32];
static char sVersion[8] = "0";
/* a joiner's password for the lobby it is joining, and how that went */
static char sJoinPassword[32];
static int sAdmitWait;      /* in a passworded lobby: not taken as a player yet */
static int sJoinRefused;    /* 1: the host turned our password down; 2: Steam kept us out of the lobby;
                             * 3: the host's game is another co-op version */
static char sRefusedBuild[16]; /* 3: the host's build, "" if it did not say */
static char sRefusedRelease[24]; /* 3: the host's release, "" from a build older than the updater */
static int sJoinDirect;     /* joining one lobby by its id (not looking for any friend's) */
static u64s sJoinTarget;    /* the lobby our last JoinLobby asked for: any other entry is stale */

/* the lobby screen's Public Games, see coopSteamBrowseRefresh */
#define MAX_PUBLIC 16
typedef struct
{
    u64s lobby;
    char name[32];
    int members;
    int locked;
    int inmission;
} SteamPublic;
static SteamPublic sPublicList[MAX_PUBLIC];
static int sNumPublic;
static u64s sListCall;      /* a request is out */
static int sListDone;       /* a list has come back at least once */

static u64s rd64(const unsigned char *p)
{
    u64s v;
    memcpy(&v, p, 8);
    return v;
}

static unsigned int rd32(const unsigned char *p)
{
    unsigned int v;
    memcpy(&v, p, 4);
    return v;
}

static int loadSym(void **fn, const char *name)
{
    *fn = LIB_SYM(sLib, name);
    if (*fn == NULL)
    {
        portConLogf("ge: steam: %s is missing from %s (too old a steam_api?)\n", name, STEAM_LIB);
        return 0;
    }
    return 1;
}

static int loadAll(void)
{
    int ok = 1;

#define L(fn, name) ok &= loadSym((void **) &fn, name)
    L(pInitFlat, "SteamAPI_InitFlat");
    L(pIsSteamRunning, "SteamAPI_IsSteamRunning");
    L(pShutdown, "SteamAPI_Shutdown");
    L(pGetHSteamPipe, "SteamAPI_GetHSteamPipe");
    L(pMDInit, "SteamAPI_ManualDispatch_Init");
    L(pMDRunFrame, "SteamAPI_ManualDispatch_RunFrame");
    L(pMDGetNextCallback, "SteamAPI_ManualDispatch_GetNextCallback");
    L(pMDFreeLastCallback, "SteamAPI_ManualDispatch_FreeLastCallback");
    L(pMDGetAPICallResult, "SteamAPI_ManualDispatch_GetAPICallResult");
    L(pSteamUser, "SteamAPI_SteamUser_v023");
    L(pSteamMatchmaking, "SteamAPI_SteamMatchmaking_v009");
    L(pSteamNetMsgs, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
    L(pUserGetSteamID, "SteamAPI_ISteamUser_GetSteamID");
    L(pCreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby");
    L(pJoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby");
    L(pLeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    L(pSetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    L(pGetLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    L(pGetLobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    L(pGetNumLobbyMembers, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    L(pGetLobbyMemberByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    L(pSetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence");
    L(pClearRichPresence, "SteamAPI_ISteamFriends_ClearRichPresence");
    L(pInviteDialog, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    L(pGetFriendCount, "SteamAPI_ISteamFriends_GetFriendCount");
    L(pGetFriendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex");
    L(pGetFriendGamePlayed, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    L(pGetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    L(pGetFriendPersonaState, "SteamAPI_ISteamFriends_GetFriendPersonaState");
    L(pInviteUserToLobby, "SteamAPI_ISteamMatchmaking_InviteUserToLobby");
    L(pIdentSetSteamID64, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
    L(pIdentGetSteamID64, "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
    L(pSendMessageToUser, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    L(pReceiveMessagesOnChannel, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    L(pAcceptSessionWithUser, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    L(pCloseSessionWithUser, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
    L(pMessageRelease, "SteamAPI_SteamNetworkingMessage_t_Release");
    L(pRequestLobbyList, "SteamAPI_ISteamMatchmaking_RequestLobbyList");
    L(pAddLobbyStringFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
    L(pAddLobbyDistanceFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter");
    L(pGetLobbyByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex");
    L(pSetLobbyType, "SteamAPI_ISteamMatchmaking_SetLobbyType");
    L(pGetFriendRelationship, "SteamAPI_ISteamFriends_GetFriendRelationship");
#undef L
    pSetLobbyOwner = (sbool (*)(void *, u64s, u64s)) LIB_SYM(sLib, "SteamAPI_ISteamMatchmaking_SetLobbyOwner");
    *(void **) &pStartVoice = LIB_SYM(sLib, "SteamAPI_ISteamUser_StartVoiceRecording");
    *(void **) &pStopVoice = LIB_SYM(sLib, "SteamAPI_ISteamUser_StopVoiceRecording");
    *(void **) &pGetAvailableVoice = LIB_SYM(sLib, "SteamAPI_ISteamUser_GetAvailableVoice");
    *(void **) &pGetVoice = LIB_SYM(sLib, "SteamAPI_ISteamUser_GetVoice");
    *(void **) &pDecompressVoice = LIB_SYM(sLib, "SteamAPI_ISteamUser_DecompressVoice");
    if (pStartVoice == NULL || pStopVoice == NULL || pGetAvailableVoice == NULL || pGetVoice == NULL || pDecompressVoice == NULL)
    {
        portConLogf("ge: steam: no Steam Voice exports in %s; voice chat is off\n", STEAM_LIB);
        pStartVoice = NULL;
    }
    pSteamFriends = (void *(*)(void)) LIB_SYM(sLib, "SteamAPI_SteamFriends_v018");
    if (pSteamFriends == NULL)
    {
        ok &= loadSym((void **) &pSteamFriends, "SteamAPI_SteamFriends_v017");
    }
    return ok;
}

/* Steam reads steam_appid.txt from the working folder; put one beside the
 * exe and in the working folder so a missing file can't break the launch. */
static void writeAppId(void)
{
    const char *paths[2];
    int i;

    paths[0] = portRootPath("steam_appid.txt");
    paths[1] = "steam_appid.txt";
    for (i = 0; i < 2; i++)
    {
        FILE *f = fopen(paths[i], "r");

        if (f != NULL)
        {
            int id = 0;

            if (i == 0 && fscanf(f, "%d", &id) == 1 && id > 0)
            {
                sAppId = id;    /* a game's own AppID, once it has one; Spacewar until then */
            }
            fclose(f);
            continue;
        }
        f = fopen(paths[i], "w");
        if (f != NULL)
        {
            fprintf(f, "%d\n", APPID_SPACEWAR);
            fclose(f);
        }
    }
}

static int peerSlot(u64s id)
{
    int i;

    for (i = 0; i < COOP_NET_MAX_PEERS; i++)
    {
        if (id != 0 && sPeers[i] == id)
        {
            return i;
        }
    }
    return -1;
}

/* the slot that now holds id (slot < 0: its own or the first free one), -1 when all are taken */
static int setPeer(int slot, u64s id)
{
    int i;

    if (slot < 0)
    {
        slot = peerSlot(id);
    }
    for (i = 0; slot < 0 && i < COOP_NET_MAX_PEERS; i++)
    {
        if (sPeers[i] == 0)
        {
            slot = i;
        }
    }
    if (slot < 0)
    {
        return -1;
    }
    sPeers[slot] = id;
    memset(sPeerIdent[slot], 0, IDENTITY_SIZE);
    pIdentSetSteamID64(sPeerIdent[slot], id);
    return slot;
}

static void dropPeer(int slot)
{
    if (slot >= 0 && slot < COOP_NET_MAX_PEERS && sPeers[slot] != 0)
    {
        pCloseSessionWithUser(sNM, sPeerIdent[slot]);
        sPeers[slot] = 0;
    }
}

static void dropAllPeers(void)
{
    int i;

    for (i = 0; i < COOP_NET_MAX_PEERS; i++)
    {
        dropPeer(i);
    }
}

static int isLobbyMember(u64s id)
{
    int n, i;

    if (sLobby == 0)
    {
        return 0;
    }
    n = pGetNumLobbyMembers(sMM, sLobby);
    for (i = 0; i < n; i++)
    {
        if (pGetLobbyMemberByIndex(sMM, sLobby, i) == id)
        {
            return 1;
        }
    }
    return 0;
}

/* FNV-1a over the password and the lobby id: the password itself never
 * goes over the wire, and a hash for one lobby is no good for another */
static unsigned int pwHash(const char *pw, u64s lobby)
{
    unsigned int h = 2166136261u;
    int i;

    for (i = 0; pw[i] != '\0'; i++)
    {
        h = (h ^ (unsigned char) pw[i]) * 16777619u;
    }
    for (i = 0; i < 8; i++)
    {
        h = (h ^ (unsigned char) (lobby >> (i * 8))) * 16777619u;
    }
    return h;
}

/* a short control message to anyone, peer or not */
static void sendControl(u64s to, unsigned int kind, unsigned int arg)
{
    unsigned char ident[IDENTITY_SIZE];
    unsigned int m[2];

    m[0] = kind;
    m[1] = arg;
    memset(ident, 0, sizeof(ident));
    pIdentSetSteamID64(ident, to);
    pSendMessageToUser(sNM, ident, m, sizeof(m), SEND_UNRELIABLE_NONAGLE, 0);
}

static int isFriend(u64s id)
{
    return pGetFriendRelationship(sFriends, id) == 3 /* k_EFriendRelationshipFriend */;
}

/* host: the lobby's type and keys, as set now */
static void applyLobbyOptions(void)
{
    char buf[48];

    if (!sIsHost || sLobby == 0)
    {
        return;
    }
    pSetLobbyType(sMM, sLobby, sPublic ? 2 /* public */ : 1 /* friends only */);
    pSetLobbyData(sMM, sLobby, LOBBY_KEY_VERSION, sVersion);
    pSetLobbyData(sMM, sLobby, LOBBY_KEY_BUILD, PORT_BUILD_COMMIT);
    pSetLobbyData(sMM, sLobby, LOBBY_KEY_RELEASE, portUpdateOwn());
    snprintf(buf, sizeof(buf), "%s", pGetFriendPersonaName(sFriends, sSelf));
    pSetLobbyData(sMM, sLobby, LOBBY_KEY_NAME, buf);
    pSetLobbyData(sMM, sLobby, LOBBY_KEY_PASSWORD, sPassword[0] != '\0' ? "1" : "0");
}

/* host: a control message from someone in the lobby (1 when it was one) */
static int hostControl(u64s from, const unsigned char *data, int size)
{
    unsigned int m[2];

    if (size != 8)
    {
        return 0;
    }
    memcpy(m, data, 8);
    if (m[0] != PW_ASK)
    {
        return 0;
    }
    if (peerSlot(from) >= 0)
    {
        sendControl(from, PW_OK, 0);    /* in already; our answer was lost */
    }
    else if (isLobbyMember(from) && (sPassword[0] == '\0' || isFriend(from) || m[1] == pwHash(sPassword, sLobby)))
    {
        if (setPeer(-1, from) >= 0)
        {
            sendControl(from, PW_OK, 0);
            portConLogf("ge: steam: %s joined (password)\n", pGetFriendPersonaName(sFriends, from));
        }
    }
    else
    {
        sendControl(from, PW_NO, 0);
        portConLogf("ge: steam: turned %llu away: wrong password\n", from);
    }
    return 1;
}

/* joiner: the host's answer to our password (1 when it was one) */
static int joinControl(u64s from, const unsigned char *data, int size)
{
    unsigned int m[2];

    if (size != 8 || from != sPeers[0])
    {
        return 0;
    }
    memcpy(m, data, 8);
    if (m[0] == PW_OK)
    {
        if (sAdmitWait)
        {
            portConLogf("ge: steam: the host let us in\n");
        }
        sAdmitWait = 0;
        return 1;
    }
    if (m[0] == PW_NO)
    {
        if (sAdmitWait)
        {
            portConLogf("ge: steam: wrong password for lobby %llu\n", sLobby);
            sJoinRefused = 1;
            dropAllPeers();
            pLeaveLobby(sMM, sLobby);
            sLobby = 0;
            sJoinWanted = 0;
            sJoinCall = 0;
            sAdmitWait = 0;
        }
        return 1;
    }
    return 0;
}

static void onLobbyList(const unsigned char *p)
{
    int n = (int) rd32(p);
    int i;

    sListCall = 0;
    sListDone = 1;
    sNumPublic = 0;
    for (i = 0; i < n && sNumPublic < MAX_PUBLIC; i++)
    {
        u64s lobby = pGetLobbyByIndex(sMM, i);
        const char *v;
        SteamPublic *e;

        /* each answer is read before the next call: the string may not outlive it */
        v = pGetLobbyData(sMM, lobby, LOBBY_KEY);
        if (lobby == 0 || lobby == sLobby || v == NULL || strcmp(v, "1") != 0)
        {
            continue;
        }
        e = &sPublicList[sNumPublic];
        e->lobby = lobby;
        v = pGetLobbyData(sMM, lobby, LOBBY_KEY_PASSWORD);
        e->locked = (v != NULL && strcmp(v, "1") == 0);
        v = pGetLobbyData(sMM, lobby, LOBBY_KEY_NAME);
        snprintf(e->name, sizeof(e->name), "%s", (v != NULL && v[0] != '\0') ? v : "?");
        v = pGetLobbyData(sMM, lobby, LOBBY_KEY_MISSION);
        e->inmission = (v != NULL && strcmp(v, "1") == 0);
        e->members = pGetNumLobbyMembers(sMM, lobby);
        sNumPublic++;
    }
    portConLogf("ge: steam: %d public game(s) listed\n", sNumPublic);
}

/* Who hosts a lobby: its owner, unless a migration moved the game to
 * another member (LOBBY_HOST_KEY, set by the owner once it is the host). */
static u64s lobbyHost(u64s lobby)
{
    const char *h = pGetLobbyData(sMM, lobby, LOBBY_HOST_KEY);
    u64s id = (h != NULL && h[0] != '\0') ? strtoull(h, NULL, 10) : 0;

    if (id != 0 && id != sSelf && isLobbyMember(id))
    {
        return id;
    }
    return pGetLobbyOwner(sMM, lobby);
}

static void joinLobby(u64s lobby)
{
    if (sLobby == lobby || lobby == 0)
    {
        return;
    }
    if (sLobby != 0)
    {
        pLeaveLobby(sMM, sLobby);
        sLobby = 0;
    }
    portConLogf("ge: steam: joining lobby %llu\n", lobby);
    sJoinTarget = lobby;
    sJoinCall = pJoinLobby(sMM, lobby);
    sJoinWanted = 0;
}

/* A friend playing Spacewar with a lobby open is a candidate host. */
static void findHostingFriend(void)
{
    unsigned char info[32];
    int n, i;

    n = pGetFriendCount(sFriends, 4 /* immediate */);
    for (i = 0; i < n; i++)
    {
        u64s f = pGetFriendByIndex(sFriends, i, 4);

        memset(info, 0, sizeof(info));
        if (pGetFriendGamePlayed(sFriends, f, info) && (int) (rd64(info) & 0xFFFFFF) == sAppId && rd64(info + 16) != 0)
        {
            portConLogf("ge: steam: %s is hosting\n", pGetFriendPersonaName(sFriends, f));
            joinLobby(rd64(info + 16));
            return;
        }
    }
}

static void onLobbyCreated(const unsigned char *p)
{
    int result = (int) rd32(p);
    u64s lobby = rd64(p + 8);

    if (result != 1 /* k_EResultOK */ || lobby == 0)
    {
        portConLogf("ge: steam: lobby creation failed (result %d)\n", result);
        return;
    }
    if (lobby == sLobby)
    {
        return; /* real Steam reports it twice: call result and broadcast */
    }
    if (!sIsHost)
    {   /* a Host pressed before a Join: that lobby came too late, and holding
         * it would put us in an empty lobby of our own */
        portConLogf("ge: steam: lobby %llu came after we chose to join, leaving it\n", lobby);
        pLeaveLobby(sMM, lobby);
        return;
    }
    sLobby = lobby;
    pSetLobbyData(sMM, lobby, LOBBY_KEY, "1");
    applyLobbyOptions();
    portConLogf("ge: steam: lobby %llu open\n", lobby);
    if (sOverlay)
    {
        pInviteDialog(sFriends, lobby);
    }
}

static void onLobbyEnter(const unsigned char *p)
{
    u64s lobby = rd64(p);
    unsigned int response = rd32(p + 16);
    const char *tag;

    if (!sIsHost && lobby == sLobby && sPeers[0] != 0)
    {
        return; /* real Steam reports it twice: call result and broadcast. Again
                 * would close the session the host has just opened with us */
    }
    if (!sIsHost && lobby != sJoinTarget)
    {   /* an entry we no longer want (a lobby we created, then chose to join
         * another; an earlier join): never take it for the one we are joining */
        portConLogf("ge: steam: entered lobby %llu, but we are joining %llu: leaving it\n", lobby, sJoinTarget);
        if (response == 1 && lobby != sLobby)
        {
            pLeaveLobby(sMM, lobby);
        }
        return;
    }
    sJoinCall = 0;
    if (sIsHost)
    {
        if (lobby != sLobby)
        {   /* the callback can beat LobbyCreated: take the lobby from here */
            sLobby = lobby;
        }
        return; /* our own lobby */
    }
    if (response != 1)
    {
        portConLogf("ge: steam: could not join lobby %llu (response %u)\n", lobby, response);
        if (sJoinDirect)
        {   /* that lobby or none (an invite, Join Game, Discord): say so, back to idle.
             * A friends-only lobby keeps out anyone who isn't the host's
             * Steam friend, so a Discord friend needs the host's lobby Public. */
            sJoinRefused = 2;
            sJoinDirect = 0;
            return;
        }
        sJoinWanted = 1;
        return;
    }
    tag = pGetLobbyData(sMM, lobby, LOBBY_KEY);
    if (tag == NULL || strcmp(tag, "1") != 0)
    {
        portConLogf("ge: steam: lobby %llu is not a GoldenEye co-op game\n", lobby);
        pLeaveLobby(sMM, lobby);
        sJoinWanted = 1;
        return;
    }
    tag = pGetLobbyData(sMM, lobby, LOBBY_KEY_VERSION);
    if (strcmp(sVersion, "0") != 0 && tag != NULL && strcmp(tag, sVersion) != 0)
    {   /* its packets would never reach us (coop.c drops another version's):
         * say "update needed" instead of waiting on a game that can't start */
        char theirs[8];
        const char *b;

        snprintf(theirs, sizeof(theirs), "%s", tag);   /* the next GetLobbyData can reuse its buffer */
        b = pGetLobbyData(sMM, lobby, LOBBY_KEY_BUILD);
        snprintf(sRefusedBuild, sizeof(sRefusedBuild), "%s", b != NULL ? b : "");
        b = pGetLobbyData(sMM, lobby, LOBBY_KEY_RELEASE);
        snprintf(sRefusedRelease, sizeof(sRefusedRelease), "%s", b != NULL ? b : "");
        portConLogf("ge: steam: lobby %llu is co-op version %s (build %s), ours is %s (build %s): update needed\n",
                lobby, theirs[0] != '\0' ? theirs : "?", sRefusedBuild[0] != '\0' ? sRefusedBuild : "?", sVersion,
                PORT_BUILD_COMMIT);
        pLeaveLobby(sMM, lobby);
        sJoinRefused = 3;
        sJoinDirect = 0;
        sJoinWanted = 0;
        sJoinCall = 0;
        return;
    }
    sLobby = lobby;
    dropAllPeers();
    setPeer(0, lobbyHost(lobby));
    tag = pGetLobbyData(sMM, lobby, LOBBY_KEY_PASSWORD);
    sAdmitWait = (tag != NULL && strcmp(tag, "1") == 0);
    portConLogf("ge: steam: in lobby %llu, host %s\n", lobby, pGetFriendPersonaName(sFriends, sPeers[0]));
}

static void onChatUpdate(const unsigned char *p)
{
    u64s lobby = rd64(p);
    u64s who = rd64(p + 8);
    unsigned int change = rd32(p + 24);

    int slot = peerSlot(who);

    if (lobby != sLobby || slot < 0 || !(change & (0x2 | 0x4 | 0x8 | 0x10)))
    {
        return;
    }
    portConLogf("ge: steam: %s left\n", pGetFriendPersonaName(sFriends, who));
    if (!sIsHost)
    {   /* the host: keep the lobby and the slot, the game picks a new host
         * among those still here (coopNetMigrate) */
        sHostLeft = 1;
        return;
    }
    dropPeer(slot);
}

/* After a migration Steam's owner of the lobby (whoever it handed it to
 * when the old host left) may not be the game's host: an owner that is not
 * hosting passes it on, and the host says in the lobby that it hosts. */
static void fixOwner(void)
{
    u64s owner;
    char buf[64];

    if (sLobby == 0 || (sPolls % 60) != 0)
    {
        return;
    }
    owner = pGetLobbyOwner(sMM, sLobby);
    if (!sIsHost && owner == sSelf && sPeers[0] != 0 && pSetLobbyOwner != NULL && isLobbyMember(sPeers[0]))
    {
        portConLogf("ge: steam: passing the lobby to the host\n");
        pSetLobbyOwner(sMM, sLobby, sPeers[0]);
    }
    if (sIsHost && !sHostTagged && owner == sSelf)
    {
        snprintf(buf, sizeof(buf), "%llu", sSelf);
        pSetLobbyData(sMM, sLobby, LOBBY_HOST_KEY, buf);
        sHostTagged = 1;
    }
}

static void onSessionRequest(const unsigned char *p)
{
    u64s from = pIdentGetSteamID64((void *) p);

    if (peerSlot(from) >= 0)
    {
        pAcceptSessionWithUser(sNM, p);
        return;
    }
    if (sIsHost && isLobbyMember(from) && sPassword[0] != '\0' && !isFriend(from))
    {   /* hear them out: they are a player once their password matches */
        pAcceptSessionWithUser(sNM, p);
        return;
    }
    if (sIsHost && isLobbyMember(from) && setPeer(-1, from) >= 0)
    {
        pAcceptSessionWithUser(sNM, p);
        portConLogf("ge: steam: %s joined\n", pGetFriendPersonaName(sFriends, from));
        return;
    }
    {   /* once per sender: a host keeps knocking until we are in its lobby */
        static u64s lastIgnored;

        if (from != lastIgnored)
        {
            lastIgnored = from;
            portConLogf("ge: steam: ignoring a session from %llu (not in our lobby)\n", from);
        }
    }
}

static void handle(int id, const unsigned char *p)
{
    switch (id)
    {
        case CB_LobbyCreated:
            onLobbyCreated(p);
            break;
        case CB_LobbyEnter:
            onLobbyEnter(p);
            break;
        case CB_LobbyChatUpdate:
            onChatUpdate(p);
            break;
        case CB_LobbyMatchList:
            onLobbyList(p);
            break;
        case CB_GameLobbyJoinRequested:
            /* an accepted invite, or Join Game on a friend in a lobby: lobby, then the friend */
            if (rd64(p) != sLobby)
            {
                coopPresenceQueueJoin(rd64(p), 0, 0, pGetFriendPersonaName(sFriends, rd64(p + 8)));
            }
            break;
        case CB_GameRichPresenceJoinRequested:
        {   /* Join Game through our "connect" rich presence: the friend, then the string */
            const char *c = strstr((const char *) p + 8, "+connect_lobby ");
            if (c != NULL && strtoull(c + 15, NULL, 10) != sLobby)
            {
                coopPresenceQueueJoin(strtoull(c + 15, NULL, 10), 0, 0, pGetFriendPersonaName(sFriends, rd64(p)));
            }
            break;
        }
        case CB_NetMsgSessionRequest:
            onSessionRequest(p);
            break;
        case CB_NetMsgSessionFailed:
            if (peerSlot(pIdentGetSteamID64((void *) p)) >= 0)
            {
                portConLogf("ge: steam: connection to the other player failed\n");
            }
            break;
        default:
            break;
    }
}

static void pump(void)
{
    unsigned char msg[16];

    sPumped++;
    pMDRunFrame(sPipe);
    while (pMDGetNextCallback(sPipe, msg))
    {
        int id = (int) rd32(msg + 4);
        const unsigned char *param = *(unsigned char **) (msg + 8);

        if (id == CB_SteamAPICallCompleted)
        {
            u64s call = rd64(param);
            int expected = (int) rd32(param + 8);
            unsigned int size = rd32(param + 12);
            unsigned char res[512];
            sbool failed = 0;

            if (size <= sizeof(res) && pMDGetAPICallResult(sPipe, call, res, (int) size, expected, &failed) && !failed)
            {
                handle(expected, res);
            }
        }
        else
        {
            handle(id, param);
        }
        pMDFreeLastCallback(sPipe);
    }

    if (sJoinWanted && (sPolls % 120) == 0)
    {
        findHostingFriend();
    }
    if (sAdmitWait && sPeers[0] != 0 && (sPolls % 30) == 0)
    {
        sendControl(sPeers[0], PW_ASK, sJoinHashSet ? sJoinHash : pwHash(sJoinPassword, sLobby));
    }
    fixOwner();
    sPolls++;
}

static int steamOpen(void)
{
    char err[1024];
    int r;

    if (sReady)
    {
        return 1;
    }
    sLib = LIB_OPEN(STEAM_LIB);
    if (sLib == NULL)
    {
        portConLogf("ge: steam: %s not found next to the game; co-op is off\n", STEAM_LIB);
        return 0;
    }
    if (!loadAll())
    {
        return 0;
    }
    writeAppId();
    if (!pIsSteamRunning())
    {
        portConLogf("ge: steam: Steam is not running; start it and log in, then launch again\n");
        return 0;
    }
    memset(err, 0, sizeof(err));
    r = pInitFlat(err);
    if (r != 0)
    {
        portConLogf("ge: steam: init failed (%d): %s\n", r, err);
        return 0;
    }
    pMDInit();
    sPipe = pGetHSteamPipe();
    sUser = pSteamUser();
    sMM = pSteamMatchmaking();
    sFriends = pSteamFriends();
    sNM = pSteamNetMsgs();
    if (sUser == NULL || sMM == NULL || sFriends == NULL || sNM == NULL)
    {
        portConLogf("ge: steam: an interface is missing; co-op is off\n");
        return 0;
    }
    sSelf = pUserGetSteamID(sUser);
    sReady = 1;
    portConLogf("ge: steam: ready as %s (%llu)\n", pGetFriendPersonaName(sFriends, sSelf), sSelf);
    return 1;
}

int coopSteamInit(const char *spec)
{
    if (!steamOpen())
    {
        return 0;
    }
    sIsHost = 0;
    sJoinWanted = 0;
    memset(sPeers, 0, sizeof(sPeers));
    sLobby = 0;
    sOverlay = 0;
    sJoinCall = 0;
    sAdmitWait = 0;
    if (strncmp(spec, "steam:join", 10) != 0)
    {
        sJoinPassword[0] = '\0';
        sJoinHashSet = 0;
    }
    sHostLeft = 0;
    sHostTagged = 1;
    sJoinTarget = 0;

    /* "steam" hosts and opens the invite overlay; "steam:host" hosts (the
     * lobby screen invites in game); "steam:idle" only brings Steam up;
     * "steam:join" finds a friend; "steam:join:<id>" joins that lobby */
    if (strcmp(spec, "steam:idle") == 0)
    {
        return 1;
    }
    if (strncmp(spec, "steam:join", 10) == 0)
    {
        sIsHost = 0;
        sJoinRefused = 0;
        sJoinDirect = spec[10] == ':' && spec[11] != '\0';
        if (sJoinDirect)
        {
            joinLobby(strtoull(spec + 11, NULL, 10));
        }
        else
        {
            sJoinWanted = 1;
            portConLogf("ge: steam: looking for a friend who is hosting\n");
        }
    }
    else
    {
        sIsHost = 1;
        sOverlay = strcmp(spec, "steam:host") != 0;
        sCreateCall = pCreateLobby(sMM, sPublic ? 2 /* public */ : 1 /* friends only */, COOP_NET_MAX_PEERS + 1);
        portConLogf("ge: steam: creating a lobby\n");
    }
    return 1;
}

int coopSteamIsHost(void)
{
    return sIsHost;
}

/* see coopNetMigrate: the lobby stays, the peers change */
void coopSteamMigrate(int ashost, const unsigned long long *ids, int n)
{
    int i;

    if (!sReady)
    {
        return;
    }
    coopSteamVoiceRecord(0);
    dropAllPeers();
    sIsHost = ashost;
    sHostLeft = 0;
    sJoinWanted = 0;
    for (i = 0; i < n && i < COOP_NET_MAX_PEERS && (ashost || i == 0); i++)
    {
        if (ids[i] != 0)
        {
            setPeer(i, ids[i]);
        }
    }
    if (!ashost)
    {
        portConLogf("ge: steam: new host %s\n", pGetFriendPersonaName(sFriends, sPeers[0]));
        return;
    }
    portConLogf("ge: steam: hosting lobby %llu now\n", sLobby);
    sHostTagged = 0;    /* fixOwner tags the lobby once Steam makes us its owner */
    if (sLobby != 0)
    {
        if (pSetLobbyOwner != NULL && pGetLobbyOwner(sMM, sLobby) != sSelf)
        {
            pSetLobbyOwner(sMM, sLobby, sSelf);   /* refused unless we own it; the owner passes it on */
        }
    }
}

int coopSteamHostLeft(void)
{
    return sReady && !sIsHost && sHostLeft;
}

int coopSteamHasPeer(void)
{
    return coopSteamPeerConnected(0) || coopSteamPeerConnected(1) || coopSteamPeerConnected(2);
}

int coopSteamPeerConnected(int peer)
{
    return peer >= 0 && peer < COOP_NET_MAX_PEERS && sPeers[peer] != 0;
}

void coopSteamDropPeer(int peer)
{
    if (sReady)
    {
        dropPeer(peer);
    }
}

void coopSteamSendTo(int peer, const void *buf, int len)
{
    if (!coopSteamPeerConnected(peer))
    {
        return;
    }
    pSendMessageToUser(sNM, sPeerIdent[peer], buf, (unsigned int) len, SEND_UNRELIABLE_NONAGLE, 0);
}

void coopSteamSend(const void *buf, int len)
{
    int i;

    for (i = 0; i < COOP_NET_MAX_PEERS; i++)
    {
        coopSteamSendTo(i, buf, len);
    }
}

int coopSteamRecv(void *buf, int cap)
{
    return coopSteamRecvFrom(buf, cap, NULL);
}

int coopSteamRecvFrom(void *buf, int cap, int *peer)
{
    void *msgs[1];

    if (sLib == NULL)
    {
        return 0;
    }
    for (;;)
    {
        const unsigned char *m;
        const void *data;
        int size;
        u64s from;

        if (pReceiveMessagesOnChannel(sNM, 0, msgs, 1) <= 0)
        {
            pump(); /* queue drained: run callbacks, then report empty */
            return 0;
        }
        m = (const unsigned char *) msgs[0];
        data = *(void *const *) m;
        size = (int) rd32(m + 4);

        /* The sender identity is at 12 in the real 32-bit DLL and at 16 by
         * the headers' packing; take whichever holds a SteamID. */
        if (sSenderOfs == 0)
        {
            from = pIdentGetSteamID64((void *) (m + 12));
            sSenderOfs = (from != 0) ? 12 : 16;
            portConLogf("ge: steam: message sender read at offset %d\n", sSenderOfs);
        }
        from = pIdentGetSteamID64((void *) (m + sSenderOfs));
        if (sIsHost ? hostControl(from, (const unsigned char *) data, size)
                    : joinControl(from, (const unsigned char *) data, size))
        {
            pMessageRelease(msgs[0]);   /* the password handshake: ours, not the game's */
            continue;
        }
        if (!sIsHost && sAdmitWait)
        {
            pMessageRelease(msgs[0]);   /* not in yet: the host is not talking to us */
            continue;
        }
        if (peerSlot(from) >= 0 && size > 0 && size <= cap)
        {
            memcpy(buf, data, (size_t) size);
            pMessageRelease(msgs[0]);
            if (peer != NULL)
            {
                *peer = peerSlot(from);
            }
            return size;
        }
        pMessageRelease(msgs[0]); /* someone else, or too big: drop */
    }
}

/* Leaves the lobby and closes the session. SteamAPI itself stays up for the
 * rest of the run: the lobby screen hosts and joins again without a restart,
 * and Steam tidies up when the process exits. */
void coopSteamShutdown(void)
{
    if (!sReady)
    {
        return;
    }
    dropAllPeers();
    if (sLobby != 0)
    {
        pLeaveLobby(sMM, sLobby);
    }
    pClearRichPresence(sFriends);
    sPresence[0] = '\0';   /* coop_presence.c sets it again */
    sLobby = 0;
    sJoinCall = 0;
    sIsHost = 0;
    sJoinWanted = 0;
    sAdmitWait = 0;
}

/* ---- the lobby screen ------------------------------------------------------- */

/* 0: no lobby, 1: creating one or looking for one, 2: in a lobby */
int coopSteamLobbyState(void)
{
    if (!sReady)
    {
        return 0;
    }
    if (sLobby != 0)
    {
        return 2;
    }
    return (sIsHost || sJoinWanted || sJoinCall != 0) ? 1 : 0;
}

const char *coopSteamSelfName(void)
{
    return sReady ? pGetFriendPersonaName(sFriends, sSelf) : "";
}

unsigned long long coopSteamSelfId(void)
{
    return sReady ? (unsigned long long) sSelf : 0;
}

const char *coopSteamPeerName(void)
{
    return coopSteamPeerNameAt(0);
}

const char *coopSteamPeerNameAt(int peer)
{
    return (sReady && coopSteamPeerConnected(peer)) ? pGetFriendPersonaName(sFriends, sPeers[peer]) : "";
}

unsigned long long coopSteamPeerIdAt(int peer)
{
    return (sReady && coopSteamPeerConnected(peer)) ? (unsigned long long) sPeers[peer] : 0;
}

/* the partner's SteamID, 0 until known (keys the co-op campaign record) */
unsigned long long coopSteamPeerId(void)
{
    return coopSteamPeerIdAt(0);
}

/* Online friends, those hosting first. Returns how many. */
int coopSteamFriendsRefresh(void)
{
    unsigned char info[32];
    int n, i, k;

    sNumFriends = 0;
    if (!sReady)
    {
        return 0;
    }
    n = pGetFriendCount(sFriends, 4 /* immediate */);
    for (i = 0; i < n && sNumFriends < MAX_FRIENDS; i++)
    {
        u64s f = pGetFriendByIndex(sFriends, i, 4);
        SteamFriend *e;

        if (pGetFriendPersonaState(sFriends, f) == 0 /* offline */)
        {
            continue;
        }
        e = &sFriendList[sNumFriends++];
        e->id = f;
        e->lobby = 0;
        memset(info, 0, sizeof(info));
        if (pGetFriendGamePlayed(sFriends, f, info) && (int) (rd64(info) & 0xFFFFFF) == sAppId)
        {
            e->lobby = rd64(info + 16);
        }
        snprintf(e->name, sizeof(e->name), "%s", pGetFriendPersonaName(sFriends, f));
    }
    /* hosting friends to the top, keeping Steam's order otherwise */
    for (i = 1; i < sNumFriends; i++)
    {
        SteamFriend t = sFriendList[i];

        for (k = i; k > 0 && t.lobby != 0 && sFriendList[k - 1].lobby == 0; k--)
        {
            sFriendList[k] = sFriendList[k - 1];
        }
        sFriendList[k] = t;
    }
    return sNumFriends;
}

const char *coopSteamFriendName(int i)
{
    return (i >= 0 && i < sNumFriends) ? sFriendList[i].name : "";
}

int coopSteamFriendHosting(int i)
{
    return i >= 0 && i < sNumFriends && sFriendList[i].lobby != 0;
}

int coopSteamFriendIsPeer(int i)
{
    return i >= 0 && i < sNumFriends && peerSlot(sFriendList[i].id) >= 0;
}

void coopSteamFriendInvite(int i)
{
    if (sIsHost && sLobby != 0 && i >= 0 && i < sNumFriends)
    {
        pInviteUserToLobby(sMM, sLobby, sFriendList[i].id);
        portConLogf("ge: steam: invited %s\n", sFriendList[i].name);
    }
}

/* the lobby a friend is hosting, 0 if none */
unsigned long long coopSteamFriendLobby(int i)
{
    return (i >= 0 && i < sNumFriends) ? sFriendList[i].lobby : 0;
}

void coopSteamInviteOverlay(void)
{
    if (sIsHost && sLobby != 0)
    {
        pInviteDialog(sFriends, sLobby);
    }
}

/* ---- public games and the password ------------------------------------------ */

/* host: list the lobby publicly or for friends only, and its password ("" for
 * none); the game's protocol version keys the list. Live while hosting. */
void coopSteamSetLobbyOptions(int pub, const char *password, int version)
{
    sPublic = pub != 0;
    snprintf(sPassword, sizeof(sPassword), "%s", password != NULL ? password : "");
    snprintf(sVersion, sizeof(sVersion), "%d", version);
    if (sReady)
    {
        applyLobbyOptions();
    }
}

/* ask Steam for the public games of our version (the answer comes in a pump) */
void coopSteamBrowseRefresh(void)
{
    if (!sReady || sListCall != 0)
    {
        return;
    }
    pAddLobbyStringFilter(sMM, LOBBY_KEY, "1", 0 /* equal */);
    pAddLobbyStringFilter(sMM, LOBBY_KEY_VERSION, sVersion, 0);
    pAddLobbyDistanceFilter(sMM, 3 /* worldwide */);
    sListCall = pRequestLobbyList(sMM);
}

/* -1 still looking (no list yet), else how many */
int coopSteamBrowseCount(void)
{
    return sListDone ? sNumPublic : -1;
}

const char *coopSteamBrowseName(int i)
{
    return (i >= 0 && i < sNumPublic) ? sPublicList[i].name : "";
}

int coopSteamBrowseMembers(int i)
{
    return (i >= 0 && i < sNumPublic) ? sPublicList[i].members : 0;
}

int coopSteamBrowseLocked(int i)
{
    return i >= 0 && i < sNumPublic && sPublicList[i].locked;
}

int coopSteamBrowseInMission(int i)
{
    return i >= 0 && i < sNumPublic && sPublicList[i].inmission;
}

/* host: in a mission (1) or in the menus (0), for Public Games */
void coopSteamSetInMission(int on)
{
    static int last = -1;

    on = on != 0;
    if (!sReady || !sIsHost || sLobby == 0)
    {
        last = -1;
        return;
    }
    if (on != last)
    {
        last = on;
        pSetLobbyData(sMM, sLobby, LOBBY_KEY_MISSION, on ? "1" : "0");
    }
}

unsigned long long coopSteamBrowseLobby(int i)
{
    return (i >= 0 && i < sNumPublic) ? sPublicList[i].lobby : 0;
}

/* the password the next steam:join sends, if its lobby wants one */
void coopSteamSetJoinPassword(const char *password)
{
    snprintf(sJoinPassword, sizeof(sJoinPassword), "%s", password != NULL ? password : "");
    sJoinHashSet = 0;
}

/* the next steam:join answers a password check with this hash (it came in a
 * Discord join secret, from a host that let us in) */
void coopSteamSetJoinPasswordHash(unsigned int hash)
{
    sJoinPassword[0] = '\0';
    sJoinHash = hash;
    sJoinHashSet = 1;
}

/* host: the hash a joiner's game must send for this lobby (1), or 0 with no password */
int coopSteamPasswordHash(unsigned int *hash)
{
    if (!sReady || !sIsHost || sLobby == 0 || sPassword[0] == '\0')
    {
        return 0;
    }
    *hash = pwHash(sPassword, sLobby);
    return 1;
}

/* the lobby we are in asks strangers for a password */
int coopSteamLobbyHasPassword(void)
{
    const char *v;

    if (!sReady || sLobby == 0)
    {
        return 0;
    }
    v = pGetLobbyData(sMM, sLobby, LOBBY_KEY_PASSWORD);
    return v != NULL && strcmp(v, "1") == 0;
}

unsigned long long coopSteamLobbyId(void)
{
    return sReady ? (unsigned long long) sLobby : 0;
}

/* ---- rich presence ------------------------------------------------------------ */

/* Steam up with no lobby, for presence and invites from the first menu.
 * Tried once: the lobby screen tries again when it opens. */
int coopSteamPresenceOpen(void)
{
    static int tried;

    if (!tried)
    {
        tried = 1;
        steamOpen();
    }
    return sReady;
}

/* callbacks (invites, Join Game) while no Steam session reads the transport */
void coopSteamPresenceTick(void)
{
    static unsigned int seen;

    if (sReady && sPumped == seen)
    {
        pump();
    }
    seen = sPumped;
}

/* What friends see. "status" is the line in the friend's game info; Steam's
 * friends list shows steam_display, a token from the app's rich presence
 * file: Spacewar's (AppID 480) has only its own lines (main menu, waiting for
 * a match), a game's own AppID ships coop/steam/rich_presence_english.vdf,
 * whose #Status shows our status as it is. steam_player_group puts players
 * of one lobby together in the list; "connect" is the Join Game button. */
void coopSteamSetPresence(const char *status, int phase, unsigned long long group, int groupsize, int joinable)
{
    char now[sizeof(sPresence)], buf[40];
    const char *display;

    if (!sReady)
    {
        return;
    }
    if (sAppId != APPID_SPACEWAR)
    {
        display = "#Status";
    }
    else
    {
        display = (phase != COOP_PRESENCE_MISSION) ? "#StatusWithoutScore" : "";   /* "At the main menu", "Waiting for match" */
    }
    snprintf(now, sizeof(now), "%s|%d|%llu|%d|%d", status, phase, group, groupsize, joinable);
    if (strcmp(now, sPresence) == 0)
    {
        return;
    }
    snprintf(sPresence, sizeof(sPresence), "%s", now);
    pSetRichPresence(sFriends, "status", status);
    pSetRichPresence(sFriends, "gamestatus", (phase == COOP_PRESENCE_MENU) ? "AtMainMenu" : (phase == COOP_PRESENCE_LOBBY) ? "WaitingForMatch" : "");
    pSetRichPresence(sFriends, "steam_display", display);
    snprintf(buf, sizeof(buf), "%llu", group);
    pSetRichPresence(sFriends, "steam_player_group", (group != 0 && groupsize > 1) ? buf : "");
    snprintf(buf, sizeof(buf), "%d", groupsize);
    pSetRichPresence(sFriends, "steam_player_group_size", (group != 0 && groupsize > 1) ? buf : "");
    snprintf(buf, sizeof(buf), "+connect_lobby %llu", group);
    pSetRichPresence(sFriends, "connect", (group != 0 && joinable) ? buf : "");
}

/* 1: in a passworded lobby, waiting for the host to check ours */
int coopSteamAdmitWaiting(void)
{
    return sReady && sAdmitWait;
}

/* once after a join failed: 1 the host turned our password down, 2 Steam
 * kept us out (friends only, full, gone), 3 the host's game is another
 * co-op version (coopSteamRefusedBuild says which build) */
int coopSteamJoinRefused(void)
{
    int r = sJoinRefused;

    sJoinRefused = 0;
    return r;
}

/* ---- voice (Steam Voice, src/game/coop_voice.c) ----------------------------
 * Steam records the microphone and compresses it itself, and only hands over
 * data while someone is talking (it drops silence). The game moves the
 * compressed bytes in its own packets and asks Steam here to decode them. */

static int sRecording;

/* 1 when Steam is up and has the voice calls */
int coopSteamVoiceAvailable(void)
{
    return sReady && pStartVoice != NULL;
}

void coopSteamVoiceRecord(int on)
{
    on = on != 0;
    if (!coopSteamVoiceAvailable() || on == sRecording)
    {
        return;
    }
    sRecording = on;
    if (on)
    {
        pStartVoice(sUser);
    }
    else
    {
        pStopVoice(sUser);  /* Steam keeps the last words coming for a moment */
    }
}

/* compressed voice recorded since the last call: bytes, 0 none */
int coopSteamVoiceRead(void *buf, int cap)
{
    unsigned int avail = 0, written = 0;
    int r;

    if (!coopSteamVoiceAvailable() || cap <= 0)
    {
        return 0;
    }
    r = pGetAvailableVoice(sUser, &avail, NULL, 0);
    if (r != 0 || avail == 0)
    {
        return 0;   /* 2 not recording, 3 no data: nobody is talking */
    }
    r = pGetVoice(sUser, 1, buf, (unsigned int) cap, &written, 0, NULL, 0, NULL, 0);
    if (r != 0 || written > (unsigned int) cap)
    {
        return 0;
    }
    return (int) written;
}

/* decode a partner's voice to 16-bit mono at `rate`: samples, 0 on a fault */
int coopSteamVoiceDecode(const void *in, int len, short *out, int cap, int rate)
{
    unsigned int written = 0;
    int r;

    if (!coopSteamVoiceAvailable() || len <= 0 || cap <= 0)
    {
        return 0;
    }
    r = pDecompressVoice(sUser, in, (unsigned int) len, out, (unsigned int) cap * 2, &written, (unsigned int) rate);
    if (r != 0)
    {
        return 0;
    }
    return (int) (written / 2);
}

/* after a refusal 3: the host's build ("" if it did not say), and ours */
const char *coopSteamRefusedBuild(void)
{
    return sRefusedBuild;
}

const char *coopSteamOwnBuild(void)
{
    return PORT_BUILD_COMMIT;
}

/* after a refusal 3: the host's release ("" from a build older than the updater) */
const char *coopSteamRefusedRelease(void)
{
    return sRefusedRelease;
}
