/**
 * Story co-op: typed text chat and the toast line.
 *
 * The chat key (default T, [controls] chat) opens a line at the bottom left;
 * typing goes through the port's text input (video.c portTextInputStart), so
 * the keys stop reaching Bond while it is open. Return sends the line to
 * everyone, Escape (or the key again on an empty line) drops it. The lobby
 * opens the same line with its own key (front.c).
 *
 * A line rides COOPEV_CHAT events, up to COOPCHAT_PARTS of 24 characters,
 * which the reliable channel delivers in order; the receiver joins the parts
 * of each message number and shows the line once the last part is in.
 *
 * The feed keeps the newest COOPFEED_LINES lines with who said them; each
 * fades after COOPFEED_SHOW ticks in play (the lobby shows the newest few
 * whatever their age). Toasts (coopFeedToast: a partner completed an
 * objective, joined or left, gave us ammo; the host left, ...) stack above
 * the chat lines, newest lowest, for COOPTOAST_SHOW ticks, fading in and
 * out: the lead in the player's colour and the rest after it in white,
 * word-wrapped to three fifths of the screen, at most three rows. While the host is not
 * answering (coop.c coopHudSessionBanner) coop_screens.c's panel says so.
 *
 * The HUD font has no lowercase, so everything shows in capitals.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include "textrelated.h"
#include "front.h"
#include "fr.h"
#include "player.h"
#include "othermodemicrocode.h"
#include "bondview.h"
#include "options.h"
#include "lv.h"
#include "coop.h"

extern void portConLogf(const char *fmt, ...);
extern Gfx *portHudScaleBegin(Gfx *gdl, f32 x, f32 y);
extern Gfx *portHudScaleEnd(Gfx *gdl);

#ifdef GE_VIDEO
extern void portTextInputStart(void);
extern void portTextInputStop(void);
extern void portTextInputPoll(char *chr, s32 *key);
extern s32 portTextInputActive(void);
#define PORTTEXT_BACKSPACE 1
#define PORTTEXT_ACCEPT    2
#define PORTTEXT_CANCEL    3
#else
#define portTextInputStart()
#define portTextInputStop()
#define portTextInputPoll(c, k) do { if (c) *(c) = 0; if (k) *(k) = 0; } while (0)
#define portTextInputActive() 0
#define PORTTEXT_BACKSPACE 1
#define PORTTEXT_ACCEPT    2
#define PORTTEXT_CANCEL    3
#endif

#define COOPCHAT_PART     24    /* characters in one event */
#define COOPCHAT_PARTS    3
#define COOPCHAT_MAX      (COOPCHAT_PART * COOPCHAT_PARTS - 9)  /* typed; leaves room for "NAME: " on screen */
#define COOPFEED_LINES    6
#define COOPFEED_SHOW     (60 * 10)     /* a line stays 10 s in play */
#define COOPFEED_FADE     60
#define COOPTOAST_SHOW    (60 * 5)
#define COOPTOAST_FADEIN  10
#define COOPTOAST_FADE    30
#define COOPTOAST_MAX     4
#define COOPTOAST_ALERTCOL 0xFF6060FF
#define COOPTOAST_LINE    0x0B  /* a toast line's height */

typedef struct
{
    u8 msg;     /* the sender's message number */
    u8 part;    /* from 0 */
    u8 parts;
    u8 len;     /* characters in text */
    char text[COOPCHAT_PART];
} CoopChatPart;

typedef struct
{
    char text[80];
    s32 player;     /* -1 a note from the game */
    u32 tick;
} CoopFeedLine;

static s32 sTyping;
static char sLine[COOPCHAT_MAX + 1];
static s32 sLineLen;
static s32 sEatEscape;      /* ticks the pause must ignore Escape for */
static s32 sTypingInMission;
static u8 sNextMsg;
static u32 sNow;            /* ticks, menus included */

/* a message being put together, per sender */
static struct
{
    u8 msg;
    u8 have;    /* parts in so far */
    char text[COOPCHAT_PART * COOPCHAT_PARTS + 1];
} sIn[COOP_MAX_PLAYERS];

static CoopFeedLine sFeed[COOPFEED_LINES];
static s32 sFeedCount;

static struct
{
    char text[112];
    s32 player;     /* -1 a note from the game, COOPTOAST_ALERT session news */
    u32 tick;
} sToast[COOPTOAST_MAX];
static s32 sToastCount;

static u32 coopFeedColour(s32 player)
{
    static const u32 colours[COOP_MAX_PLAYERS] = { 0xFFFF80FF, 0x80E0FFFF, 0xFF90E0FF, 0x90FF90FF };

    if (player == COOPTOAST_ALERT)
    {
        return COOPTOAST_ALERTCOL;
    }
    return (player >= 0 && player < COOP_MAX_PLAYERS) ? colours[player] : 0xFFFFFFFF;
}

static u32 coopFeedAlpha(u32 colour, s32 a)
{
    a = (a < 0) ? 0 : (a > 0xFF) ? 0xFF : a;
    return (colour & ~0xFFu) | (u32) (((colour & 0xFF) * (u32) a) / 0xFF);
}

/* HUD-font copy: capitals, ASCII, one line ending in '\n' */
static void coopFeedText(char *out, s32 cap, const char *in)
{
    s32 n;

    coopAsciiName(out, cap - 1, in, 1);
    n = (s32) strlen(out);
    out[n++] = '\n';
    out[n] = '\0';
}

static void coopFeedPush(const char *text, s32 player)
{
    if (sFeedCount == COOPFEED_LINES)
    {
        memmove(&sFeed[0], &sFeed[1], sizeof(sFeed[0]) * (COOPFEED_LINES - 1));
        sFeedCount--;
    }
    snprintf(sFeed[sFeedCount].text, sizeof(sFeed[0].text), "%s", text);
    sFeed[sFeedCount].player = player;
    sFeed[sFeedCount].tick = sNow;
    sFeedCount++;
}

static void coopChatShow(s32 player, const char *said)
{
    char name[24];
    char line[96];
    const char *who = coopPlayerName(player);

    snprintf(name, sizeof(name), "%s", (who != NULL && who[0] != '\0') ? who : "PARTNER");
    snprintf(line, sizeof(line), "%s: %s", name, said);
    coopFeedPush(line, player);
    portConLogf("ge: coop: chat from player %d: %s\n", player + 1, said);
}

/* the feed's clock while co-op is off (coopChatTick runs only with it on), so
 * a toast shown then (an invite during a solo mission) still goes away */
void coopFeedClock(void)
{
    sNow++;
}

void coopFeedToast(const char *text, s32 player)
{
    if (sToastCount == COOPTOAST_MAX)
    {
        memmove(&sToast[0], &sToast[1], sizeof(sToast[0]) * (COOPTOAST_MAX - 1));
        sToastCount--;
    }
    snprintf(sToast[sToastCount].text, sizeof(sToast[0].text), "%s", text);
    {   /* hudmsg style ("... left the game.\n"): no full stop on a toast */
        char *t = sToast[sToastCount].text;
        s32 n = (s32) strlen(t);

        while (n > 0 && (t[n - 1] == '\n' || t[n - 1] == '.'))
        {
            t[--n] = '\0';
        }
    }
    sToast[sToastCount].player = player;
    sToast[sToastCount].tick = sNow;
    sToastCount++;
    portConLogf("ge: coop: toast: %s\n", text);
}

/* ---- sending -------------------------------------------------------------- */

static void coopChatSend(const char *text)
{
    s32 len = (s32) strlen(text);
    s32 parts = (len + COOPCHAT_PART - 1) / COOPCHAT_PART;
    s32 i;

    if (parts < 1)
    {
        return;
    }
    if (parts > COOPCHAT_PARTS)
    {
        parts = COOPCHAT_PARTS;
    }
    sNextMsg++;
    for (i = 0; i < parts; i++)
    {
        CoopChatPart cp;
        s32 n = len - i * COOPCHAT_PART;

        memset(&cp, 0, sizeof(cp));
        cp.msg = sNextMsg;
        cp.part = (u8) i;
        cp.parts = (u8) parts;
        cp.len = (u8) ((n > COOPCHAT_PART) ? COOPCHAT_PART : n);
        memcpy(cp.text, text + i * COOPCHAT_PART, cp.len);
        coopExtSend(COOPEV_CHAT, 0xFF /* COOP_ALL */, &cp, sizeof(cp));
    }
    coopChatShow(coopExtSelf(), text);
}

void coopChatApply(const u8 *raw, s32 from)
{
    CoopChatPart cp;

    memcpy(&cp, raw, sizeof(cp));
    if (from < 0 || from >= COOP_MAX_PLAYERS || cp.parts < 1 || cp.parts > COOPCHAT_PARTS || cp.part >= cp.parts
        || cp.len > COOPCHAT_PART)
    {
        return;
    }
    if (cp.part == 0 || cp.msg != sIn[from].msg)
    {   /* a new message: in-order delivery means a stale one can only be cut short */
        sIn[from].msg = cp.msg;
        sIn[from].have = 0;
        memset(sIn[from].text, 0, sizeof(sIn[from].text));
    }
    if (cp.part != sIn[from].have)
    {
        return;
    }
    memcpy(sIn[from].text + cp.part * COOPCHAT_PART, cp.text, cp.len);
    sIn[from].have++;
    if (sIn[from].have == cp.parts)
    {
        coopChatShow(from, sIn[from].text);
    }
}

/* ---- typing ---------------------------------------------------------------- */

static void coopChatClose(void)
{
    if (sTyping)
    {
        portTextInputStop();
    }
    sTyping = 0;
    sLineLen = 0;
    sLine[0] = '\0';
}

void coopChatPress(void)
{
    if (sTyping || !coopSessionActive() || !coopLobbyHasPartner() || portTextInputActive())
    {
        return;
    }
    sTyping = 1;
    sLineLen = 0;
    sLine[0] = '\0';
    sTypingInMission = 0;
    portTextInputStart();
}

s32 coopChatTyping(void)
{
    return sTyping;
}

s32 coopChatEatEscape(void)
{
    return sTyping || sEatEscape > 0;
}

/* GE_COOP_TESTCHAT=<s>:<text>: <s> seconds into the mission, open the line
 * with <text> typed in ('_' for a space), and send it 2 s later;
 * GE_COOP_TESTLOBBYCHAT=<s>:<text> the same <s> seconds after a partner shows
 * up in the lobby (harness only) */
typedef struct
{
    const char *env;
    s32 at;     /* -2 not read yet, -1 off or done */
    u32 base;
    char text[COOPCHAT_MAX + 1];
} CoopChatTest;

static void coopChatTestOne(CoopChatTest *t, s32 active, u32 now)
{
    if (t->at == -2)
    {
        const char *e = getenv(t->env);
        const char *colon = (e != NULL) ? strchr(e, ':') : NULL;
        char *u;

        t->at = (colon != NULL) ? atoi(e) * 60 : -1;
        snprintf(t->text, sizeof(t->text), "%s", (colon != NULL) ? colon + 1 : "");
        for (u = t->text; *u != '\0'; u++)
        {
            *u = (*u == '_') ? ' ' : *u;   /* the harness's env lists split on spaces */
        }
    }
    if (t->at < 0 || !active)
    {
        return;
    }
    if (t->base == 0)
    {
        t->base = now + 1;
    }
    if (now - (t->base - 1) == (u32) t->at)
    {
        coopChatPress();
        snprintf(sLine, sizeof(sLine), "%s", t->text);
        sLineLen = (s32) strlen(sLine);
    }
    else if (now - (t->base - 1) == (u32) t->at + 120 && sTyping)
    {
        coopChatSend(sLine);
        coopChatClose();
        t->at = -1;
    }
}

/* GE_COOP_TESTTOASTS=<s>: from <s> seconds into the mission, one toast of
 * each kind every 1.5 s, to see them stack (harness only) */
static void coopToastTestTick(s32 inmission)
{
    static const struct
    {
        const char *text;
        s32 player;
    } toasts[] = {
        { "PARTNER 2 COMPLETED A|FIND PLANE IGNITION KEY", 1 },
        { "PARTNER 2 gave you 30 PP7 rounds", 1 },
        { "Partner 3 left the mission.\n", 2 },
        { "PARTNER 4 COMPLETED C|DESTROY ALL ALARMS AND COMMUNICATION EQUIPMENT", 3 },
        { "YOU ARE THE HOST NOW|PARTNER 1 LEFT", COOPTOAST_ALERT },
    };
    static s32 at = -2;
    s32 t;

    if (at == -2)
    {
        const char *e = getenv("GE_COOP_TESTTOASTS");

        at = (e != NULL) ? atoi(e) * 60 : -1;
    }
    t = (s32) coopExtMissionTicks() - at;
    if (at < 0 || !inmission || t < 0 || t % 90 != 0)
    {
        return;
    }
    if (t / 90 < (s32) (sizeof(toasts) / sizeof(toasts[0])))
    {
        coopFeedToast(toasts[t / 90].text, toasts[t / 90].player);
    }
}

static void coopChatTestTick(s32 inmission)
{
    static CoopChatTest mission = { "GE_COOP_TESTCHAT", -2 };
    static CoopChatTest lobby = { "GE_COOP_TESTLOBBYCHAT", -2 };

    coopToastTestTick(inmission);

    coopChatTestOne(&mission, inmission && coopLobbyHasPartner(), coopExtMissionTicks());
    coopChatTestOne(&lobby, !inmission && coopLobbyHasPartner() && coopLobbyPartnerOnScreen(), sNow);
}

void coopChatTick(s32 inmission)
{
    s32 guard;

    sNow++;
    coopChatTestTick(inmission);
    if (sEatEscape > 0)
    {
        sEatEscape--;
    }
    if (!sTyping)
    {
        return;
    }
    if (inmission)
    {
        sTypingInMission = 1;
    }
    else if (sTypingInMission || !coopSessionActive())
    {   /* the mission ended (or the session did) under the line */
        coopChatClose();
        return;
    }
    for (guard = 0; guard < 64 && sTyping; guard++)
    {
        char c = 0;
        s32 key = 0;

        portTextInputPoll(&c, &key);
        if (c == 0 && key == 0)
        {
            break;
        }
        if (c >= 0x20 && c < 0x7F)
        {
            if (sLineLen < COOPCHAT_MAX)
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
            s32 start = 0;

            while (sLine[start] == ' ')
            {
                start++;
            }
            if (sLine[start] != '\0')
            {
                coopChatSend(sLine + start);
            }
            coopChatClose();
            sEatEscape = 2;
        }
        else if (key == PORTTEXT_CANCEL)
        {
            coopChatClose();
            sEatEscape = 2;     /* the same Escape also latched the pause */
        }
    }
}

/* ---- drawing ----------------------------------------------------------------- */

s32 coopFeedLines(s32 maxlines, char lines[][80], s32 *players)
{
    s32 first = (sFeedCount > maxlines) ? sFeedCount - maxlines : 0;
    s32 n = 0;
    s32 i;

    for (i = first; i < sFeedCount; i++, n++)
    {   /* as typed: the lobby's font has lowercase */
        snprintf(lines[n], 80, "%s", sFeed[i].text);
        players[n] = sFeed[i].player;
    }
    return n;
}

void coopChatTypedLine(char *out, s32 cap)
{
    snprintf(out, cap, "%s", sTyping ? sLine : "");
}

/* cut a line (ending in '\n') with "..." so it is no wider than maxw */
static void coopFeedFit(char *text, s32 maxw)
{
    char orig[128];
    s32 th = 0, tw = 0;
    s32 k;

    textMeasure(&th, &tw, text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    if (tw <= maxw)
    {
        return;
    }
    snprintf(orig, sizeof(orig), "%s", text);
    for (k = (s32) strlen(orig) - 2; k > 1; k--)
    {
        sprintf(text, "%.*s...\n", (int) k, orig);
        textMeasure(&th, &tw, text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
        if (tw <= maxw)
        {
            return;
        }
    }
}

static Gfx *coopFeedPrint(Gfx *gdl, s32 x, s32 y, char *text, u32 colour)
{
    return textRenderOutlined(gdl, &x, &y, text, ptrFontBankGothicChars, ptrFontBankGothic, colour,
                              coopFeedAlpha(0x000000A0, (s32) (colour & 0xFF)), (s32) viGetX(), viGetY(), 0, 0);
}

#define COOPTOAST_LINES 3

typedef struct
{
    char text[100];     /* HUD text ending in '\n' */
    s32 x;              /* from the toast's left */
    s32 w;
    s32 row;
    s32 lead;           /* in the player's colour, else white */
} CoopToastPiece;

static s32 coopFeedWidth(const char *text)
{
    s32 th = 0, tw = 0;

    textMeasure(&th, &tw, (char *) text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    return tw;
}

/* as many of in's words as fit avail (HUD text in out); returns the rest,
 * or in itself when not even the first word fits */
static const char *coopToastWords(const char *in, s32 avail, char *out, s32 cap)
{
    s32 cut = 0;
    s32 n;

    for (n = 0; ; n++)
    {
        if (in[n] == ' ' || in[n] == '\0')
        {
            char try[112];

            snprintf(try, sizeof(try), "%.*s", (int) n, in);
            coopFeedText(out, cap, try);
            if (coopFeedWidth(out) > avail)
            {
                break;
            }
            cut = n;
            if (in[n] == '\0')
            {
                break;
            }
        }
    }
    snprintf(out, cap, "%.*s", (int) cut, in);
    coopFeedText(out, cap, out);
    in += cut;
    while (*in == ' ')
    {
        in++;
    }
    return in;
}

/* add the words of in at row/x on, wrapping to new rows up to COOPTOAST_LINES;
 * the last row is cut with "..." */
static s32 coopToastFlow(CoopToastPiece *pc, s32 n, s32 *row, s32 *x, const char *in, s32 maxw, s32 lead)
{
    while (*in != '\0' && n < COOPTOAST_LINES * 2)
    {
        CoopToastPiece *p = &pc[n];
        const char *next = coopToastWords(in, maxw - *x, p->text, sizeof(p->text));

        if (next == in)
        {   /* not one word fits: a new row, or cut it on the last */
            if (*x > 0 && *row < COOPTOAST_LINES - 1)
            {
                (*row)++;
                *x = 0;
                continue;
            }
            coopFeedText(p->text, sizeof(p->text), in);
            coopFeedFit(p->text, maxw - *x);
            next = "";
        }
        else if (*next != '\0' && *row == COOPTOAST_LINES - 1)
        {   /* more than the last row holds */
            coopFeedText(p->text, sizeof(p->text), in);
            coopFeedFit(p->text, maxw - *x);
            next = "";
        }
        p->x = *x;
        p->w = coopFeedWidth(p->text);
        p->row = *row;
        p->lead = lead;
        n++;
        in = next;
        *x += p->w + 4;
        if (*in != '\0')
        {
            (*row)++;
            *x = 0;
        }
    }
    return n;
}

/* a toast's pieces: the lead (to '|') then "- " and the rest, word-wrapped
 * to maxw; returns the pieces, *rows the rows they take */
static s32 coopToastLayout(const char *toast, s32 maxw, CoopToastPiece *pc, s32 *rows)
{
    char lead[112], rest[112];
    const char *bar = strchr(toast, '|');
    s32 row = 0, x = 0;
    s32 n;

    snprintf(lead, sizeof(lead), "%.*s", (int) ((bar != NULL) ? bar - toast : (s32) strlen(toast)), toast);
    n = coopToastFlow(pc, 0, &row, &x, lead, maxw, 1);
    if (bar != NULL && bar[1] != '\0' && row < COOPTOAST_LINES)
    {
        s32 first = n;
        s32 row0 = row;

        snprintf(rest, sizeof(rest), "- %s", bar + 1);
        n = coopToastFlow(pc, n, &row, &x, rest, maxw, 0);
        if (n > first && strcmp(pc[first].text, "-\n") == 0 && row > row0)
        {   /* only the dash fit beside the lead: the rest on its own rows, no dash */
            row = row0 + 1;
            x = 0;
            n = coopToastFlow(pc, first, &row, &x, bar + 1, maxw, 0);
        }
    }
    *rows = row + 1;
    return n;
}

/* one toast with its top row at y: a dark band, the lead in colour, the rest in white */
static Gfx *coopToastDraw(Gfx *gdl, s32 x, s32 y, CoopToastPiece *pc, s32 n, s32 rows, u32 colour, s32 a)
{
    s32 right = 0;
    s32 i;

    for (i = 0; i < n; i++)
    {
        right = (pc[i].x + pc[i].w > right) ? pc[i].x + pc[i].w : right;
    }
    gdl = microcode_constructor_related_to_menus(gdl, x - 3, y - 2, x + right + 3, y + rows * COOPTOAST_LINE - 1,
                                                 coopFeedAlpha(0x00000080, a));
    for (i = 0; i < n; i++)
    {
        gdl = coopFeedPrint(gdl, x + pc[i].x, y + pc[i].row * COOPTOAST_LINE, pc[i].text,
                            coopFeedAlpha(pc[i].lead ? colour : 0xFFFFFFFF, a));
    }
    return gdl;
}

/* the toasts stacked up from y (the line above the chat), newest lowest */
static Gfx *coopToastsRender(Gfx *gdl, s32 left, s32 y)
{
    const s32 maxw = viGetViewWidth() * 3 / 5;
    CoopToastPiece pc[COOPTOAST_LINES * 2];
    s32 n, rows;
    s32 i;

    /* the host not answering, or a new one being reached: coop_screens.c's panel */
    for (i = sToastCount - 1; i >= 0; i--)
    {
        u32 age = sNow - sToast[i].tick;
        s32 a = 0xFF;

        if (age < COOPTOAST_FADEIN)
        {
            a = (s32) ((age + 1) * 0xFF / (COOPTOAST_FADEIN + 1));
        }
        else if (age > COOPTOAST_SHOW - COOPTOAST_FADE)
        {
            a = (s32) ((COOPTOAST_SHOW - age) * 0xFF / COOPTOAST_FADE);
        }
        n = coopToastLayout(sToast[i].text, maxw, pc, &rows);
        y -= rows * COOPTOAST_LINE + 3;
        gdl = coopToastDraw(gdl, left, y + 3, pc, n, rows, coopFeedColour(sToast[i].player), a);
    }
    return gdl;
}

/* the chat lines, bottom left, newest lowest, and the toasts stacked above them */
Gfx *coopFeedRender(Gfx *gdl)
{
    s32 left = viGetViewLeft() + 0x14;
    s32 bottom = viGetViewTop() + viGetViewHeight() - 0x34;
    s32 y;
    s32 i;
    s32 any;

    while (sToastCount > 0 && sNow - sToast[0].tick >= COOPTOAST_SHOW)
    {
        memmove(&sToast[0], &sToast[1], sizeof(sToast[0]) * (COOPTOAST_MAX - 1));
        sToastCount--;
    }
    any = sTyping || sToastCount > 0;
    for (i = 0; i < sFeedCount && !any; i++)
    {
        any = sNow - sFeed[i].tick < COOPFEED_SHOW;
    }
    if (!any)
    {
        return gdl;
    }

    if (hudAnchorsToEdges())
    {
        PORT_HUD_ANCHOR(gdl, PORT_HUD_ANCHOR_LEFT);
    }
    gdl = portHudScaleBegin(gdl, (f32) viGetViewLeft(), (f32) (viGetViewTop() + viGetViewHeight()));
    gdl = microcode_constructor(gdl);
    y = bottom;
    if (sTyping)
    {
        char typed[96];
        char text[100];

        snprintf(typed, sizeof(typed), "SAY: %s_", sLine);
        coopFeedText(text, sizeof(text), typed);
        gdl = microcode_constructor_related_to_menus(gdl, left - 3, y - 2, left + viGetViewWidth() / 2, y + 0x0B, 0x00000080);
        gdl = coopFeedPrint(gdl, left, y, text, 0xFFFFFFFF);
        y -= 0x0C;
    }
    for (i = sFeedCount - 1; i >= 0; i--)
    {
        u32 age = sNow - sFeed[i].tick;
        char text[100];
        s32 a = 0xFF;

        if (age >= COOPFEED_SHOW && !sTyping)
        {
            continue;   /* faded (all of them show again while typing) */
        }
        if (!sTyping && age > COOPFEED_SHOW - COOPFEED_FADE)
        {
            a = (s32) ((COOPFEED_SHOW - age) * 0xFF / COOPFEED_FADE);
        }
        coopFeedText(text, sizeof(text), sFeed[i].text);
        gdl = coopFeedPrint(gdl, left, y, text, coopFeedAlpha(coopFeedColour(sFeed[i].player), a));
        y -= 0x0B;
    }
    gdl = coopToastsRender(gdl, left, y + 0x0B - 4);
    gdl = combiner_bayer_lod_perspective(gdl);
    gdl = portHudScaleEnd(gdl);
    if (hudAnchorsToEdges())
    {
        PORT_HUD_ANCHOR_OFF(gdl);
    }
    return gdl;
}
