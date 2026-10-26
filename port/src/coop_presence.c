/**
 * Rich presence and outside joins, for Steam and Discord at once.
 *
 * The game (src/game/coop_presence.c) says each frame what it is doing; this
 * pushes it to Steam's rich presence (coopSteamSetPresence) and Discord's
 * activity (coop_discord.c) when it changes, and keeps one join waiting for
 * the game to take: an accepted Steam invite, Join Game in the friends list,
 * "+connect_lobby" on the command line, or Join / Ask to Join in Discord.
 *
 * Discord's join secret is "ge1:<lobby id>:<password hash in hex, or 0>". The
 * hash is the one the host's game checks (coop_steam.c pwHash: the password
 * with the lobby id), so a player the host let in through Discord gets past
 * the password without being told it, and only for that lobby.
 *
 * Without Steam (the UDP test transport) or without Discord each side just
 * stays quiet. GE_NO_PRESENCE turns both off.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "coop_net.h"

/* the repo's include/string.h is the N64 stub (memcpy, strchr, strlen) */
extern void *memset(void *, int, size_t);
extern void *memmove(void *, const void *, size_t);
extern int strcmp(const char *, const char *);
extern int strncmp(const char *, const char *, size_t);
extern char *strstr(const char *, const char *);

extern void portConLogf(const char *fmt, ...);

static struct
{
    int set;
    unsigned long long lobby;
    int haspw;
    unsigned int hash;
    char from[40];
} sJoin;

static char sDiscordKey[256];   /* the activity's first line and phase: a new one restarts the clock */
static long long sDiscordStart;

void coopPresenceQueueJoin(unsigned long long lobby, int haspw, unsigned int pwhash, const char *from)
{
    if (lobby == 0)
    {
        return;
    }
    sJoin.set = 1;
    sJoin.lobby = lobby;
    sJoin.haspw = haspw;
    sJoin.hash = pwhash;
    snprintf(sJoin.from, sizeof(sJoin.from), "%s", (from != NULL && from[0] != '\0') ? from : "a friend");
    portConLogf("ge: presence: join lobby %llu (from %s)\n", lobby, sJoin.from);
}

int coopPresenceTakeJoin(unsigned long long *lobby, int *haspw, unsigned int *pwhash, char *from, int cap)
{
    if (!sJoin.set)
    {
        return 0;
    }
    sJoin.set = 0;
    *lobby = sJoin.lobby;
    *haspw = sJoin.haspw;
    *pwhash = sJoin.hash;
    if (from != NULL && cap > 0)
    {
        snprintf(from, (size_t) cap, "%s", sJoin.from);
    }
    return 1;
}

int coopPresencePeekJoin(char *from, int cap)
{
    if (sJoin.set && from != NULL && cap > 0)
    {
        snprintf(from, (size_t) cap, "%s", sJoin.from);
    }
    return sJoin.set;
}

/* "ge1:<lobby>:<hash>" from Discord */
static void takeDiscordJoin(void)
{
    const char *s = coopDiscordTakeJoin();
    unsigned long long lobby;
    unsigned int hash;
    char *end;

    if (s[0] == '\0')
    {
        return;
    }
    if (strncmp(s, "ge1:", 4) != 0)
    {
        portConLogf("ge: discord: a join secret that isn't ours: %s\n", s);
        return;
    }
    lobby = strtoull(s + 4, &end, 10);
    hash = (*end == ':') ? (unsigned int) strtoul(end + 1, NULL, 16) : 0;
    coopPresenceQueueJoin(lobby, hash != 0, hash, "Discord");
}

static void pushDiscord(const CoopPresence *p)
{
    char key[sizeof(sDiscordKey)], party[40], secret[64];
    unsigned int hash = 0;
    int haspw;

    snprintf(key, sizeof(key), "%d|%s", p->phase, p->details);
    if (strcmp(key, sDiscordKey) != 0)
    {
        snprintf(sDiscordKey, sizeof(sDiscordKey), "%s", key);
        sDiscordStart = (long long) time(NULL);
    }
    party[0] = '\0';
    secret[0] = '\0';
    if (p->lobby != 0)
    {
        snprintf(party, sizeof(party), "ge-%llu", p->lobby);
        /* the host can hand out its password's hash; a joiner in a lobby
         * with a password can't, so it offers no Join */
        haspw = coopSteamPasswordHash(&hash);
        if (p->joinable && (haspw || !coopSteamLobbyHasPassword()))
        {
            snprintf(secret, sizeof(secret), "ge1:%llu:%x", p->lobby, haspw ? hash : 0u);
        }
    }
    coopDiscordSetActivity(p->details, p->state, sDiscordStart, party, p->players, p->maxplayers, secret);
    if (secret[0] == '\0')
    {
        while (coopDiscordAskCount() > 0)
        {
            coopDiscordRespond(0, 0);   /* nothing to let them into */
        }
    }
}

void coopPresenceTick(const CoopPresence *p)
{
    static int off = -1;

    if (off < 0)
    {
        off = getenv("GE_NO_PRESENCE") != NULL;
    }
    if (off)
    {
        return;
    }
    coopSteamPresenceOpen();
    coopSteamPresenceTick();
    coopDiscordTick();
    takeDiscordJoin();
    if (p == NULL)
    {
        return;
    }
    coopSteamSetPresence(p->status, p->phase, p->lobby, p->players, p->joinable);
    pushDiscord(p);
}
