/**
 * Co-op transport.
 *
 * GE_COOP=steam              host over Steam (lobby + invite, see coop_steam.c)
 * GE_COOP=steam:join[:<id>]  join a friend's Steam lobby (found by itself
 *                            when no id is given)
 * GE_COOP=host:<port>        plain UDP: listen on <port>, reply to whoever writes first
 * GE_COOP=join:<ip>:<port>   plain UDP: send to the host at <ip>:<port>
 *
 * Non-blocking, one peer. Packets are game-defined (see src/game/coop.c).
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
static int sHasPeer;
static struct sockaddr_in sPeer;

int coopNetInit(void)
{
    const char *e = getenv("GE_COOP");
    struct sockaddr_in bindaddr;
    int port;

    if (sSock != INVALID_SOCKET)
    {
        return 1;
    }
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
    memset(&sPeer, 0, sizeof(sPeer));
    bindaddr.sin_family = AF_INET;
    sPeer.sin_family = AF_INET;

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
        sHasPeer = 1;
        sPeer.sin_addr.s_addr = inet_addr(ip);
        sPeer.sin_port = htons((unsigned short) port);
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

int coopNetHasPeer(void)
{
    return sSteam ? coopSteamHasPeer() : sHasPeer;
}

void coopNetSend(const void *buf, int len)
{
    if (sSteam)
    {
        coopSteamSend(buf, len);
        return;
    }
    if (sSock == INVALID_SOCKET || !sHasPeer)
    {
        return;
    }
    sendto(sSock, (const char *) buf, len, 0, (struct sockaddr *) &sPeer, sizeof(sPeer));
}

int coopNetRecv(void *buf, int cap)
{
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int n;

    if (sSteam)
    {
        return coopSteamRecv(buf, cap);
    }
    if (sSock == INVALID_SOCKET)
    {
        return 0;
    }
    n = (int) recvfrom(sSock, (char *) buf, cap, 0, (struct sockaddr *) &from, &fromlen);
    if (n <= 0)
    {
        return 0;
    }
    if (sIsHost && !sHasPeer)
    {
        sPeer = from;
        sHasPeer = 1;
        portConLogf("ge: coop: peer %s:%d connected\n", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
    }
    return n;
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
    sHasPeer = 0;
}
