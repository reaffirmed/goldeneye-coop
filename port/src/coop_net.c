/**
 * Co-op transport.
 *
 * GE_COOP=steam              host over Steam (lobby + invite, see coop_steam.c)
 * GE_COOP=steam:join[:<id>]  join a friend's Steam lobby (found by itself
 *                            when no id is given)
 * steam:idle                 Steam up for the lobby screen's friend list,
 *                            no lobby yet (the screen hosts or joins)
 * GE_COOP=host:<port>        plain UDP: listen on <port>; the first three
 *                            addresses to write take the peer slots
 * GE_COOP=join:<ip>:<port>   plain UDP: send to the host at <ip>:<port>
 *
 * Non-blocking, up to three peers (see coop_net.h). Packets are
 * game-defined (see src/game/coop.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define COOP_CLOSE closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#define COOP_CLOSE close
#define INVALID_SOCKET (-1)
typedef int SOCKET;
#endif

#include "coop_net.h"

extern void portConLogf(const char *fmt, ...);

static SOCKET sSock = INVALID_SOCKET;
static int sSteam; /* the Steam backend is in use */
static int sIsHost;
static int sHasPeer[COOP_NET_MAX_PEERS];
static struct sockaddr_in sPeer[COOP_NET_MAX_PEERS];

int coopNetInit(void)
{
    return coopNetStart(getenv("GE_COOP"));
}

/* the lobby screen's host and join land here; any open session is closed first */
int coopNetStart(const char *e)
{
    struct sockaddr_in bindaddr;
    int port;

    coopNetShutdown();
    if (e == NULL || e[0] == '\0')
    {
        return 0;
    }
    if (strncmp(e, "steam", 5) == 0)
    {
        if (!coopSteamInit(e))
        {
            return 0;
        }
        sSteam = 1;
        return 1;
    }

#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            portConLogf("ge: coop: WSAStartup failed\n");
            return 0;
        }
    }
#endif

    memset(&bindaddr, 0, sizeof(bindaddr));
    memset(sPeer, 0, sizeof(sPeer));
    bindaddr.sin_family = AF_INET;
    sPeer[0].sin_family = AF_INET;

    if (strncmp(e, "host:", 5) == 0)
    {
        port = atoi(e + 5);
        sIsHost = 1;
        bindaddr.sin_addr.s_addr = htonl(INADDR_ANY);
        bindaddr.sin_port = htons((unsigned short) port);
    }
    else if (strncmp(e, "join:", 5) == 0)
    {
        char ip[64];
        const char *colon = strrchr(e + 5, ':');
        size_t n;

        if (colon == NULL)
        {
            portConLogf("ge: coop: GE_COOP=join:<ip>:<port> expected, got %s\n", e);
            return 0;
        }
        n = (size_t) (colon - (e + 5));
        if (n >= sizeof(ip))
        {
            n = sizeof(ip) - 1;
        }
        memcpy(ip, e + 5, n);
        ip[n] = '\0';
        port = atoi(colon + 1);
        sIsHost = 0;
        sHasPeer[0] = 1;
        sPeer[0].sin_addr.s_addr = inet_addr(ip);
        sPeer[0].sin_port = htons((unsigned short) port);
        bindaddr.sin_addr.s_addr = htonl(INADDR_ANY);
        bindaddr.sin_port = 0;
    }
    else
    {
        portConLogf("ge: coop: GE_COOP=%s not recognized (host:<port> or join:<ip>:<port>)\n", e);
        return 0;
    }

    sSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sSock == INVALID_SOCKET)
    {
        portConLogf("ge: coop: socket failed\n");
        return 0;
    }
    if (bind(sSock, (struct sockaddr *) &bindaddr, sizeof(bindaddr)) != 0)
    {
        portConLogf("ge: coop: bind failed\n");
        COOP_CLOSE(sSock);
        sSock = INVALID_SOCKET;
        return 0;
    }
#ifdef _WIN32
    {
        u_long nb = 1;
        ioctlsocket(sSock, FIONBIO, &nb);
    }
#else
    fcntl(sSock, F_SETFL, fcntl(sSock, F_GETFL, 0) | O_NONBLOCK);
#endif

    portConLogf("ge: coop: %s on udp %d\n", sIsHost ? "hosting" : "joining", port);
    return 1;
}

int coopNetIsHost(void)
{
    return sSteam ? coopSteamIsHost() : sIsHost;
}

int coopNetPeerConnected(int peer)
{
    if (sSteam)
    {
        return coopSteamPeerConnected(peer);
    }
    return peer >= 0 && peer < COOP_NET_MAX_PEERS && sHasPeer[peer];
}

int coopNetHasPeer(void)
{
    int i;

    for (i = 0; i < COOP_NET_MAX_PEERS; i++)
    {
        if (coopNetPeerConnected(i))
        {
            return 1;
        }
    }
    return 0;
}

void coopNetDropPeer(int peer)
{
    if (sSteam)
    {
        coopSteamDropPeer(peer);
    }
    else if (sIsHost && peer >= 0 && peer < COOP_NET_MAX_PEERS && sHasPeer[peer])
    {   /* a joiner keeps the host: it has nobody else to talk to */
        portConLogf("ge: coop: peer %d dropped\n", peer);
        sHasPeer[peer] = 0;
    }
}

const char *coopNetPeerName(int peer)
{
    static char name[COOP_NET_MAX_PEERS][24];

    if (sSteam)
    {
        return coopSteamPeerNameAt(peer);
    }
    if (!coopNetPeerConnected(peer))
    {
        return "";
    }
    snprintf(name[peer], sizeof(name[peer]), "%s:%d", inet_ntoa(sPeer[peer].sin_addr), ntohs(sPeer[peer].sin_port));
    return name[peer];
}

unsigned long long coopNetPeerId(int peer)
{
    if (sSteam)
    {
        return coopSteamPeerIdAt(peer);
    }
    if (!coopNetPeerConnected(peer))
    {
        return 0;
    }
    return ((unsigned long long) ntohl(sPeer[peer].sin_addr.s_addr) << 16) | ntohs(sPeer[peer].sin_port);
}

void coopNetSendTo(int peer, const void *buf, int len)
{
    if (sSteam)
    {
        coopSteamSendTo(peer, buf, len);
        return;
    }
    if (sSock == INVALID_SOCKET || !coopNetPeerConnected(peer))
    {
        return;
    }
    sendto(sSock, (const char *) buf, len, 0, (struct sockaddr *) &sPeer[peer], sizeof(sPeer[peer]));
}

void coopNetSend(const void *buf, int len)
{
    int i;

    for (i = 0; i < COOP_NET_MAX_PEERS; i++)
    {
        coopNetSendTo(i, buf, len);
    }
}

int coopNetRecv(void *buf, int cap)
{
    return coopNetRecvFrom(buf, cap, NULL);
}

int coopNetRecvFrom(void *buf, int cap, int *peer)
{
    struct sockaddr_in from;
    socklen_t fromlen;
    int n, i, slot;

    if (sSteam)
    {
        return coopSteamRecvFrom(buf, cap, peer);
    }
    if (sSock == INVALID_SOCKET)
    {
        return 0;
    }
    for (;;)
    {
        fromlen = sizeof(from);
        n = (int) recvfrom(sSock, (char *) buf, cap, 0, (struct sockaddr *) &from, &fromlen);
        if (n <= 0)
        {
            return 0;
        }
        slot = -1;
        for (i = 0; i < COOP_NET_MAX_PEERS; i++)
        {
            if (sHasPeer[i] && sPeer[i].sin_addr.s_addr == from.sin_addr.s_addr && sPeer[i].sin_port == from.sin_port)
            {
                slot = i;
                break;
            }
        }
        if (slot < 0 && sIsHost)
        {
            for (i = 0; i < COOP_NET_MAX_PEERS && slot < 0; i++)
            {
                if (!sHasPeer[i])
                {
                    slot = i;
                    sPeer[i] = from;
                    sHasPeer[i] = 1;
                    portConLogf("ge: coop: peer %s:%d connected (slot %d)\n", inet_ntoa(from.sin_addr), ntohs(from.sin_port), i);
                }
            }
        }
        if (slot < 0)
        {
            continue;   /* a stranger, or the host is full */
        }
        if (peer != NULL)
        {
            *peer = slot;
        }
        return n;
    }
}

void coopNetShutdown(void)
{
    if (sSteam)
    {
        coopSteamShutdown();
        sSteam = 0;
    }
    if (sSock != INVALID_SOCKET)
    {
        COOP_CLOSE(sSock);
        sSock = INVALID_SOCKET;
    }
    memset(sHasPeer, 0, sizeof(sHasPeer));
    sIsHost = 0;
}

/* see coop_net.h; a UDP peer id is its address << 16 | its port */
void coopNetMigrate(int ashost, const unsigned long long *ids, int n)
{
    int i;

    if (sSteam)
    {
        coopSteamMigrate(ashost, ids, n);
        return;
    }
    memset(sHasPeer, 0, sizeof(sHasPeer));
    sIsHost = ashost;
    for (i = 0; i < n && i < COOP_NET_MAX_PEERS && (ashost || i == 0); i++)
    {
        if (ids[i] == 0)
        {
            continue;
        }
        memset(&sPeer[i], 0, sizeof(sPeer[i]));
        sPeer[i].sin_family = AF_INET;
        sPeer[i].sin_addr.s_addr = htonl((unsigned int) (ids[i] >> 16));
        sPeer[i].sin_port = htons((unsigned short) (ids[i] & 0xFFFF));
        sHasPeer[i] = 1;
        portConLogf("ge: coop: %s %s:%d (slot %d)\n", ashost ? "keeping a slot for" : "new host at",
                    inet_ntoa(sPeer[i].sin_addr), ntohs(sPeer[i].sin_port), i);
    }
}

int coopNetHostLeft(void)
{
    return sSteam ? coopSteamHostLeft() : 0;
}

int coopNetIsSteam(void)
{
    return sSteam;
}

int coopNetIsOpen(void)
{
    return sSteam || sSock != INVALID_SOCKET;
}
