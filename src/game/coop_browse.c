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
#include <boss.h>
#include "coop.h"
#include "coop_net.h"
#include "port_update.h"

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
static s32 sKeys;               /* COOPKEYS_*: a pad player's keyboard (coop_chat.c) */
static s32 sKeysSkip;           /* the press that opened the grid must not type in it */
static s32 sTypingFor;          /* the public game a join password is for */
static char sLine[COOPBROWSE_PW + 1];
static s32 sLineLen;
static u32 sNow;
static u32 sLastRefresh;
static s32 sRefused;            /* ticks left on "Wrong password" */
static s32 sRefusedWhy;         /* coopSteamJoinRefused's answer: 1 password, 2 kept out, 3 another version */

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

/* the public game is in a mission: joining drops in (coop.c coopDropInTick) */
s32 coopBrowseInMission(s32 i)
{
    return coopSteamBrowseInMission(i);
}

/* a versus game: in a match it can't be joined (no late joining) */
s32 coopBrowseVersus(s32 i)
{
    return coopSteamBrowseVersus(i);
}

/* what the public game plays ("Versus: Temple, Normal", "Co-op: Dam"), "" from an older host */
const char *coopBrowseWhat(s32 i)
{
    return coopSteamBrowseWhat(i);
}

/* host: what we play, for the others' Public Games */
static void coopBrowseSetWhat(s32 inmission)
{
    const s32 versus = (coopSetting(COOPSET_VSMODE) & 1) != 0;
    char what[40];

    if (inmission && coopVersus())
    {
        char arena[32], scenario[32];

        coopVsPresenceNames(bossGetStageNum(), arena, sizeof(arena), scenario, sizeof(scenario));
        snprintf(what, sizeof(what), "Versus: %s, %s", arena, scenario);
    }
    else if (inmission && coopMissionName(bossGetStageNum()) != NULL)
    {
        snprintf(what, sizeof(what), "Co-op: %s", coopMissionName(bossGetStageNum()));
    }
    else
    {
        snprintf(what, sizeof(what), "%s lobby", versus ? "Versus" : "Co-op");
    }
    coopSteamSetWhat(what, versus);
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
    if (sRefusedWhy == 3)
    {
        return "Update needed to join.\n";
    }
    return (sRefusedWhy == 2) ? "Couldn't join that game.\n" : "Wrong password.\n";
}

/* the dim line under it, "" for none */
const char *coopBrowseRefusedHint(void)
{
    static char text[64];

    if (sRefusedWhy == 2)
    {   /* Steam kept us out: friends only (a Discord join from someone who
         * isn't the host's Steam friend), full, or gone */
        return "It's full, or friends only.\n";
    }
    if (sRefusedWhy == 3)
    {   /* builds are commits: 7 characters is enough to tell them apart */
        const char *theirs = coopSteamRefusedBuild();
        const char *rel = coopSteamRefusedRelease();

        if (rel[0] != '\0' && portUpdateOwn()[0] != '\0' && portUpdCompareVersions(rel, portUpdateOwn()) == -1)
        {   /* we are the newer one: nothing for us to get */
            snprintf(text, sizeof(text), "Host has %.12s and needs to update.\n", rel);
        }
        else if (rel[0] != '\0')
        {
            snprintf(text, sizeof(text), "Host has %.12s, you %.12s\n", rel, portUpdateOwn()[0] != '\0' ? portUpdateOwn() : coopSteamOwnBuild());
        }
        else if (theirs[0] != '\0')
        {
            snprintf(text, sizeof(text), "Host %.7s, you %.7s\n", theirs, coopSteamOwnBuild());
        }
        else
        {
            snprintf(text, sizeof(text), "Host is on another build.\n");
        }
        return text;
    }
    return "";
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
    sKeys = coopPadKeysOpen("PASSWORD", COOPBROWSE_PW);
    sKeysSkip = 1;
    if (sKeys != COOPKEYS_GRID)
    {
        portTextInputStart();
    }
}

static void coopBrowseTypeStop(void)
{
    if (sTyping)
    {
        if (sKeys != COOPKEYS_GRID)
        {
            portTextInputStop();
        }
        coopPadKeysClose(sKeys);
    }
    sTyping = 0;
    sKeys = COOPKEYS_NONE;
}

/* the password is on the on-screen keyboard (front.c draws it over the lobby) */
s32 coopBrowseOnGrid(void)
{
    return sTyping && sKeys == COOPKEYS_GRID;
}

static void coopBrowseTyped(s32 kind)
{
    if (kind == COOPBROWSE_TYPE_HOST)
    {
        coopBrowseSetPassword(sLine);   /* empty clears it */
    }
    else if (sLineLen > 0)
    {
        coopBrowseJoinWith(sTypingFor, sLine);
    }
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

#ifdef GE_VIDEO
    if (sTyping && sKeys == COOPKEYS_GRID)
    {
        extern s32 portOskTextStep(char *out, s32 cap);
        s32 r, kind = sTyping;

        if (sKeysSkip)
        {
            sKeysSkip = 0;
            return;
        }
        r = portOskTextStep(sLine, sizeof(sLine));
        sLineLen = (s32) strlen(sLine);
        if (r != 0)
        {
            sKeys = COOPKEYS_NONE;  /* the grid has closed itself */
            coopBrowseTypeStop();
            if (r > 0)
            {
                coopBrowseTyped(kind);
            }
        }
        return;
    }
#endif
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
            coopBrowseTyped(kind);
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
    coopSteamSetInMission(inmission);   /* host: Public Games shows Drop In while we play */
    if ((sNow % 30) == 0)
    {
        coopBrowseSetWhat(inmission);
    }
    if (sDirty)
    {
        sDirty = 0;
        /* GE_COOP_FAKEVER=<n> (harness): claim another co-op version, to
         * see the "update needed" line */
        coopSteamSetLobbyOptions(sPublic, sPassword,
                                 getenv("GE_COOP_FAKEVER") != NULL ? atoi(getenv("GE_COOP_FAKEVER")) : coopExtVersion());
    }
    {
        const s32 why = coopSteamJoinRefused();

        if (why)
        {
            sRefused = (why >= 2) ? 2 * COOPBROWSE_REFUSED : COOPBROWSE_REFUSED;
            if (why == 3)
            {   /* another co-op version: the updater looks for a newer release
                 * (the CO-OP screen offers it), even with auto_update off */
                const char *rel = coopSteamRefusedRelease();

                if (rel[0] == '\0' || portUpdateNewer(rel))
                {
                    portUpdateCheck(1);
                }
            }
            sRefusedWhy = why;
            if (why == 2)
            {   /* a panel too (coop_screens.c): the rows' red line is easy to miss */
                coopScreenError("Couldn't Join", "Steam kept us out of that game.\nIt's full, or friends only.");
            }
            else if (why == 3)
            {   /* (it used to say Wrong Password here too) */
                const char *rel = coopSteamRefusedRelease();
                char text[96];

                if (rel[0] != '\0' && !portUpdateNewer(rel) && portUpdateOwn()[0] != '\0')
                {
                    snprintf(text, sizeof(text), "The host has %.12s and needs\nto update before you can join.", rel);
                }
                else if (rel[0] != '\0')
                {
                    snprintf(text, sizeof(text), "The host has %.12s. Pick\nUpdate on PLAY to get it,\nthen join again.", rel);
                }
                else
                {
                    snprintf(text, sizeof(text), "The host is on another version.\nOne of you needs to update.");
                }
                coopScreenError("Update Needed", text);
            }
            else
            {
                coopScreenError("Wrong Password", "The host's game has a password,\nand that wasn't it.");
            }
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
