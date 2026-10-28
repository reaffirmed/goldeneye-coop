/**
 * Story co-op votes: what to do after a mission, and skipping a cutscene, so
 * the host is not the only one who decides.
 *
 *  - After a mission the debrief has a third page, WHAT NEXT: replay the
 *    mission, the next one (after a win) or back to the lobby. Each player
 *    votes there; a vote can be changed until it is decided. It is decided
 *    when everyone has voted, or 30 s after the first vote with the votes in
 *    by then. Most votes win; a tie goes the host's way if the host is in it,
 *    else to the lobby. The host starts the mission (the others follow it in
 *    as from the lobby) or everyone goes to the lobby.
 *  - In a cutscene (the mission intro, or the ending that waits for a press)
 *    a skip press is a vote. It passes once more than half the players
 *    want it (both of two, two of three, three of four), and then every game
 *    skips. Unanswered, the cutscene runs out as it always did. With nobody
 *    else in the stage a press skips at once.
 *
 * The host counts both: a joiner's vote goes to the host (COOPEV_VOTE), and
 * the host sends the count to everyone after each change.
 *
 * Harness: GE_COOP_TESTVOTE=<choice>[:<s>]: vote that (0 replay, 1 next,
 * 2 lobby) <s> seconds (default 3) into the debrief. GE_COOP_TESTSKIP=<s>:
 * press skip <s> seconds into each cutscene.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include <boss.h>
#include "bondview.h"
#include "coop.h"

extern void portConLogf(const char *fmt, ...);

#define VOTE_WAIT       (30 * 60)   /* after the first vote, decide with the votes in */
#define VOTE_ACT_AFTER  90          /* host: show the outcome this long, then act on it */
#define VOTE_RESEND     60          /* host: the count again this often while a vote is open (a joiner back late) */

enum { VOTEMSG_RESULT = 1, VOTEMSG_TALLY, VOTEMSG_SKIP, VOTEMSG_SKIPTALLY };

typedef struct
{
    u8 what;            /* VOTEMSG_* */
    u8 choice;          /* RESULT: COOPVOTE_*; SKIP / SKIPTALLY: the cutscene, COOPSKIP_* */
    u8 outcome;         /* TALLY: COOPVOTE_* once decided, 0xFF open; SKIPTALLY: 1 passed */
    s8 next;            /* TALLY: the next mission's stage, -1 none */
    u8 votes[COOP_MAX_PLAYERS];   /* TALLY: each player's, 0xFF none; SKIPTALLY: 1 wants to skip */
    s32 stage;          /* the mission voted on, or the cutscene's */
    s16 left;           /* TALLY: ticks until it is decided anyway, -1 no clock yet */
    u8 need;            /* SKIPTALLY: votes it takes */
    u8 pad;
} CoopVoteMsg;

/* the results vote, as the host last told us (or as we count it, hosting) */
static s32 sStage = -1;
static u8 sVotes[COOP_MAX_PLAYERS];
static s32 sOutcome = -1;
static s32 sNext = -1;
static s32 sLeft = -1;
static u32 sFirstTick;
static s32 sActIn;          /* host: ticks until we act on the outcome */
static s32 sMine = -1;      /* our own vote, shown at once */
static u32 sTick;

/* the skip vote */
static s32 sSkipKind;       /* the cutscene the count is for, 0 none */
static s32 sSkipStage;
static u8 sSkipVotes[COOP_MAX_PLAYERS];
static s32 sSkipNeed;
static s32 sSkipPassed;
static s32 sSkipMine;

static s32 coopVoteCount(const u8 *votes, s32 choice)
{
    s32 p, n = 0;

    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        n += (votes[p] == choice);
    }
    return n;
}

/* players still talking to us, us included: one who crashed or lost the line
 * stops counting after 3 s, not after the 30 s their connection is kept */
static s32 coopVotePresent(void)
{
    s32 p, n = 0;

    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        n += (p == coopDesignSelf() || coopLobbyPlayerHere(p));
    }
    return (n < 1) ? 1 : n;
}

/* ---- after the mission ---------------------------------------------------- */

static void coopVoteReset(s32 stage)
{
    sStage = stage;
    memset(sVotes, 0xFF, sizeof(sVotes));
    sOutcome = -1;
    sLeft = -1;
    sFirstTick = 0;
    sActIn = 0;
    sMine = -1;
    sNext = coopVoteNextStage();
}

static void coopVoteSendTally(void)
{
    CoopVoteMsg m;

    memset(&m, 0, sizeof(m));
    m.what = VOTEMSG_TALLY;
    m.stage = sStage;
    m.outcome = (sOutcome < 0) ? 0xFF : (u8) sOutcome;
    m.next = (s8) sNext;
    memcpy(m.votes, sVotes, sizeof(m.votes));
    m.left = (s16) sLeft;
    coopDesignSend(COOPEV_VOTE, COOP_ALL_PLAYERS, &m, sizeof(m));
}

/* host: a vote, ours or a joiner's */
static void coopVoteTake(s32 player, s32 choice)
{
    if (sOutcome >= 0 || player < 0 || player >= COOP_MAX_PLAYERS || choice < 0 || choice >= COOPVOTE_COUNT
        || (choice == COOPVOTE_NEXT && sNext < 0))
    {
        return;
    }
    if (sFirstTick == 0)
    {
        sFirstTick = sTick;
    }
    sVotes[player] = (u8) choice;
    portConLogf("ge: coop: vote: player %d votes %s\n", player + 1, coopVoteName(choice));
}

/* the votes of the players still here: a player who left takes theirs along */
static void coopVoteHere(const u8 *votes, u8 *out)
{
    s32 p;

    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        out[p] = (p == coopDesignSelf() || coopLobbyPlayerHere(p)) ? votes[p] : 0xFF;
    }
}

/* host: everyone has voted, or the time is up */
static void coopVoteDecide(void)
{
    s32 voted = 0, best = -1, bestn = 0, tie = 0, c, p, n;
    u8 votes[COOP_MAX_PLAYERS];
    s32 host;

    coopVoteHere(sVotes, votes);
    host = votes[coopDesignSelf()];
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        voted += (votes[p] != 0xFF);
    }
    if (voted == 0)
    {
        return;
    }
    sLeft = (s32) (VOTE_WAIT - (sTick - sFirstTick));
    if (sLeft < 0)
    {
        sLeft = 0;
    }
    if (voted < coopVotePresent() && sLeft > 0)
    {
        return;
    }
    for (c = 0; c < COOPVOTE_COUNT; c++)
    {
        n = coopVoteCount(votes, c);
        if (n > bestn)
        {
            best = c;
            bestn = n;
            tie = 0;
        }
        else if (n == bestn && n > 0)
        {
            tie = 1;
        }
    }
    if (tie)
    {   /* the host's way if the host is in the tie, else the lobby */
        best = (host != 0xFF && coopVoteCount(votes, host) == bestn) ? host : COOPVOTE_LOBBY;
    }
    sOutcome = best;
    sActIn = VOTE_ACT_AFTER;
    portConLogf("ge: coop: vote: decided %s (%d of %d voted, %d for it)\n", coopVoteName(best), voted, coopVotePresent(), bestn);
}

static void coopVoteAct(void)
{
    if (sOutcome == COOPVOTE_REPLAY)
    {
        coopVoteStartMission(sStage);
    }
    else if (sOutcome == COOPVOTE_NEXT && sNext >= 0)
    {
        coopVoteStartMission(sNext);
    }
    else
    {
        coopVoteToLobby();
    }
}

static void coopVoteHarness(void)
{
    static s32 choice = -2, at, n, stage = -1;

    if (choice == -2)
    {
        const char *e = getenv("GE_COOP_TESTVOTE");

        choice = (e != NULL) ? atoi(e) : -1;
        at = (e != NULL && strchr(e, ':') != NULL) ? atoi(strchr(e, ':') + 1) * 60 : 180;
    }
    if (choice < 0 || !coopVoteShown())
    {
        return;
    }
    if (stage != sStage)
    {
        stage = sStage;
        n = 0;
    }
    if (++n == at)
    {
        portConLogf("ge: coop: vote: harness votes %s\n", coopVoteName(choice));
        coopVotePick(choice);
    }
}

/* front.c's debrief: a vote is open for the mission just played */
s32 coopVoteShown(void)
{
    return coopSessionActive() && !(coopSetting(COOPSET_VSMODE) & 1) && coopVoteThisStage() >= 0 && sStage == coopVoteThisStage();
}

/* front.c: our vote */
void coopVotePick(s32 choice)
{
    CoopVoteMsg m;

    if (!coopVoteShown() || sOutcome >= 0 || choice < 0 || choice >= COOPVOTE_COUNT || (choice == COOPVOTE_NEXT && sNext < 0))
    {
        return;
    }
    sMine = choice;
    if (coopDesignIsHost())
    {
        coopVoteTake(coopDesignSelf(), choice);
        coopVoteDecide();
        coopVoteSendTally();
        return;
    }
    memset(&m, 0, sizeof(m));
    m.what = VOTEMSG_RESULT;
    m.choice = (u8) choice;
    m.stage = sStage;
    coopDesignSend(COOPEV_VOTE, 0, &m, sizeof(m));
}

/* front.c: player p's vote (0xFF none), the outcome (-1 open), the next
 * mission (-1 none), ticks until it is decided anyway (-1 no clock) */
s32 coopVoteOf(s32 p)
{
    if (p == coopDesignSelf() && sMine >= 0 && sOutcome < 0)
    {
        return sMine;   /* ours shows before the host's count comes back */
    }
    return (p >= 0 && p < COOP_MAX_PLAYERS) ? sVotes[p] : 0xFF;
}

s32 coopVoteOutcome(void)
{
    return sOutcome;
}

s32 coopVoteNext(void)
{
    return sNext;
}

s32 coopVoteLeft(void)
{
    return sLeft;
}

const char *coopVoteName(s32 choice)
{
    switch (choice)
    {
        case COOPVOTE_REPLAY:   return "replay";
        case COOPVOTE_NEXT:     return "next mission";
        case COOPVOTE_LOBBY:    return "lobby";
    }
    return "none";
}

/* ---- skipping a cutscene -------------------------------------------------- */

static void coopSkipReset(s32 kind)
{
    sSkipKind = kind;
    sSkipStage = bossGetStageNum();
    memset(sSkipVotes, 0, sizeof(sSkipVotes));
    sSkipNeed = coopVotePresent() / 2 + 1;
    sSkipPassed = 0;
    sSkipMine = 0;
}

static void coopSkipSendTally(void)
{
    CoopVoteMsg m;

    memset(&m, 0, sizeof(m));
    m.what = VOTEMSG_SKIPTALLY;
    m.choice = (u8) sSkipKind;
    m.stage = sSkipStage;
    m.outcome = (u8) sSkipPassed;
    m.need = (u8) sSkipNeed;
    memcpy(m.votes, sSkipVotes, sizeof(m.votes));
    coopDesignSend(COOPEV_VOTE, COOP_ALL_PLAYERS, &m, sizeof(m));
}

/* host: the skip votes of the players still here, against more than half of them */
static s32 coopSkipCount(void)
{
    s32 p, n = 0;

    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        n += sSkipVotes[p] && (p == coopDesignSelf() || coopLobbyPlayerHere(p));
    }
    sSkipNeed = coopVotePresent() / 2 + 1;
    sSkipPassed = (n >= sSkipNeed);
    return n;
}

/* host: player p wants this cutscene skipped */
static void coopSkipTake(s32 player, s32 kind, s32 stage)
{
    s32 n;

    if (stage != bossGetStageNum() || player < 0 || player >= COOP_MAX_PLAYERS)
    {
        return;
    }
    if (sSkipKind != kind || sSkipStage != stage)
    {
        coopSkipReset(kind);
    }
    if (sSkipPassed)
    {
        return;
    }
    sSkipVotes[player] = 1;
    n = coopSkipCount();
    portConLogf("ge: coop: skip vote: player %d, %d of %d needed%s\n", player + 1, n, sSkipNeed, sSkipPassed ? ": skipping" : "");
    coopSkipSendTally();
}

/* host, each tick of a skip vote: someone left, so it may need fewer now */
static void coopSkipRecount(void)
{
    const s32 need = sSkipNeed;
    s32 n;

    if (sSkipKind == 0 || sSkipPassed || sSkipStage != bossGetStageNum())
    {
        return;
    }
    n = coopSkipCount();
    if (sSkipNeed != need || sSkipPassed)
    {
        portConLogf("ge: coop: skip vote: %d of %d needed now%s\n", n, sSkipNeed, sSkipPassed ? ": skipping" : "");
        coopSkipSendTally();
    }
}

/* bondview2.c: a skip press in cutscene `kind` (COOPSKIP_*). 1: skip now (no
 * one else to ask); 0: it was a vote, coopSkipPassed says when it passes */
s32 coopSkipPress(s32 kind)
{
    CoopVoteMsg m;

    if (!coopSessionActive() || coopVotePresent() < 2)
    {
        return 1;
    }
    if (sSkipKind != kind || sSkipStage != bossGetStageNum())
    {
        coopSkipReset(kind);
    }
    if (sSkipMine)
    {
        return 0;
    }
    sSkipMine = 1;
    if (coopDesignIsHost())
    {
        coopSkipTake(coopDesignSelf(), kind, bossGetStageNum());
        return 0;
    }
    memset(&m, 0, sizeof(m));
    m.what = VOTEMSG_SKIP;
    m.choice = (u8) kind;
    m.stage = bossGetStageNum();
    coopDesignSend(COOPEV_VOTE, 0, &m, sizeof(m));
    portConLogf("ge: coop: skip vote sent\n");
    return 0;
}

/* bondview2.c: enough of us want cutscene `kind` skipped */
s32 coopSkipPassed(s32 kind)
{
    return sSkipPassed && sSkipKind == kind && sSkipStage == bossGetStageNum();
}

/* coop_hud.c: votes for skipping the cutscene on now, and how many it takes;
 * 0 when nobody has asked */
s32 coopSkipVotes(s32 *need, s32 *mine)
{
    s32 p, n = 0;

    if (sSkipKind == 0 || sSkipStage != bossGetStageNum() || sSkipPassed)
    {
        return 0;
    }
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        n += sSkipVotes[p];
    }
    if (sSkipMine && !sSkipVotes[coopDesignSelf()])
    {
        n++;    /* ours, before the host's count comes back */
    }
    *need = sSkipNeed;
    *mine = sSkipMine;
    return n;
}

/* coop_hud.c: whose skip we still wait for, -1 nobody */
s32 coopSkipWaitingFor(s32 i)
{
    s32 p;

    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        if (coopLobbyPlayerHere(p) && !sSkipVotes[p] && !(p == coopDesignSelf() && sSkipMine) && i-- == 0)
        {
            return p;
        }
    }
    return -1;
}

static s32 sSkipHarnessPress;

static void coopSkipHarness(void)
{
    static s32 at = -2;
    static s32 n;

    if (at == -2)
    {
        const char *e = getenv("GE_COOP_TESTSKIP");

        at = (e != NULL) ? atoi(e) * 60 : -1;
    }
    if (at < 0)
    {
        return;
    }
    if (!coopCutsceneActive() && !stop_time_flag)
    {
        n = 0;  /* each cutscene counts from its start */
        return;
    }
    if (++n == at)
    {
        portConLogf("ge: coop: skip vote: harness presses skip\n");
        sSkipHarnessPress = 1;
    }
}

/* bondview2.c: the harness pressed skip (GE_COOP_TESTSKIP), once */
s32 coopSkipTestPress(void)
{
    s32 was = sSkipHarnessPress;

    sSkipHarnessPress = 0;
    return was;
}

/* host migration renumbered everyone: newof[old] = new, -1 gone (coop.c coopRenumber) */
void coopVoteRenumber(const s32 *newof)
{
    u8 votes[COOP_MAX_PLAYERS], skips[COOP_MAX_PLAYERS];
    s32 p;

    memset(votes, 0xFF, sizeof(votes));
    memset(skips, 0, sizeof(skips));
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        if (newof[p] >= 0 && newof[p] < COOP_MAX_PLAYERS)
        {
            votes[newof[p]] = sVotes[p];
            skips[newof[p]] = sSkipVotes[p];
        }
    }
    memcpy(sVotes, votes, sizeof(sVotes));
    memcpy(sSkipVotes, skips, sizeof(sSkipVotes));
}

/* ---- events and the tick -------------------------------------------------- */

void coopVoteApply(const u8 *raw, s32 from)
{
    CoopVoteMsg m;

    memcpy(&m, raw, sizeof(m));
    switch (m.what)
    {
        case VOTEMSG_RESULT:
            if (coopDesignIsHost() && m.stage == sStage)
            {
                coopVoteTake(from, m.choice);
                coopVoteDecide();
                coopVoteSendTally();
            }
            break;
        case VOTEMSG_TALLY:
            if (coopDesignIsHost() || from != 0)
            {
                break;
            }
            if (m.stage != sStage)
            {
                coopVoteReset(m.stage);
            }
            memcpy(sVotes, m.votes, sizeof(sVotes));
            sNext = m.next;
            sLeft = m.left;
            if (sOutcome < 0 && m.outcome != 0xFF)
            {
                sOutcome = m.outcome;
                portConLogf("ge: coop: vote: the host says %s\n", coopVoteName(sOutcome));
                if (sOutcome == COOPVOTE_LOBBY)
                {
                    sActIn = VOTE_ACT_AFTER;
                }
            }
            break;
        case VOTEMSG_SKIP:
            if (coopDesignIsHost())
            {
                coopSkipTake(from, m.choice, m.stage);
            }
            break;
        case VOTEMSG_SKIPTALLY:
            if (coopDesignIsHost() || from != 0 || m.stage != bossGetStageNum())
            {
                break;
            }
            if (sSkipKind != m.choice || sSkipStage != m.stage)
            {
                coopSkipReset(m.choice);
            }
            memcpy(sSkipVotes, m.votes, sizeof(sSkipVotes));
            sSkipNeed = m.need;
            if (!sSkipPassed && m.outcome)
            {
                sSkipPassed = 1;
                portConLogf("ge: coop: skip vote passed, skipping\n");
            }
            break;
    }
}

/* coop_ext.c, every tick (menus too) */
void coopVoteTick(s32 inmission)
{
    static s32 wasin;
    const s32 instage = bossGetStageNum() != LEVELID_TITLE;    /* the intro too, which is not yet "in mission" */

    (void) inmission;
    sTick++;
    if (instage && !wasin)
    {
        sStage = -1;    /* a mission starts: its debrief has its own vote */
    }
    wasin = instage;
    if (instage)
    {
        if (!coopCutsceneActive() && !stop_time_flag)
        {
            sSkipKind = 0;
        }
        if (coopDesignIsHost())
        {
            coopSkipRecount();
        }
        coopSkipHarness();
        return;
    }
    sSkipKind = 0;
    if (!coopSessionActive() || (coopSetting(COOPSET_VSMODE) & 1))
    {
        return;
    }
    if (coopDesignIsHost() && coopVoteThisStage() >= 0 && sStage != coopVoteThisStage())
    {   /* back from a mission: open its vote */
        coopVoteReset(coopVoteThisStage());
        coopVoteSendTally();
    }
    coopVoteHarness();
    if (coopDesignIsHost() && sStage >= 0 && sOutcome < 0)
    {
        coopVoteDecide();
        if (sOutcome >= 0 || (sTick % VOTE_RESEND) == 0)
        {
            coopVoteSendTally();
        }
    }
    if (sOutcome >= 0 && sActIn > 0 && --sActIn == 0)
    {
        if (coopDesignIsHost())
        {
            coopVoteAct();
        }
        else if (sOutcome == COOPVOTE_LOBBY)
        {
            coopVoteToLobby();
        }
    }
}
