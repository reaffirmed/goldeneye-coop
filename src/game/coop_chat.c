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
 * objective, ...) show centred near the top for COOPTOAST_SHOW ticks.
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
#define COOPTOAST_FADE    40
#define COOPTOAST_MAX     3

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
    char text[80];
    s32 player;
    u32 tick;
} sToast[COOPTOAST_MAX];
static s32 sToastCount;

static u32 coopFeedColour(s32 player)
{
    static const u32 colours[COOP_MAX_PLAYERS] = { 0xFFFF80FF, 0x80E0FFFF, 0xFF90E0FF, 0x90FF90FF };

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
    s32 n = 0;

    for (; in != NULL && *in != '\0' && *in != '\n' && n < cap - 2; in++)
    {
        char c = *in;

        if ((u8) c >= 0x80 || (u8) c < 0x20)
        {
            c = '?';
        }
        else if (c >= 'a' && c <= 'z')
        {
            c -= 'a' - 'A';
        }
        out[n++] = c;
    }
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

void coopFeedToast(const char *text, s32 player)
{
    if (sToastCount == COOPTOAST_MAX)
    {
        memmove(&sToast[0], &sToast[1], sizeof(sToast[0]) * (COOPTOAST_MAX - 1));
        sToastCount--;
    }
    snprintf(sToast[sToastCount].text, sizeof(sToast[0].text), "%s", text);
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

static void coopChatTestTick(s32 inmission)
{
    static CoopChatTest mission = { "GE_COOP_TESTCHAT", -2 };
    static CoopChatTest lobby = { "GE_COOP_TESTLOBBYCHAT", -2 };

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

/* the chat lines, bottom left, newest lowest; and the toasts, centred near the top */
Gfx *coopFeedRender(Gfx *gdl)
{
    s32 left = viGetViewLeft() + 0x14;
    s32 bottom = viGetViewTop() + viGetViewHeight() - 0x34;
    s32 y;
    s32 i;
    s32 any = sTyping || sToastCount > 0;

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
    gdl = combiner_bayer_lod_perspective(gdl);
    gdl = portHudScaleEnd(gdl);
    if (hudAnchorsToEdges())
    {
        PORT_HUD_ANCHOR_OFF(gdl);
    }

    /* toasts */
    while (sToastCount > 0 && sNow - sToast[0].tick >= COOPTOAST_SHOW)
    {
        memmove(&sToast[0], &sToast[1], sizeof(sToast[0]) * (COOPTOAST_MAX - 1));
        sToastCount--;
    }
    if (sToastCount > 0)
    {   /* centred a little under the top; a toast's lines are split at '|' and each cut to fit */
        const s32 cx = viGetViewLeft() + viGetViewWidth() / 2;
        const s32 maxw = viGetViewWidth() - 0x20;

        gdl = microcode_constructor(gdl);
        y = viGetViewTop() + viGetViewHeight() * 3 / 8;
        for (i = 0; i < sToastCount; i++)
        {
            u32 age = sNow - sToast[i].tick;
            const char *at = sToast[i].text;
            s32 a = 0xFF;
            s32 line;

            if (age > COOPTOAST_SHOW - COOPTOAST_FADE)
            {
                a = (s32) ((COOPTOAST_SHOW - age) * 0xFF / COOPTOAST_FADE);
            }
            for (line = 0; line < 2 && *at != '\0'; line++)
            {
                char part[80];
                char text[100];
                const char *bar = strchr(at, '|');
                s32 n = (bar != NULL) ? (s32) (bar - at) : (s32) strlen(at);
                s32 th = 0, tw = 0;

                snprintf(part, sizeof(part), "%.*s", (int) n, at);
                at += n + (bar != NULL);
                coopFeedText(text, sizeof(text), part);
                coopFeedFit(text, maxw);
                textMeasure(&th, &tw, text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
                gdl = microcode_constructor_related_to_menus(gdl, cx - tw / 2 - 4, y - 2, cx + tw / 2 + 4, y + th + 1,
                                                             coopFeedAlpha(0x00000090, a));
                gdl = coopFeedPrint(gdl, cx - tw / 2, y, text,
                                    coopFeedAlpha(line == 0 ? coopFeedColour(sToast[i].player) : 0xFFFFFFFF, a));
                y += th + 3;
            }
            y += 6;
        }
        gdl = combiner_bayer_lod_perspective(gdl);
    }
    return gdl;
}
