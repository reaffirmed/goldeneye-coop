/**
 * Rich presence and outside joins: what friends see on Steam and Discord, and
 * what the game does with an invite they accept or a Join they click.
 *
 * Every frame, in the menus (front.c menu_init) and in stages (coopTick),
 * this sums up the game in a CoopPresence and hands it to
 * port/src/coop_presence.c, which sends Steam and Discord what changed:
 *
 *   menus          "In the menus"
 *   lobby screen   "In the co-op lobby (2/4)", "Looking for a co-op game"
 *   mission        "Co-op: Dam, 00 Agent (2/4)", "Solo: Dam, Agent"
 *   versus         "Versus: Temple, Licence to Kill (3/4)"
 *
 * Friends may join while the lobby has room, in the lobby and in co-op
 * missions (late join); not in a versus match, which takes nobody after it
 * starts.
 *
 * A join (Steam invite or Join Game, "+connect_lobby", Discord) waits until
 * the game is in the menus: then we leave any lobby we are in, join that
 * one and open the lobby screen. One that comes during a mission waits for
 * its end, and a toast says so.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <boss.h>
#include "coop.h"
#include "coop_net.h"
#include "front.h"
#include "player.h"

/* the repo's include/string.h is the N64 stub (memcpy, strchr, strlen) */
extern void *memset(void *, int, size_t);
extern void *memmove(void *, const void *, size_t);
extern int strcmp(const char *, const char *);
extern int strncmp(const char *, const char *, size_t);
extern char *strstr(const char *, const char *);

extern void portConLogf(const char *fmt, ...);

static const char *const sDiffNames[] = { "Agent", "Secret Agent", "00 Agent", "007" };

/* players in the session; a change counts once it has held for 2 s (a
 * joiner hears of the others through the host's hellos, which come and go
 * around their 3 s window in a mission) */
static s32 coopPresencePlayers(void)
{
    static s32 shown, held;
    s32 i, n = 0;

    if (coopEnabled())
    {
        for (i = 0; i < COOP_MAX_PLAYERS; i++)
        {
            n += coopLobbyPlayerHere(i);
        }
    }
    n = (n < 1) ? 1 : n;
    if (n == shown || shown == 0)
    {
        shown = n;
        held = 0;
    }
    else if (++held > 120)
    {
        shown = n;
        held = 0;
    }
    return shown;
}

static void coopPresenceBuild(CoopPresence *p, s32 instage, char *status, char *details, char *state, s32 cap)
{
    const s32 lobbystate = coopLobbyState();
    const s32 inlobby = coopLobbyIsSteam() && (lobbystate == COOPLOBBY_HOSTING || lobbystate == COOPLOBBY_JOINED);
    const s32 versus = coopSetting(COOPSET_VSMODE) & 1;
    const s32 players = coopPresencePlayers();

    memset(p, 0, sizeof(*p));
    p->status = status;
    p->details = details;
    p->state = state;
    p->lobby = inlobby ? coopSteamLobbyId() : 0;
    p->players = players;
    p->maxplayers = COOP_MAX_PLAYERS;
    p->joinable = p->lobby != 0 && players < COOP_MAX_PLAYERS;

    if (instage)
    {
        const s32 stage = bossGetStageNum();
        const char *mission = coopMissionName(stage);
        s32 diff = selected_difficulty;

        p->phase = COOP_PRESENCE_MISSION;
        if (diff < 0 || diff >= (s32) (sizeof(sDiffNames) / sizeof(sDiffNames[0])))
        {
            diff = 0;
        }
        if (coopVersus())
        {
            char arena[32], scenario[32];

            coopVsPresenceNames(stage, arena, sizeof(arena), scenario, sizeof(scenario));
            p->players = coopVsPlayers();
            p->joinable = 0;    /* no late joining a match */
            if (strcmp(scenario, "Normal") == 0)
            {   /* Rare's plain deathmatch */
                snprintf(status, cap, "Versus: %s (%d/%d)", arena, p->players, COOP_MAX_PLAYERS);
                snprintf(state, cap, "Free for all");
            }
            else
            {
                snprintf(status, cap, "Versus: %s, %s (%d/%d)", arena, scenario, p->players, COOP_MAX_PLAYERS);
                snprintf(state, cap, "%s", scenario);
            }
            snprintf(details, cap, "Versus: %s", arena);
        }
        else if (mission == NULL)
        {   /* split-screen multiplayer, a cutscene stage */
            p->lobby = 0;
            p->joinable = 0;
            snprintf(status, cap, "Playing");
            snprintf(details, cap, "Playing");
            snprintf(state, cap, "%s", (getPlayerCount() > 1) ? "Split-screen multiplayer" : "In a mission");
        }
        else if (p->lobby != 0 || coopSessionActive())
        {
            const s32 guards = coopSetting(COOPSET_DIFFICULTY);

            snprintf(status, cap, "Co-op: %s, %s (%d/%d)", mission, sDiffNames[diff], players, COOP_MAX_PLAYERS);
            snprintf(details, cap, "Co-op: %s (%s)", mission, sDiffNames[diff]);
            if (guards > 0)
            {
                snprintf(state, cap, "In a mission, %s guards", coopDifficultyName(guards));
            }
            else
            {
                snprintf(state, cap, "In a mission");
            }
        }
        else
        {
            snprintf(status, cap, "Solo: %s, %s", mission, sDiffNames[diff]);
            snprintf(details, cap, "Solo: %s (%s)", mission, sDiffNames[diff]);
            snprintf(state, cap, "In a mission");
        }
        return;
    }

    if (p->lobby != 0 || lobbystate == COOPLOBBY_HOSTING || lobbystate == COOPLOBBY_JOINED)
    {
        const char *mode = versus ? "versus" : "co-op";

        p->phase = COOP_PRESENCE_LOBBY;
        snprintf(status, cap, "In the %s lobby (%d/%d)", mode, players, COOP_MAX_PLAYERS);
        snprintf(details, cap, "%s lobby", versus ? "Versus" : "Co-op");
        if (!coopLobbyOnScreen())
        {
            snprintf(state, cap, "%s", versus ? "Setting up a match" : "Picking a mission");
        }
        else
        {
            snprintf(state, cap, "%s", (players < COOP_MAX_PLAYERS) ? "Waiting for players" : "Lobby full");
        }
        return;
    }
    if (coopLobbyOnScreen())
    {
        p->phase = COOP_PRESENCE_LOBBY;
        snprintf(status, cap, "Looking for a co-op game");
        snprintf(details, cap, "Co-op lobby");
        snprintf(state, cap, "Looking for a game");
        return;
    }
    p->phase = COOP_PRESENCE_MENU;
    snprintf(status, cap, "In the menus");
    snprintf(details, cap, "In the menus");
    snprintf(state, cap, "Main menu");
}

/* in the menus: the join waiting, if any, now */
static void coopPresenceJoinNow(void)
{
    static s32 ticks;
    unsigned long long lobby;
    unsigned int hash;
    s32 haspw, state;
    char from[40];

    /* boot menus settled (as the GE_MENU harness waits), and not on the way into a stage */
    if (++ticks < 120 || current_menu == MENU_RUN_STAGE || !coopPresencePeekJoin(NULL, 0))
    {
        return;
    }
    coopPresenceTakeJoin(&lobby, &haspw, &hash, from, sizeof(from));
    state = coopLobbyState();
    if (lobby != coopSteamLobbyId() || (state != COOPLOBBY_HOSTING && state != COOPLOBBY_JOINED))
    {
        if (state == COOPLOBBY_HOSTING || state == COOPLOBBY_JOINED)
        {
            coopLobbyLeave();   /* the others hear it; a host hands the lobby on */
        }
        if (haspw)
        {
            coopSteamSetJoinPasswordHash(hash);
        }
        else
        {
            coopSteamSetJoinPassword("");
        }
        portConLogf("ge: coop: joining lobby %llu (from %s)\n", lobby, from);
        coopLobbyJoinLobbyId(lobby);
    }
    if (current_menu != MENU_COOP_LOBBY)
    {
        selected_folder_num = (selected_folder_num >= FOLDER1) ? selected_folder_num : FOLDER1;
        frontChangeMenu(MENU_COOP_LOBBY, FALSE);
    }
}

/* in a stage: say once that the join waits for the mission to end */
static void coopPresenceJoinLater(void)
{
    static char told[40];
    char from[40];
    char text[96];

    if (!coopPresencePeekJoin(from, sizeof(from)))
    {
        told[0] = '\0';
        return;
    }
    if (strcmp(told, from) == 0)
    {
        return;
    }
    snprintf(told, sizeof(told), "%s", from);
    snprintf(text, sizeof(text), "JOINING %s'S GAME|AFTER THIS MISSION (ABORT TO GO NOW)", from);
    coopFeedToast(text, -1);
}

/* host: GE_DISCORD_AUTOACCEPT=<s> lets in a Discord Ask to Join that has
 * waited that many seconds (harness; the lobby screen shows it meanwhile) */
static void coopPresenceHarness(void)
{
    static s32 after = -2;
    static s32 waited;

    if (after == -2)
    {
        const char *e = getenv("GE_DISCORD_AUTOACCEPT");
        after = (e != NULL) ? atoi(e) * 60 : -1;
    }
    if (after < 0 || coopDiscordAskCount() == 0 || !coopLobbyIsHost())
    {
        waited = 0;
        return;
    }
    if (++waited > after)
    {
        coopDiscordRespond(0, 1);
        waited = 0;
    }
}

void coopPresenceGameTick(s32 instage)
{
    CoopPresence p;
    char status[96], details[96], state[96];

    if (instage && bossGetStageNum() == LEVELID_TITLE)
    {
        return;     /* the menus' tick has it */
    }
    if (!coopEnabled())
    {
        coopFeedClock();
    }
    coopPresenceBuild(&p, instage, status, details, state, sizeof(status));
    coopPresenceTick(&p);
    coopPresenceHarness();
    if (instage)
    {
        coopPresenceJoinLater();
    }
    else
    {
        coopPresenceJoinNow();
    }
}

s32 coopLobbyDiscordAsks(void)
{
    return coopLobbyIsHost() ? coopDiscordAskCount() : 0;
}
