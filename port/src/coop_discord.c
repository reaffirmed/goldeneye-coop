/**
 * Discord Rich Presence and Ask to Join, over Discord's local RPC socket.
 *
 * No SDK: the Discord client listens on a named pipe (\\.\pipe\discord-ipc-N
 * on Windows) or a Unix socket ($XDG_RUNTIME_DIR/discord-ipc-N, with the snap
 * and flatpak folders as fallbacks), N = 0..9. Each frame is a little-endian
 * opcode and length, then JSON: 0 handshake, 1 frame, 2 close, 3 ping, 4 pong.
 * This is the protocol discord-rpc and the Game SDK's presence speak.
 *
 * The application id is GoldenEye Coop's own (DISCORD_APPID); GE_DISCORD_APPID
 * or discord_appid.txt beside the exe replace it, and one without digits (or
 * GE_NO_DISCORD) turns Discord off. Nothing here blocks: when Discord isn't
 * running the game looks for it again every 15 s, and when Discord turns the
 * game away before READY (a bad id) it waits longer each time, up to 5 min.
 *
 * The activity carries a party (the Steam lobby) and a join secret, so
 * friends see Ask to Join / Join and the "invite to play" chat button. The
 * secret reaches a joiner's game as ACTIVITY_JOIN; a host's game hears
 * ACTIVITY_JOIN_REQUEST and answers with SEND_ACTIVITY_JOIN_INVITE or
 * CLOSE_ACTIVITY_REQUEST (the lobby screen asks the host, coop_presence.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define GETPID() ((int) GetCurrentProcessId())
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#define GETPID() ((int) getpid())
#endif

#include "coop_net.h"

/* the repo's include/string.h is the N64 stub (memcpy, strchr, strlen) */
extern void *memset(void *, int, size_t);
extern void *memmove(void *, const void *, size_t);
extern int strcmp(const char *, const char *);
extern int strncmp(const char *, const char *, size_t);
extern char *strstr(const char *, const char *);

extern void portConLogf(const char *fmt, ...);
extern const char *portRootPath(const char *name);

#define DISCORD_APPID "1565798461310246992"     /* GoldenEye Coop on discord.com/developers */
#define DISCORD_RETRY_MS 15000
#define DISCORD_RETRY_MAX_MS 300000
#define DISCORD_BUF 16384
#define DISCORD_ASK_MS 30000
#define DISCORD_ACTIVITY_MS 4000    /* Discord takes 5 activity updates per 20 s */

enum { OP_HANDSHAKE, OP_FRAME, OP_CLOSE, OP_PING, OP_PONG };

#ifdef _WIN32
static HANDLE sPipe = INVALID_HANDLE_VALUE;
#else
static int sSock = -1;
#endif
static char sAppId[32];
static int sAppChecked;     /* the id was looked for */
static int sReady;          /* READY came back: presence goes through */
static unsigned int sNextTry;
static int sToldMissing;    /* "not running" logged once */
static int sRefused;        /* connections closed before READY, in a row */
static unsigned int sNonce;
static char sIn[DISCORD_BUF];
static int sInLen;
static char sLastActivity[2048];    /* resent after a reconnect */
static int sActivityDirty;
static unsigned int sActivitySent;  /* when, for DISCORD_ACTIVITY_MS */

/* what came in for the game (coop_presence.c takes them) */
static char sJoinSecret[128];
static struct { char id[32]; char name[40]; unsigned int at; } sAsk[4];
static int sNumAsk;

static unsigned int nowMs(void)
{
#ifdef _WIN32
    return (unsigned int) GetTickCount();
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned int) (ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
#endif
}

static int isOpen(void)
{
#ifdef _WIN32
    return sPipe != INVALID_HANDLE_VALUE;
#else
    return sSock >= 0;
#endif
}

/* 15 s, doubling with each refusal up to 5 min */
static unsigned int retryMs(void)
{
    unsigned int ms = DISCORD_RETRY_MS;
    int i;

    for (i = 0; i < sRefused && ms < DISCORD_RETRY_MAX_MS; i++)
    {
        ms *= 2;
    }
    return ms < DISCORD_RETRY_MAX_MS ? ms : DISCORD_RETRY_MAX_MS;
}

static void closeIpc(void)
{
    const int wasOpen = isOpen();

#ifdef _WIN32
    if (sPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(sPipe);
        sPipe = INVALID_HANDLE_VALUE;
    }
#else
    if (sSock >= 0)
    {
        close(sSock);
        sSock = -1;
    }
#endif
    if (sReady)
    {
        portConLogf("ge: discord: disconnected\n");
    }
    else if (wasOpen)
    {
        sRefused++;     /* Discord was there but never said READY */
    }
    sReady = 0;
    sInLen = 0;
    sNumAsk = 0;
    sNextTry = nowMs() + retryMs();
}

/* the whole buffer, or 0 (and the connection is closed) */
static int writeAll(const void *buf, int len)
{
    const char *p = (const char *) buf;

    while (len > 0)
    {
#ifdef _WIN32
        DWORD n = 0;

        if (!WriteFile(sPipe, p, (DWORD) len, &n, NULL) || n == 0)
        {
            closeIpc();
            return 0;
        }
#else
        ssize_t n = send(sSock, p, (size_t) len, MSG_NOSIGNAL);

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        {
            continue;   /* a few hundred bytes: the socket drains at once */
        }
        if (n <= 0)
        {
            closeIpc();
            return 0;
        }
#endif
        p += n;
        len -= (int) n;
    }
    return 1;
}

static int sendFrame(int op, const char *json)
{
    unsigned int hdr[2];
    int len = (int) strlen(json);

    if (!isOpen())
    {
        return 0;
    }
    hdr[0] = (unsigned int) op;
    hdr[1] = (unsigned int) len;
    return writeAll(hdr, sizeof(hdr)) && writeAll(json, len);
}

/* bytes waiting, read into sIn; 0 when the pipe is gone */
static int readSome(void)
{
#ifdef _WIN32
    DWORD avail = 0, got = 0;

    if (!PeekNamedPipe(sPipe, NULL, 0, NULL, &avail, NULL))
    {
        closeIpc();
        return 0;
    }
    if (avail == 0 || sInLen >= DISCORD_BUF)
    {
        return 1;
    }
    if (avail > (DWORD) (DISCORD_BUF - sInLen))
    {
        avail = (DWORD) (DISCORD_BUF - sInLen);
    }
    if (!ReadFile(sPipe, sIn + sInLen, avail, &got, NULL))
    {
        closeIpc();
        return 0;
    }
    sInLen += (int) got;
    return 1;
#else
    for (;;)
    {
        ssize_t n;

        if (sInLen >= DISCORD_BUF)
        {
            return 1;
        }
        n = recv(sSock, sIn + sInLen, (size_t) (DISCORD_BUF - sInLen), 0);
        if (n > 0)
        {
            sInLen += (int) n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        {
            return 1;
        }
        closeIpc();     /* 0: Discord closed it */
        return 0;
    }
#endif
}

/* ---- a little JSON ------------------------------------------------------------ */

/* "text" with quotes, backslashes and control characters escaped */
static void jsonQuote(char *out, int cap, const char *s)
{
    int n = 0;

    if (cap < 3)
    {
        return;
    }
    out[n++] = '"';
    for (; s != NULL && *s != '\0' && n < cap - 8; s++)
    {
        const unsigned char ch = (unsigned char) *s;

        if (ch == '"' || ch == '\\')
        {
            out[n++] = '\\';
            out[n++] = (char) ch;
        }
        else if (ch < 0x20)
        {
            n += snprintf(out + n, (size_t) (cap - n), "\\u%04x", ch);
        }
        else
        {
            out[n++] = (char) ch;
        }
    }
    out[n++] = '"';
    out[n] = '\0';
}

/* the string value of "key" at or after `from`, unescaped into out; NULL when absent */
static const char *jsonString(const char *from, const char *key, char *out, int cap)
{
    char pat[48];
    const char *p;
    int n = 0;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(from, pat);
    if (p == NULL)
    {
        return NULL;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':')
    {
        p++;
    }
    if (*p != '"')
    {
        return NULL;    /* null, a number, an object */
    }
    for (p++; *p != '\0' && *p != '"' && n < cap - 1; p++)
    {
        if (*p == '\\' && p[1] != '\0')
        {
            p++;
            if (*p == 'u')
            {   /* \uXXXX: names go to the menu font as '?' past ASCII */
                unsigned int u = (unsigned int) strtoul(p + 1, NULL, 16);
                out[n++] = (u >= 0x20 && u < 0x7F) ? (char) u : '?';
                p += (strlen(p) >= 5) ? 4 : strlen(p) - 1;
                continue;
            }
        }
        out[n++] = *p;
    }
    out[n] = '\0';
    return out;
}

/* ---- connecting --------------------------------------------------------------- */

static void loadAppId(void)
{
    const char *e = getenv("GE_DISCORD_APPID");
    FILE *f;
    int i;

    sAppChecked = 1;
    snprintf(sAppId, sizeof(sAppId), "%s", DISCORD_APPID);
    if (e != NULL)
    {
        snprintf(sAppId, sizeof(sAppId), "%s", e);
    }
    else if ((f = fopen(portRootPath("discord_appid.txt"), "r")) != NULL)
    {
        if (fgets(sAppId, sizeof(sAppId), f) == NULL)
        {
            sAppId[0] = '\0';
        }
        fclose(f);
    }
    for (i = 0; sAppId[i] >= '0' && sAppId[i] <= '9'; i++)
    {
    }
    sAppId[i] = '\0';   /* digits only: a snowflake */
    if (sAppId[0] == '\0')
    {
        portConLogf("ge: discord: no application id (discord_appid.txt or GE_DISCORD_APPID); Discord presence is off\n");
    }
    else if (getenv("GE_NO_DISCORD") != NULL)
    {
        portConLogf("ge: discord: GE_NO_DISCORD; Discord presence is off\n");
    }
}

#ifdef _WIN32
/* So Discord can start the game for an accepted Ask to Join when it isn't
 * running: the discord-<appid> URL protocol under HKCU, as discord-rpc's
 * Discord_Register writes it. */
static void registerLauncher(void)
{
    char exe[MAX_PATH], key[96], cmd[MAX_PATH + 8], desc[64];
    HKEY k;

    if (GetModuleFileNameA(NULL, exe, sizeof(exe)) == 0)
    {
        return;
    }
    snprintf(key, sizeof(key), "Software\\Classes\\discord-%s", sAppId);
    if (RegCreateKeyExA(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
    {
        return;
    }
    snprintf(desc, sizeof(desc), "URL:Run game %s protocol", sAppId);
    RegSetValueExA(k, NULL, 0, REG_SZ, (const BYTE *) desc, (DWORD) strlen(desc) + 1);
    RegSetValueExA(k, "URL Protocol", 0, REG_SZ, (const BYTE *) "", 1);
    RegCloseKey(k);
    snprintf(key, sizeof(key), "Software\\Classes\\discord-%s\\DefaultIcon", sAppId);
    if (RegCreateKeyExA(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS)
    {
        RegSetValueExA(k, NULL, 0, REG_SZ, (const BYTE *) exe, (DWORD) strlen(exe) + 1);
        RegCloseKey(k);
    }
    snprintf(key, sizeof(key), "Software\\Classes\\discord-%s\\shell\\open\\command", sAppId);
    if (RegCreateKeyExA(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS)
    {
        snprintf(cmd, sizeof(cmd), "\"%s\"", exe);
        RegSetValueExA(k, NULL, 0, REG_SZ, (const BYTE *) cmd, (DWORD) strlen(cmd) + 1);
        RegCloseKey(k);
    }
}
#else
static void registerLauncher(void)
{   /* Linux: Discord launches through a discord-<appid> scheme handler; not set up here */
}
#endif

static int openIpc(void)
{
    int i;

#ifdef _WIN32
    for (i = 0; i < 10; i++)
    {
        char name[64];

        snprintf(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%d", i);
        sPipe = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (sPipe != INVALID_HANDLE_VALUE)
        {
            return 1;
        }
    }
    return 0;
#else
    static const char *const sub[] = { "", "app/com.discordapp.Discord/", "snap.discord/", "app/com.discordapp.DiscordCanary/" };
    const char *dirs[4];
    int d, s;

    dirs[0] = getenv("XDG_RUNTIME_DIR");
    dirs[1] = getenv("TMPDIR");
    dirs[2] = getenv("TMP");
    dirs[3] = "/tmp";
    for (d = 0; d < 4; d++)
    {
        if (dirs[d] == NULL || dirs[d][0] == '\0')
        {
            continue;
        }
        for (s = 0; s < 4; s++)
        {
            for (i = 0; i < 10; i++)
            {
                struct sockaddr_un a;
                int fd;

                memset(&a, 0, sizeof(a));
                a.sun_family = AF_UNIX;
                snprintf(a.sun_path, sizeof(a.sun_path), "%s/%sdiscord-ipc-%d", dirs[d], sub[s], i);
                if (access(a.sun_path, F_OK) != 0)
                {
                    continue;
                }
                fd = socket(AF_UNIX, SOCK_STREAM, 0);
                if (fd < 0)
                {
                    return 0;
                }
                if (connect(fd, (struct sockaddr *) &a, sizeof(a)) == 0)
                {
                    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                    sSock = fd;
                    return 1;
                }
                close(fd);
            }
        }
    }
    return 0;
#endif
}

static void tryConnect(void)
{
    char json[128];

    if (!sAppChecked)
    {
        loadAppId();
    }
    if (sAppId[0] == '\0' || getenv("GE_NO_DISCORD") != NULL || (int) (nowMs() - sNextTry) < 0)
    {
        return;
    }
    sNextTry = nowMs() + DISCORD_RETRY_MS;
    if (!openIpc())
    {
        if (!sToldMissing)
        {
            sToldMissing = 1;
            portConLogf("ge: discord: Discord isn't running; presence is off until it starts\n");
        }
        return;
    }
    snprintf(json, sizeof(json), "{\"v\":1,\"client_id\":\"%s\"}", sAppId);
    sendFrame(OP_HANDSHAKE, json);
}

static void sendCmd(const char *cmd, const char *argsjson, const char *evt)
{
    char json[2400];

    snprintf(json, sizeof(json), "{\"cmd\":\"%s\",\"args\":%s,%s%s%s\"nonce\":\"%u\"}",
             cmd, argsjson, evt ? "\"evt\":\"" : "", evt ? evt : "", evt ? "\"," : "", ++sNonce);
    sendFrame(OP_FRAME, json);
}

static void onReady(void)
{
    static int registered;

    sReady = 1;
    sToldMissing = 0;
    sRefused = 0;
    portConLogf("ge: discord: connected (application %s)\n", sAppId);
    sendCmd("SUBSCRIBE", "{}", "ACTIVITY_JOIN");
    sendCmd("SUBSCRIBE", "{}", "ACTIVITY_JOIN_REQUEST");
    if (!registered)
    {
        registered = 1;
        registerLauncher();
    }
    sActivityDirty = 1;     /* the last activity again, on a new connection */
}

static void onJoinRequest(const char *json)
{
    const char *u = strstr(json, "\"user\"");
    char id[32], name[40];
    int i;

    if (u == NULL || jsonString(u, "id", id, sizeof(id)) == NULL)
    {
        return;
    }
    if (jsonString(u, "global_name", name, sizeof(name)) == NULL && jsonString(u, "username", name, sizeof(name)) == NULL)
    {
        snprintf(name, sizeof(name), "?");
    }
    for (i = 0; i < sNumAsk; i++)
    {
        if (strcmp(sAsk[i].id, id) == 0)
        {
            return;     /* asked again */
        }
    }
    if (sNumAsk == (int) (sizeof(sAsk) / sizeof(sAsk[0])))
    {
        coopDiscordRespond(0, 0);   /* the oldest makes room */
    }
    snprintf(sAsk[sNumAsk].id, sizeof(sAsk[sNumAsk].id), "%s", id);
    snprintf(sAsk[sNumAsk].name, sizeof(sAsk[sNumAsk].name), "%s", name);
    sAsk[sNumAsk].at = nowMs();
    sNumAsk++;
    portConLogf("ge: discord: %s asks to join\n", name);
}

static void onFrame(int op, char *json)
{
    char evt[40], buf[128];

    if (op == OP_PING)
    {
        sendFrame(OP_PONG, json);
        return;
    }
    if (op == OP_CLOSE)
    {
        if (sReady || sRefused == 0)
        {   /* a refusal (a bad id) only once: it comes back on every retry */
            portConLogf("ge: discord: closed: %s\n", jsonString(json, "message", buf, sizeof(buf)) ? buf : "?");
        }
        closeIpc();
        return;
    }
    if (op != OP_FRAME || jsonString(json, "evt", evt, sizeof(evt)) == NULL)
    {
        return;
    }
    if (strcmp(evt, "READY") == 0)
    {
        onReady();
    }
    else if (strcmp(evt, "ERROR") == 0)
    {
        portConLogf("ge: discord: error: %s\n", jsonString(json, "message", buf, sizeof(buf)) ? buf : "?");
    }
    else if (strcmp(evt, "ACTIVITY_JOIN") == 0)
    {
        if (jsonString(json, "secret", sJoinSecret, sizeof(sJoinSecret)) != NULL)
        {
            portConLogf("ge: discord: join from Discord\n");
        }
    }
    else if (strcmp(evt, "ACTIVITY_JOIN_REQUEST") == 0)
    {
        onJoinRequest(json);
    }
}

/* whole frames out of sIn */
static void parseFrames(void)
{
    while (isOpen() && sInLen >= 8)
    {
        unsigned int hdr[2];
        int len;

        memcpy(hdr, sIn, 8);
        len = (int) hdr[1];
        if (len < 0 || len > DISCORD_BUF - 9)
        {
            portConLogf("ge: discord: bad frame (%d bytes)\n", len);
            closeIpc();
            return;
        }
        if (sInLen < 8 + len)
        {
            return;
        }
        {
            char json[DISCORD_BUF];

            memcpy(json, sIn + 8, (size_t) len);
            json[len] = '\0';
            memmove(sIn, sIn + 8 + len, (size_t) (sInLen - 8 - len));
            sInLen -= 8 + len;
            onFrame((int) hdr[0], json);
        }
    }
}

/* ---- the game's side ---------------------------------------------------------- */

void coopDiscordTick(void)
{
    if (!isOpen())
    {
        tryConnect();
        return;
    }
    if (readSome())
    {
        parseFrames();
    }
    while (sNumAsk > 0 && nowMs() - sAsk[0].at > DISCORD_ASK_MS)
    {   /* Discord drops a request after about 30 s; so do we, unanswered */
        memmove(&sAsk[0], &sAsk[1], sizeof(sAsk[0]) * (size_t) (sNumAsk - 1));
        sNumAsk--;
    }
    if (sReady && sActivityDirty && nowMs() - sActivitySent >= DISCORD_ACTIVITY_MS)
    {   /* the latest one: changes in between are dropped */
        sActivityDirty = 0;
        sActivitySent = nowMs();
        sendCmd("SET_ACTIVITY", sLastActivity[0] ? sLastActivity : "{}", NULL);
    }
}

int coopDiscordConnected(void)
{
    return sReady;
}

/* The activity: details and state lines, when it started (unix seconds, 0
 * none), the party (id "" for none) of size/max, and the join secret ("" for
 * no Join button). Sent when it changes. */
void coopDiscordSetActivity(const char *details, const char *state, long long start,
                            const char *party, int size, int max, const char *secret)
{
    char args[2048], d[200], s[200], p[100], j[160];
    int n;

    jsonQuote(d, sizeof(d), details);
    jsonQuote(s, sizeof(s), state);
    n = snprintf(args, sizeof(args), "{\"pid\":%d,\"activity\":{\"details\":%s,\"state\":%s,"
                 "\"assets\":{\"large_image\":\"logo\",\"large_text\":\"GoldenEye 007 co-op\"}",
                 GETPID(), d, s);
    if (start > 0)
    {
        n += snprintf(args + n, sizeof(args) - n, ",\"timestamps\":{\"start\":%lld}", start);
    }
    if (party != NULL && party[0] != '\0' && size > 0 && max >= size)
    {
        jsonQuote(p, sizeof(p), party);
        n += snprintf(args + n, sizeof(args) - n, ",\"party\":{\"id\":%s,\"size\":[%d,%d]}", p, size, max);
        if (secret != NULL && secret[0] != '\0')
        {
            jsonQuote(j, sizeof(j), secret);
            n += snprintf(args + n, sizeof(args) - n, ",\"secrets\":{\"join\":%s}", j);
        }
    }
    snprintf(args + n, sizeof(args) - n, ",\"instance\":false}}");
    if (strcmp(args, sLastActivity) != 0)
    {
        snprintf(sLastActivity, sizeof(sLastActivity), "%s", args);
        sActivityDirty = 1;
    }
}

/* the secret of a Join clicked in Discord, once ("" none) */
const char *coopDiscordTakeJoin(void)
{
    static char out[sizeof(sJoinSecret)];

    snprintf(out, sizeof(out), "%s", sJoinSecret);
    sJoinSecret[0] = '\0';
    return out;
}

int coopDiscordAskCount(void)
{
    return sNumAsk;
}

const char *coopDiscordAskName(int i)
{
    return (i >= 0 && i < sNumAsk) ? sAsk[i].name : "";
}

/* the host's answer to the i-th Ask to Join */
void coopDiscordRespond(int i, int accept)
{
    char args[96];

    if (i < 0 || i >= sNumAsk)
    {
        return;
    }
    snprintf(args, sizeof(args), "{\"user_id\":\"%s\"}", sAsk[i].id);
    sendCmd(accept ? "SEND_ACTIVITY_JOIN_INVITE" : "CLOSE_ACTIVITY_REQUEST", args, NULL);
    portConLogf("ge: discord: %s %s\n", accept ? "let in" : "turned down", sAsk[i].name);
    memmove(&sAsk[i], &sAsk[i + 1], sizeof(sAsk[0]) * (size_t) (sNumAsk - i - 1));
    sNumAsk--;
}
