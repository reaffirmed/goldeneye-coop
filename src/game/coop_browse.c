/**
 * Story co-op: public games and the lobby password, for the lobby screen.
 *
 * A host picks Friends Only (as before) or Public before it hosts, or while
 * it hosts, and can set a password. Public lobbies show on everyone's lobby
 * screen under Public Games (the host's name, players, and a lock when it
 * has a password); Join on a locked one asks for the password first. The
 * host's Steam friends never need it: it keeps strangers out of a public
 * game. The check itself is the transport's (port/src/coop_steam.c): the
 * host only takes a stranger as a player once their password matches, so
 * nothing in the game's own packets changes.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <bondtypes.h>
#include "coop.h"
#include "coop_net.h"

extern void portConLogf(const char *fmt, ...);

#ifdef GE_VIDEO
extern void portTextInputStart(void);
extern void portTextInputStop(void);
extern void portTextInputPoll(char *chr, s32 *key);
extern s32 portTextInputActive(void);
#else
#define portTextInputStart()
#define portTextInputStop()
#define portTextInputPoll(c, k) do { if (c) *(c) = 0; if (k) *(k) = 0; } while (0)
#define portTextInputActive() 0
#endif
#define PORTTEXT_BACKSPACE 1
#define PORTTEXT_ACCEPT    2
#define PORTTEXT_CANCEL    3

#define COOPBROWSE_PW       23          /* characters */
#define COOPBROWSE_REFRESH  (60 * 5)    /* the list, while the screen is idle */
#define COOPBROWSE_REFUSED  (60 * 5)    /* "Wrong password" stays this long */

static s32 sPublic;
static char sPassword[COOPBROWSE_PW + 1];
static s32 sDirty = 1;          /* the options have to go to Steam */
static s32 sTyping;             /* COOPBROWSE_TYPE_* */
static s32 sTypingFor;          /* the public game a join password is for */
static char sLine[COOPBROWSE_PW + 1];
static s32 sLineLen;
static u32 sNow;
static u32 sLastRefresh;
static s32 sRefused;            /* ticks left on "Wrong password" */
static s32 sRefusedWhy;         /* coopSteamJoinRefused's answer: 1 password, 2 kept out */

/* ---- the host's options ---------------------------------------------------- */

s32 coopBrowsePublic(void)
{
    return sPublic;
}

void coopBrowseTogglePublic(void)
{
    sPublic = !sPublic;
    sDirty = 1;
    portConLogf("ge: coop lobby: %s\n", sPublic ? "public" : "friends only");
}

s32 coopBrowseHasPassword(void)
{
    return sPassword[0] != '\0';
}

static void coopBrowseSetPassword(const char *pw)
{
    snprintf(sPassword, sizeof(sPassword), "%s", pw);
    sDirty = 1;
    portConLogf("ge: coop lobby: password %s\n", sPassword[0] != '\0' ? "set" : "cleared");
}

/* ---- the public games ---------------------------------------------------------- */

s32 coopBrowseCount(void)
{
    return coopLobbyIsSteam() ? coopSteamBrowseCount() : 0;
}

const char *coopBrowseName(s32 i)
{
    return coopSteamBrowseName(i);
}

s32 coopBrowseMembers(s32 i)
{
    return coopSteamBrowseMembers(i);
}

s32 coopBrowseLocked(s32 i)
{
    return coopSteamBrowseLocked(i);
}

static void coopBrowseJoinWith(s32 i, const char *pw)
{
    unsigned long long lobby = coopSteamBrowseLobby(i);

    if (lobby == 0)
    {
        return;
    }
    portConLogf("ge: coop lobby: joining %s's public game%s\n", coopSteamBrowseName(i), pw[0] ? " with a password" : "");
    coopSteamSetJoinPassword(pw);
    coopLobbyJoinLobbyId(lobby);
}

/* Join on a public game: straight in, or the password first */
void coopBrowseJoin(s32 i)
{
    if (coopBrowseLocked(i))
    {
        coopBrowseTypeStart(COOPBROWSE_TYPE_JOIN, i);
        return;
    }
    coopBrowseJoinWith(i, "");
}

/* 1: in a host's passworded lobby, waiting for it to check ours */
s32 coopBrowseChecking(void)
{
    return coopLobbyIsSteam() && coopSteamAdmitWaiting();
}

/* ticks left to show "Wrong password" */
s32 coopBrowseRefused(void)
{
    return sRefused;
}

/* what to say while coopBrowseRefused: the password, or a lobby Steam kept us out of */
const char *coopBrowseRefusedText(void)
{
    return (sRefusedWhy == 2) ? "Couldn't join that game.\n" : "Wrong password.\n";
}

/* ---- typing a password ---------------------------------------------------------- */

void coopBrowseTypeStart(s32 kind, s32 forgame)
{
    if (sTyping || coopChatTyping() || portTextInputActive())
    {
        return;
    }
    sTyping = kind;
    sTypingFor = forgame;
    sLineLen = 0;
    sLine[0] = '\0';
    portTextInputStart();
}

static void coopBrowseTypeStop(void)
{
    if (sTyping)
    {
        portTextInputStop();
    }
    sTyping = 0;
}

s32 coopBrowseTyping(void)
{
    return sTyping;
}

s32 coopBrowseTypingFor(void)
{
    return sTypingFor;
}

/* what has been typed, as stars */
void coopBrowseTypedMask(char *out, s32 cap)
{
    s32 i;

    for (i = 0; i < sLineLen && i < cap - 1; i++)
    {
        out[i] = '*';
    }
    out[i] = '\0';
}

static void coopBrowseTypeTick(void)
{
    s32 guard;

    for (guard = 0; guard < 64 && sTyping; guard++)
    {
        char c = 0;
        s32 key = 0;

        portTextInputPoll(&c, &key);
        if (c == 0 && key == 0)
        {
            break;
        }
        if (c > 0x20 && c < 0x7F)
        {
            if (sLineLen < COOPBROWSE_PW)
            {
                sLine[sLineLen++] = c;
                sLine[sLineLen] = '\0';
            }
        }
        else if (key == PORTTEXT_BACKSPACE)
        {
            if (sLineLen > 0)
            {
                sLine[--sLineLen] = '\0';
            }
        }
        else if (key == PORTTEXT_ACCEPT)
        {
            s32 kind = sTyping;

            coopBrowseTypeStop();
            if (kind == COOPBROWSE_TYPE_HOST)
            {
                coopBrowseSetPassword(sLine);   /* empty clears it */
            }
            else if (sLineLen > 0)
            {
                coopBrowseJoinWith(sTypingFor, sLine);
            }
        }
        else if (key == PORTTEXT_CANCEL)
        {
            coopBrowseTypeStop();
        }
    }
}

/* ---- the harness ------------------------------------------------------------------ */

/* GE_COOP_PUBLIC=1: host a public game; GE_COOP_PASSWORD=<pw>: with that
 * password (host), or join the first public game with it (GE_COOP_LOBBY=browse) */
static void coopBrowseTestInit(void)
{
    static s32 done;
    const char *e;

    if (done)
    {
        return;
    }
    done = 1;
    e = getenv("GE_COOP_PUBLIC");
    if (e != NULL && atoi(e) != 0)
    {
        sPublic = 1;
    }
    e = getenv("GE_COOP_PASSWORD");
    if (e != NULL && getenv("GE_COOP_PUBLIC") != NULL)
    {
        snprintf(sPassword, sizeof(sPassword), "%s", e);
    }
}

/* GE_COOP_LOBBY=browse: join the first public game listed */
s32 coopBrowseHarnessJoin(void)
{
    const char *pw = getenv("GE_COOP_PASSWORD");

    if (coopBrowseCount() <= 0)
    {
        return 0;
    }
    coopBrowseJoinWith(0, (pw != NULL && coopBrowseLocked(0)) ? pw : "");
    return 1;
}

/* ---- each tick, menus and missions -------------------------------------------- */

void coopBrowseTick(s32 inmission)
{
    sNow++;
    coopBrowseTestInit();
    if (sRefused > 0)
    {
        sRefused--;
    }
    if (!coopLobbyIsSteam())
    {
        coopBrowseTypeStop();
        return;
    }
    if (sDirty)
    {
        sDirty = 0;
        coopSteamSetLobbyOptions(sPublic, sPassword, coopExtVersion());
    }
    {
        const s32 why = coopSteamJoinRefused();

        if (why)
        {
            sRefused = (why == 2) ? 2 * COOPBROWSE_REFUSED : COOPBROWSE_REFUSED;
            sRefusedWhy = why;
        }
    }
    if (inmission)
    {
        coopBrowseTypeStop();
        return;
    }
    coopBrowseTypeTick();
    if (coopLobbyState() == COOPLOBBY_IDLE && coopLobbyOnScreen()
        && (sLastRefresh == 0 || sNow - sLastRefresh >= COOPBROWSE_REFRESH))
    {
        sLastRefresh = sNow;
        coopSteamBrowseRefresh();
    }
    if (coopLobbyState() != COOPLOBBY_IDLE && sTyping == COOPBROWSE_TYPE_JOIN)
    {
        coopBrowseTypeStop();
    }
}
