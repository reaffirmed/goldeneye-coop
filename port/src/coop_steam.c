/**
 * Co-op over Steam, as AppID 480 "Spacewar" (playbook: coop-anything
 * networking-steam-spacewar.md).
 *
 * steam_api.dll (libsteam_api.so on Linux) is loaded at run time and only
 * its flat C exports are used, so no Steamworks headers or import libs are
 * needed. Struct fields are read at fixed byte offsets, because the DLL is
 * built with 8-byte packing and gcc on i686 aligns 64-bit fields to 4.
 *
 * Host:  creates a friends-only lobby, advertises it in rich presence and
 *        opens the invite overlay. Friends join with Join Game, an invite,
 *        or by starting with GE_COOP=steam:join.
 * Join:  GE_COOP=steam:join:<lobbyid>, "+connect_lobby <id>" from Steam, an
 *        accepted invite, or, with no id, the first friend found hosting.
 *
 * Game packets go to the other player's SteamID through
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

extern void portConLogf(const char *fmt, ...);
extern const char *portRootPath(const char *name);

typedef unsigned long long u64s;
typedef unsigned char sbool; /* C++ bool in the DLL: only the low byte is set */

#define APPID_SPACEWAR 480
#define LOBBY_KEY "gecoop"

/* callback ids */
enum
{
    CB_GameLobbyJoinRequested = 333,
    CB_GameRichPresenceJoinRequested = 337,
    CB_LobbyEnter = 504,
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
static void (*pIdentSetSteamID64)(void *ident, u64s id);
static u64s (*pIdentGetSteamID64)(void *ident);
static int (*pSendMessageToUser)(void *self, const void *ident, const void *data, unsigned int len, int flags, int channel);
static int (*pReceiveMessagesOnChannel)(void *self, int channel, void **out, int max);
static sbool (*pAcceptSessionWithUser)(void *self, const void *ident);
static sbool (*pCloseSessionWithUser)(void *self, const void *ident);
static void (*pMessageRelease)(void *msg);

static void *sLib;
static void *sUser, *sMM, *sFriends, *sNM;
static int sPipe;
static int sIsHost;
static u64s sSelf;
static u64s sLobby;
static u64s sPeer;          /* the other player's SteamID, 0 until known */
static unsigned char sPeerIdent[IDENTITY_SIZE];
static u64s sCreateCall, sJoinCall;
static int sJoinWanted;     /* joiner: still looking for a lobby */
static unsigned int sPolls;
static int sSenderOfs;      /* 12 or 16, see coopSteamRecv */

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
    L(pIdentSetSteamID64, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
    L(pIdentGetSteamID64, "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
    L(pSendMessageToUser, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    L(pReceiveMessagesOnChannel, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    L(pAcceptSessionWithUser, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    L(pCloseSessionWithUser, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
    L(pMessageRelease, "SteamAPI_SteamNetworkingMessage_t_Release");
#undef L
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

static void setPeer(u64s id)
{
    sPeer = id;
    memset(sPeerIdent, 0, sizeof(sPeerIdent));
    pIdentSetSteamID64(sPeerIdent, id);
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
        if (pGetFriendGamePlayed(sFriends, f, info) && (rd64(info) & 0xFFFFFF) == APPID_SPACEWAR && rd64(info + 16) != 0)
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
    char buf[64];

    if (result != 1 /* k_EResultOK */ || lobby == 0)
    {
        portConLogf("ge: steam: lobby creation failed (result %d)\n", result);
        return;
    }
    if (lobby == sLobby)
    {
        return; /* real Steam reports it twice: call result and broadcast */
    }
    sLobby = lobby;
    pSetLobbyData(sMM, lobby, LOBBY_KEY, "1");
    snprintf(buf, sizeof(buf), "+connect_lobby %llu", lobby);
    pSetRichPresence(sFriends, "connect", buf);
    pSetRichPresence(sFriends, "status", "Hosting GoldenEye co-op");
    portConLogf("ge: steam: lobby %llu open, inviting friends\n", lobby);
    pInviteDialog(sFriends, lobby);
}

static void onLobbyEnter(const unsigned char *p)
{
    u64s lobby = rd64(p);
    unsigned int response = rd32(p + 16);
    const char *tag;

    if (sIsHost)
    {
        return; /* our own lobby */
    }
    if (response != 1)
    {
        portConLogf("ge: steam: could not join lobby %llu (response %u)\n", lobby, response);
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
    sLobby = lobby;
    setPeer(pGetLobbyOwner(sMM, lobby));
    pSetRichPresence(sFriends, "status", "Playing GoldenEye co-op");
    portConLogf("ge: steam: in lobby %llu, host %s\n", lobby, pGetFriendPersonaName(sFriends, sPeer));
}

static void onChatUpdate(const unsigned char *p)
{
    u64s lobby = rd64(p);
    u64s who = rd64(p + 8);
    unsigned int change = rd32(p + 24);

    if (lobby != sLobby || who != sPeer || !(change & (0x2 | 0x4 | 0x8 | 0x10)))
    {
        return;
    }
    portConLogf("ge: steam: %s left\n", pGetFriendPersonaName(sFriends, who));
    pCloseSessionWithUser(sNM, sPeerIdent);
    sPeer = 0;
    if (!sIsHost)
    {
        sLobby = 0;
        sJoinWanted = 1; /* the host may come back with a new lobby */
    }
}

static void onSessionRequest(const unsigned char *p)
{
    u64s from = pIdentGetSteamID64((void *) p);

    if ((sIsHost && sPeer == 0 && isLobbyMember(from)) || from == sPeer)
    {
        pAcceptSessionWithUser(sNM, p);
        if (sPeer == 0)
        {
            setPeer(from);
            portConLogf("ge: steam: %s joined\n", pGetFriendPersonaName(sFriends, from));
        }
        return;
    }
    portConLogf("ge: steam: ignoring a session from %llu (not in our lobby)\n", from);
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
        case CB_GameLobbyJoinRequested:
            if (!sIsHost)
            {
                joinLobby(rd64(p));
            }
            break;
        case CB_GameRichPresenceJoinRequested:
        {
            const char *c = strstr((const char *) p + 8, "+connect_lobby ");
            if (!sIsHost && c != NULL)
            {
                joinLobby(strtoull(c + 15, NULL, 10));
            }
            break;
        }
        case CB_NetMsgSessionRequest:
            onSessionRequest(p);
            break;
        case CB_NetMsgSessionFailed:
            if (pIdentGetSteamID64((void *) p) == sPeer && sPeer != 0)
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
    sPolls++;
}

int coopSteamInit(const char *spec)
{
    char err[1024];
    int r;

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

    /* "steam" hosts; "steam:join" finds a friend; "steam:join:<id>" joins that lobby */
    if (strncmp(spec, "steam:join", 10) == 0)
    {
        sIsHost = 0;
        if (spec[10] == ':' && spec[11] != '\0')
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
        sCreateCall = pCreateLobby(sMM, 1 /* friends only */, 2);
        portConLogf("ge: steam: creating a lobby\n");
    }
    portConLogf("ge: steam: ready as %s (%llu)\n", pGetFriendPersonaName(sFriends, sSelf), sSelf);
    return 1;
}

int coopSteamIsHost(void)
{
    return sIsHost;
}

int coopSteamHasPeer(void)
{
    return sPeer != 0;
}

void coopSteamSend(const void *buf, int len)
{
    if (sPeer == 0)
    {
        return;
    }
    pSendMessageToUser(sNM, sPeerIdent, buf, (unsigned int) len, SEND_UNRELIABLE_NONAGLE, 0);
}

int coopSteamRecv(void *buf, int cap)
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

        if (from == sPeer && size > 0 && size <= cap)
        {
            memcpy(buf, data, (size_t) size);
            pMessageRelease(msgs[0]);
            return size;
        }
        pMessageRelease(msgs[0]); /* someone else, or too big: drop */
    }
}

void coopSteamShutdown(void)
{
    if (sLib == NULL)
    {
        return;
    }
    if (sPeer != 0)
    {
        pCloseSessionWithUser(sNM, sPeerIdent);
    }
    if (sLobby != 0)
    {
        pLeaveLobby(sMM, sLobby);
    }
    pClearRichPresence(sFriends);
    pShutdown();
    sPeer = sLobby = 0;
}
