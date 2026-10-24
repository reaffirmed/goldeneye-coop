/**
 * Mock libsteam_api.so for two-copy tests on one machine (no Steam, no
 * accounts). It exports the flat-API subset port/src/coop_steam.c uses.
 *
 *   GE_MOCK_STEAMID=<id>   this copy's SteamID
 *   GE_MOCK_FRIEND=<id>[,<id>...]  this copy's friends (up to 4)
 *   GE_MOCK_DIR=<dir>      shared folder for lobby files (default /tmp/gecoop-mock)
 *
 * Messages travel over localhost UDP, port 30000 + id % 10000, each prefixed
 * with the sender's id. Like real Steam, the first message from a stranger is
 * held until AcceptSessionWithUser, a message's sender identity sits at byte
 * 12 (the real 32-bit DLL's layout), and LobbyCreated arrives twice.
 *
 * Lobby data lives in <lobby>.data (key=value lines, the owner and type
 * among them), so a public lobby can be listed (RequestLobbyList) by a copy
 * that is not the owner's friend.
 *
 * Build: gcc -m32 -shared -fPIC -O1 -o libsteam_api.so steam_mock.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

typedef unsigned long long u64;
typedef unsigned char sbool;

#define API __attribute__((visibility("default")))

static u64 sSelf;
static u64 sFriend[4];
static int sNumFriends;
static char sDir[256];
static int sSock = -1;
static u64 sNextCall = 1000;
static u64 sAccepted[8];
static int sNumAccepted;
static char sValue[64];
/* RequestLobbyList: the filters set for the next request, and its results */
static char sFilterKey[4][32], sFilterVal[4][64];
static int sNumFilters;
static u64 sListed[32];
static int sNumListed;

/* callback queue */
typedef struct { int id; int size; unsigned char param[512]; } Cb;
static Cb sCbs[64];
static int sCbHead, sCbTail;
/* call results */
typedef struct { u64 call; int id; int size; unsigned char data[512]; } Res;
static Res sRes[16];
static int sNumRes;

/* inbox */
typedef struct { u64 from; int size; unsigned char data[1500]; } In;
static In sIn[64];
static int sInHead, sInTail;

static void log_(const char *s) { fprintf(stderr, "steam-mock[%llu]: %s\n", sSelf, s); }

static void pushCb(int id, const void *p, int size)
{
    Cb *c = &sCbs[sCbTail % 64];
    c->id = id;
    c->size = size;
    memcpy(c->param, p, size);
    sCbTail++;
}

static u64 pushResult(int id, const void *p, int size)
{
    Res *r = &sRes[sNumRes++ % 16];
    unsigned char done[16];
    u64 call = sNextCall++;

    r->call = call;
    r->id = id;
    r->size = size;
    memcpy(r->data, p, size);
    memset(done, 0, sizeof(done));
    memcpy(done, &call, 8);
    memcpy(done + 8, &id, 4);
    memcpy(done + 12, &size, 4);
    pushCb(703, done, 16);
    return call;
}

static void path(char *out, size_t n, u64 id, const char *ext)
{
    snprintf(out, n, "%s/%llu.%s", sDir, id, ext);
}

/* a lobby's key, "" when it has none */
static const char *dataGet(u64 lobby, const char *key, char *out, int cap)
{
    char p[320], line[160];
    FILE *f;
    size_t kl = strlen(key);

    out[0] = '\0';
    path(p, sizeof(p), lobby, "data");
    f = fopen(p, "r");
    while (f != NULL && fgets(line, sizeof(line), f) != NULL)
    {
        if (strncmp(line, key, kl) == 0 && line[kl] == '=')
        {
            snprintf(out, cap, "%s", line + kl + 1);
            out[strcspn(out, "\n")] = '\0';
        }
    }
    if (f != NULL)
    {
        fclose(f);
    }
    return out;
}

static void dataSet(u64 lobby, const char *key, const char *value)
{
    char p[320], tmp[330], line[160];
    FILE *f, *o;
    size_t kl = strlen(key);

    path(p, sizeof(p), lobby, "data");
    snprintf(tmp, sizeof(tmp), "%s.%llu", p, sSelf);
    o = fopen(tmp, "w");
    if (o == NULL)
    {
        return;
    }
    f = fopen(p, "r");
    while (f != NULL && fgets(line, sizeof(line), f) != NULL)
    {
        if (!(strncmp(line, key, kl) == 0 && line[kl] == '='))
        {
            fputs(line, o);
        }
    }
    if (f != NULL)
    {
        fclose(f);
    }
    fprintf(o, "%s=%s\n", key, value);
    fclose(o);
    rename(tmp, p);
}

static u64 readLobbyOf(u64 user)
{
    char p[320];
    FILE *f;
    u64 v = 0;

    path(p, sizeof(p), user, "lobby");
    f = fopen(p, "r");
    if (f != NULL)
    {
        if (fscanf(f, "%llu", &v) != 1)
        {
            v = 0;
        }
        fclose(f);
    }
    return v;
}

static int accepted(u64 id)
{
    int i;
    for (i = 0; i < sNumAccepted; i++)
    {
        if (sAccepted[i] == id)
        {
            return 1;
        }
    }
    return 0;
}

static unsigned short portOf(u64 id)
{
    return (unsigned short) (30000 + id % 10000);
}

static void poll_(void)
{
    unsigned char buf[1600];
    int n;
    char ip[320];
    FILE *inv;

    path(ip, sizeof(ip), sSelf, "invite");
    inv = fopen(ip, "r");
    if (inv != NULL)
    {   /* GameLobbyJoinRequested_t: lobby, then who invited */
        u64 v[2] = { 0, 0 };

        if (fscanf(inv, "%llu %llu", &v[0], &v[1]) == 2)
        {
            pushCb(333, v, 16);
            log_("invite received");
        }
        fclose(inv);
        unlink(ip);
    }

    while ((n = (int) recv(sSock, buf, sizeof(buf), 0)) > 8)
    {
        In *m = &sIn[sInTail % 64];
        u64 from;

        memcpy(&from, buf, 8);
        if (!accepted(from))
        {
            /* SteamNetworkingMessagesSessionRequest_t: the identity */
            unsigned char ident[136];
            int t = 16;
            memset(ident, 0, sizeof(ident));
            memcpy(ident, &t, 4);
            t = 8;
            memcpy(ident + 4, &t, 4);
            memcpy(ident + 8, &from, 8);
            pushCb(1251, ident, sizeof(ident));
        }
        m->from = from;
        m->size = n - 8;
        memcpy(m->data, buf + 8, n - 8);
        sInTail++;
        if (sInTail - sInHead > 64)
        {
            sInHead = sInTail - 64;
        }
    }
}

API int SteamAPI_InitFlat(char *err)
{
    const char *e;
    struct sockaddr_in a;

    (void) err;
    e = getenv("GE_MOCK_STEAMID");
    sSelf = e ? strtoull(e, NULL, 10) : 76561190000000001ULL;
    e = getenv("GE_MOCK_FRIEND");
    while (e != NULL && *e != '\0' && sNumFriends < 4)
    {
        char *end;

        sFriend[sNumFriends] = strtoull(e, &end, 10);
        if (sFriend[sNumFriends] != 0)
        {
            sNumFriends++;
        }
        e = (*end == ',') ? end + 1 : NULL;
    }
    e = getenv("GE_MOCK_DIR");
    snprintf(sDir, sizeof(sDir), "%s", e ? e : "/tmp/gecoop-mock");
    mkdir(sDir, 0777);

    sSock = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(portOf(sSelf));
    if (bind(sSock, (struct sockaddr *) &a, sizeof(a)) != 0)
    {
        log_("bind failed");
        return 1;
    }
    fcntl(sSock, F_SETFL, O_NONBLOCK);
    log_("init");
    return 0;
}

API sbool SteamAPI_IsSteamRunning(void) { return 1; }
API void SteamAPI_Shutdown(void) { log_("shutdown"); }
API int SteamAPI_GetHSteamPipe(void) { return 1; }
API void SteamAPI_ManualDispatch_Init(void) {}
API void SteamAPI_ManualDispatch_RunFrame(int pipe) { (void) pipe; poll_(); }

API sbool SteamAPI_ManualDispatch_GetNextCallback(int pipe, unsigned char *msg)
{
    Cb *c;
    int user = 1;
    void *p;

    (void) pipe;
    if (sCbHead == sCbTail)
    {
        return 0;
    }
    c = &sCbs[sCbHead % 64];
    p = c->param;
    memcpy(msg, &user, 4);
    memcpy(msg + 4, &c->id, 4);
    memcpy(msg + 8, &p, 4);
    memcpy(msg + 12, &c->size, 4);
    return 1;
}

API void SteamAPI_ManualDispatch_FreeLastCallback(int pipe) { (void) pipe; sCbHead++; }

API sbool SteamAPI_ManualDispatch_GetAPICallResult(int pipe, u64 call, void *buf, int size, int expected, sbool *failed)
{
    int i;
    (void) pipe;
    for (i = 0; i < 16; i++)
    {
        if (sRes[i].call == call && sRes[i].id == expected && sRes[i].size <= size)
        {
            memcpy(buf, sRes[i].data, sRes[i].size);
            *failed = 0;
            return 1;
        }
    }
    *failed = 1;
    return 0;
}

static int sIface;
API void *SteamAPI_SteamUser_v023(void) { return &sIface; }
API void *SteamAPI_SteamMatchmaking_v009(void) { return &sIface; }
API void *SteamAPI_SteamFriends_v018(void) { return &sIface; }
API void *SteamAPI_SteamNetworkingMessages_SteamAPI_v002(void) { return &sIface; }

API u64 SteamAPI_ISteamUser_GetSteamID(void *self) { (void) self; return sSelf; }

API u64 SteamAPI_ISteamMatchmaking_CreateLobby(void *self, int type, int max)
{
    unsigned char r[16];
    u64 lobby = 0x0186000000000000ULL | (sSelf & 0xFFFFFFFFFFFFULL);
    int ok = 1;
    char p[320];
    FILE *f;
    u64 call;

    (void) self; (void) max;
    path(p, sizeof(p), sSelf, "lobby");
    f = fopen(p, "w");
    fprintf(f, "%llu\n", lobby);
    fclose(f);
    path(p, sizeof(p), lobby, "data");
    unlink(p);
    path(p, sizeof(p), lobby, "members");
    unlink(p);
    {
        char v[32];
        snprintf(v, sizeof(v), "%llu", sSelf);
        dataSet(lobby, "__owner", v);
        snprintf(v, sizeof(v), "%d", type);
        dataSet(lobby, "__type", v);
    }
    memset(r, 0, sizeof(r));
    memcpy(r, &ok, 4);
    memcpy(r + 8, &lobby, 8);
    call = pushResult(513, r, 16);
    pushCb(513, r, 16); /* real Steam also broadcasts it */
    log_("lobby created");
    return call;
}

API u64 SteamAPI_ISteamMatchmaking_JoinLobby(void *self, u64 lobby)
{
    unsigned char r[24];
    unsigned int resp = 1;
    char p[320];
    FILE *f;

    (void) self;
    path(p, sizeof(p), lobby, "members");
    f = fopen(p, "a");
    fprintf(f, "%llu\n", sSelf);
    fclose(f);
    memset(r, 0, sizeof(r));
    memcpy(r, &lobby, 8);
    memcpy(r + 16, &resp, 4);
    log_("joined lobby");
    return pushResult(504, r, 24);
}

API void SteamAPI_ISteamMatchmaking_LeaveLobby(void *self, u64 lobby)
{
    char p[320], tmp[330];
    FILE *f, *o;
    u64 v;
    (void) self;
    if ((lobby & 0xFFFFFFFFFFFFULL) == (sSelf & 0xFFFFFFFFFFFFULL))
    {
        path(p, sizeof(p), sSelf, "lobby");
        unlink(p);
        path(p, sizeof(p), lobby, "data");
        unlink(p);
        return;
    }
    /* a member: off the member list */
    path(p, sizeof(p), lobby, "members");
    snprintf(tmp, sizeof(tmp), "%s.%llu", p, sSelf);
    f = fopen(p, "r");
    o = fopen(tmp, "w");
    while (f != NULL && o != NULL && fscanf(f, "%llu", &v) == 1)
    {
        if (v != sSelf)
        {
            fprintf(o, "%llu\n", v);
        }
    }
    if (f != NULL)
    {
        fclose(f);
    }
    if (o != NULL)
    {
        fclose(o);
        rename(tmp, p);
    }
    log_("left lobby");
}

API sbool SteamAPI_ISteamMatchmaking_SetLobbyData(void *self, u64 l, const char *k, const char *v)
{
    (void) self;
    dataSet(l, k, v);
    return 1;
}

API const char *SteamAPI_ISteamMatchmaking_GetLobbyData(void *self, u64 l, const char *k)
{
    (void) self;
    dataGet(l, k, sValue, sizeof(sValue));
    if (sValue[0] == '\0' && strcmp(k, "gecoop") == 0)
    {
        snprintf(sValue, sizeof(sValue), "1");  /* a lobby made before the data file */
    }
    return sValue;
}

API sbool SteamAPI_ISteamMatchmaking_SetLobbyType(void *self, u64 l, int type)
{
    char v[16];
    (void) self;
    snprintf(v, sizeof(v), "%d", type);
    dataSet(l, "__type", v);
    log_(type == 2 ? "lobby type public" : "lobby type friends only");
    return 1;
}

API void SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter(void *self, const char *k, const char *v, int cmp)
{
    (void) self; (void) cmp;
    if (sNumFilters < 4)
    {
        snprintf(sFilterKey[sNumFilters], sizeof(sFilterKey[0]), "%s", k);
        snprintf(sFilterVal[sNumFilters], sizeof(sFilterVal[0]), "%s", v);
        sNumFilters++;
    }
}

API void SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter(void *self, int f) { (void) self; (void) f; }

/* every public lobby in the folder that passes the filters */
API u64 SteamAPI_ISteamMatchmaking_RequestLobbyList(void *self)
{
    DIR *d = opendir(sDir);
    struct dirent *e;
    unsigned int n;
    char v[64];

    (void) self;
    sNumListed = 0;
    while (d != NULL && (e = readdir(d)) != NULL && sNumListed < 32)
    {
        const char *dot = strrchr(e->d_name, '.');
        u64 lobby;
        int i, ok;

        if (dot == NULL || strcmp(dot, ".data") != 0)
        {
            continue;
        }
        lobby = strtoull(e->d_name, NULL, 10);
        ok = strcmp(dataGet(lobby, "__type", v, sizeof(v)), "2") == 0;
        for (i = 0; i < sNumFilters && ok; i++)
        {
            ok = strcmp(dataGet(lobby, sFilterKey[i], v, sizeof(v)), sFilterVal[i]) == 0;
        }
        if (ok)
        {
            sListed[sNumListed++] = lobby;
        }
    }
    if (d != NULL)
    {
        closedir(d);
    }
    sNumFilters = 0;
    n = (unsigned int) sNumListed;
    log_("lobby list");
    return pushResult(510, &n, 4);
}

API u64 SteamAPI_ISteamMatchmaking_GetLobbyByIndex(void *self, int i)
{
    (void) self;
    return (i >= 0 && i < sNumListed) ? sListed[i] : 0;
}

static u64 ownerOf(u64 lobby)
{
    /* mock ids keep the owner's low 48 bits; the high bits are the usual 7656119... */
    int i;
    char v[32];

    if (dataGet(lobby, "__owner", v, sizeof(v))[0] != '\0')
    {
        return strtoull(v, NULL, 10);
    }

    for (i = 0; i < sNumFriends; i++)
    {
        if ((sFriend[i] & 0xFFFFFFFFFFFFULL) == (lobby & 0xFFFFFFFFFFFFULL))
        {
            return sFriend[i];
        }
    }
    return sSelf;
}

API u64 SteamAPI_ISteamMatchmaking_GetLobbyOwner(void *self, u64 lobby) { (void) self; return ownerOf(lobby); }

static int members(u64 lobby, u64 *out, int max)
{
    char p[320];
    FILE *f;
    int n = 0;
    u64 v;

    out[n++] = ownerOf(lobby);
    path(p, sizeof(p), lobby, "members");
    f = fopen(p, "r");
    while (f != NULL && n < max && fscanf(f, "%llu", &v) == 1)
    {
        out[n++] = v;
    }
    if (f != NULL)
    {
        fclose(f);
    }
    return n;
}

API int SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(void *self, u64 lobby)
{
    u64 m[16];
    (void) self;
    return members(lobby, m, 16);
}

API u64 SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(void *self, u64 lobby, int i)
{
    u64 m[16];
    int n;
    (void) self;
    n = members(lobby, m, 16);
    return (i >= 0 && i < n) ? m[i] : 0;
}

API sbool SteamAPI_ISteamFriends_SetRichPresence(void *self, const char *k, const char *v)
{
    char b[300];
    (void) self;
    snprintf(b, sizeof(b), "rich presence %s=%s", k, v);
    log_(b);
    return 1;
}

API void SteamAPI_ISteamFriends_ClearRichPresence(void *self) { (void) self; }
API void SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog(void *self, u64 l) { (void) self; (void) l; log_("invite overlay"); }
API int SteamAPI_ISteamFriends_GetFriendCount(void *self, int flags) { (void) self; (void) flags; return sNumFriends; }
API u64 SteamAPI_ISteamFriends_GetFriendByIndex(void *self, int i, int flags) { (void) self; (void) flags; return (i >= 0 && i < sNumFriends) ? sFriend[i] : 0; }

API sbool SteamAPI_ISteamFriends_GetFriendGamePlayed(void *self, u64 user, unsigned char *info)
{
    u64 lobby = readLobbyOf(user);
    u64 game = 480;
    (void) self;
    if (lobby == 0)
    {
        return 0;
    }
    memset(info, 0, 24);
    memcpy(info, &game, 8);
    memcpy(info + 16, &lobby, 8);
    return 1;
}

API int SteamAPI_ISteamFriends_GetFriendRelationship(void *self, u64 user)
{
    int i;
    (void) self;
    for (i = 0; i < sNumFriends; i++)
    {
        if (sFriend[i] == user)
        {
            return 3; /* friend */
        }
    }
    return 0;
}

API int SteamAPI_ISteamFriends_GetFriendPersonaState(void *self, u64 user) { (void) self; (void) user; return 1; /* online */ }

/* an invite is a file the friend's poll_ picks up as GameLobbyJoinRequested */
API sbool SteamAPI_ISteamMatchmaking_InviteUserToLobby(void *self, u64 lobby, u64 user)
{
    char p[320];
    FILE *f;

    (void) self;
    path(p, sizeof(p), user, "invite");
    f = fopen(p, "w");
    if (f == NULL)
    {
        return 0;
    }
    fprintf(f, "%llu %llu\n", lobby, sSelf);
    fclose(f);
    log_("invite sent");
    return 1;
}

API const char *SteamAPI_ISteamFriends_GetFriendPersonaName(void *self, u64 user)
{
    static char name[8][32];
    static int k;
    (void) self;
    k = (k + 1) & 7;
    snprintf(name[k], sizeof(name[k]), "mock%llu", user % 1000);
    return name[k];
}

API void SteamAPI_SteamNetworkingIdentity_SetSteamID64(unsigned char *ident, u64 id)
{
    int t = 16, sz = 8;
    memset(ident, 0, 136);
    memcpy(ident, &t, 4);
    memcpy(ident + 4, &sz, 4);
    memcpy(ident + 8, &id, 8);
}

API u64 SteamAPI_SteamNetworkingIdentity_GetSteamID64(const unsigned char *ident)
{
    int t;
    u64 id;
    memcpy(&t, ident, 4);
    if (t != 16)
    {
        return 0;
    }
    memcpy(&id, ident + 8, 8);
    return id;
}

API int SteamAPI_ISteamNetworkingMessages_SendMessageToUser(void *self, const unsigned char *ident, const void *data, unsigned int len, int flags, int ch)
{
    unsigned char buf[1600];
    struct sockaddr_in a;
    u64 to = SteamAPI_SteamNetworkingIdentity_GetSteamID64(ident);
    (void) self; (void) flags; (void) ch;

    if (len > 1500 || to == 0)
    {
        return 2;
    }
    memcpy(buf, &sSelf, 8);
    memcpy(buf + 8, data, len);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(portOf(to));
    sendto(sSock, buf, len + 8, 0, (struct sockaddr *) &a, sizeof(a));
    if (!accepted(to) && sNumAccepted < 8)
    {
        sAccepted[sNumAccepted++] = to; /* sending opens the session our way */
    }
    return 1;
}

API int SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(void *self, int ch, void **out, int max)
{
    (void) self; (void) ch;
    int i;

    poll_();
    while (sInHead != sInTail && sIn[sInHead % 64].size < 0)
    {
        sInHead++;   /* taken out of order earlier */
    }
    for (i = sInHead; max > 0 && i != sInTail; i++)
    {
        In *m = &sIn[i % 64];
        unsigned char *msg;

        if (m->size < 0 || !accepted(m->from))
        {
            continue; /* held until the session is accepted */
        }
        msg = calloc(1, 12 + 136 + m->size);
        {
            void *data = msg + 12 + 136;
            memcpy(data, m->data, m->size);
            memcpy(msg, &data, 4);
            memcpy(msg + 4, &m->size, 4);
            SteamAPI_SteamNetworkingIdentity_SetSteamID64(msg + 12, m->from);
        }
        out[0] = msg;
        m->size = -1;
        return 1;
    }
    return 0;
}

API sbool SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(void *self, const unsigned char *ident)
{
    u64 id = SteamAPI_SteamNetworkingIdentity_GetSteamID64(ident);
    (void) self;
    if (!accepted(id) && sNumAccepted < 8)
    {
        sAccepted[sNumAccepted++] = id;
    }
    log_("session accepted");
    return 1;
}

API sbool SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser(void *self, const unsigned char *ident) { (void) self; (void) ident; return 1; }
API void SteamAPI_SteamNetworkingMessage_t_Release(void *msg) { free(msg); }
