/**
 * Modern captions: the mission's speech and messages as subtitle lines.
 *
 * Rare prints a character's line at the top of the screen as "Natalya: We
 * could escape through the\n           library windows.", hand-wrapped and
 * indented, and the mission's messages ("Covert modem installed.", "Picked
 * up a ...") one at a time at the bottom left. With Caption Style on Modern
 * (the default) both become caption lines in the style of a modern shooter:
 * bottom centre, on a dark soft box each, the speaker's name in its own
 * colour and the line in white after it, re-wrapped to the box, up to
 * COOPSUB_LINES at once with the newest lowest. Sound Captions (coop_access.c)
 * join the same stack, above the speech, in grey brackets.
 *
 * Story only (solo and co-op): versus and split-screen keep Rare's lines.
 * Caption Style = Classic brings Rare's back everywhere.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include "textrelated.h"
#include "fr.h"
#include "player.h"
#include "othermodemicrocode.h"
#include "bondview.h"
#include "coop.h"

extern void portConLogf(const char *fmt, ...);
extern s32 portCaptionsClassic(void);   /* video.c: [display] caption_style */

#define COOPSUB_LINES    3      /* captions on screen at once */
#define COOPSUB_TEXT     200
#define COOPSUB_NAME     24
#define COOPSUB_ROWS     4      /* wrapped rows of one caption */
#define COOPSUB_MIN_MS   2600
#define COOPSUB_CHAR_MS  55     /* reading time a character */
#define COOPSUB_MAX_MS   7000
#define COOPSUB_IN_MS    150
#define COOPSUB_OUT_MS   400
#define COOPSUB_FONT     ptrFontZurichBoldChars, ptrFontZurichBold   /* Rare's speech font: mixed case */
#define COOPSUB_SCALE    0.85f
#define COOPSUB_ROW_H    0x0C
#define COOPSUB_PAD_X    5
#define COOPSUB_PAD_Y    2
#define COOPSUB_GAP      3      /* between two captions' boxes */

typedef struct
{
    char name[COOPSUB_NAME];    /* "" for a message nobody says */
    char text[COOPSUB_TEXT];    /* one line, Rare's wrapping taken out */
    u32 colour;                 /* the name's, RGBA */
    u32 ms;                     /* wall clock when it came */
    u32 show;                   /* ms on screen */
} CoopSub;

static CoopSub sSub[COOPSUB_LINES];
static void coopSubtitlesHarness(void);
static s32 sSubCount;

static u32 coopSubNowMs(void)
{
    return (u32) (osGetTime() / 46875ull);
}

/* Modern captions take Rare's lines now: story (one player on this PC, not
 * versus) with Caption Style on Modern */
s32 coopSubtitlesOn(void)
{
    return !portCaptionsClassic() && getPlayerCount() == 1 && !coopVersus();
}

/* a speaker's colour: fixed for the leads, from a hash for the rest */
static u32 coopSubColour(const char *name)
{
    static const struct { const char *name; u32 colour; } fixed[] = {
        { "Bond", 0x8FC8FFFF },             /* steel blue */
        { "Natalya", 0xF7A1C4FF },          /* pink */
        { "Trevelyan", 0xFFB347FF },        /* amber */
        { "Janus", 0xFFB347FF },
        { "Ourumov", 0xFF7B6BFF },          /* red */
        { "Mishkin", 0xB9E07AFF },          /* green */
        { "Valentin", 0xE8D36AFF },         /* gold */
        { "Boris", 0x9EE6D8FF },            /* teal */
        { "Warning", 0xFF5A4FFF },          /* the mission's alerts */
        { "Caution", 0xFF5A4FFF },
        { "Mission Failure", 0xFF5A4FFF },
    };
    static const u32 others[] = { 0xC9B6FFFF, 0x9BE564FF, 0xFFD27FFF, 0x7FD8FFFF, 0xF5A97FFF };
    u32 h = 0;
    s32 i;

    for (i = 0; i < (s32) (sizeof(fixed) / sizeof(fixed[0])); i++)
    {
        if (strcmp(fixed[i].name, name) == 0)
        {
            return fixed[i].colour;
        }
    }
    for (i = 0; name[i] != '\0'; i++)
    {
        h = h * 31u + (u8) name[i];
    }
    return others[h % (sizeof(others) / sizeof(others[0]))];
}

/* "Natalya: We could escape through the\n   library windows.\n" into the
 * name and one line of text; a message without a speaker keeps name "" */
static void coopSubSplit(const char *mess, char *name, char *text)
{
    const char *colon = strchr(mess, ':');
    const char *p;
    s32 n = 0;
    s32 space = 0;
    s32 i;

    name[0] = '\0';
    if (colon != NULL && colon > mess && colon - mess < COOPSUB_NAME && colon[1] == ' ')
    {   /* a name is letters, spaces and dots: "Dr. Doak", "Mission Failure" */
        for (p = mess; p < colon; p++)
        {
            if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == ' ' || *p == '.' || *p == '\''))
            {
                break;
            }
        }
        if (p == colon && mess[0] >= 'A' && mess[0] <= 'Z')
        {
            memcpy(name, mess, colon - mess);
            name[colon - mess] = '\0';
            mess = colon + 1;
        }
    }
    for (i = 0; mess[i] != '\0' && n < COOPSUB_TEXT - 1; i++)
    {   /* Rare's line breaks and indents become one space */
        char c = mess[i];

        if (c == '\n' || c == '\r' || c == '\t' || c == ' ')
        {
            space = (n > 0);
            continue;
        }
        if (space && n < COOPSUB_TEXT - 2)
        {
            text[n++] = ' ';
        }
        space = 0;
        text[n++] = c;
    }
    text[n] = '\0';
}

/* bondview2.c hudmsgTopShow / hudmsgBottomShow: take Rare's line as a
 * caption; 0 when Modern captions are off and Rare should print it */
s32 coopSubtitleShow(const char *mess)
{
    u32 now = coopSubNowMs();
    char name[COOPSUB_NAME];
    char text[COOPSUB_TEXT];
    CoopSub *s;
    s32 i;

    if (!coopSubtitlesOn() || mess == NULL)
    {
        return 0;
    }
    coopSubSplit(mess, name, text);
    if (text[0] == '\0')
    {
        return 1;   /* Rare's blank lines */
    }
    for (i = 0; i < sSubCount; i++)
    {
        if (strcmp(sSub[i].name, name) == 0 && strcmp(sSub[i].text, text) == 0)
        {   /* said again while still up (a warning repeated): keep it up */
            sSub[i].ms = (now - sSub[i].ms < COOPSUB_IN_MS) ? sSub[i].ms : now - COOPSUB_IN_MS;
            return 1;
        }
    }
    if (sSubCount == COOPSUB_LINES)
    {
        memmove(&sSub[0], &sSub[1], sizeof(sSub[0]) * (COOPSUB_LINES - 1));
        sSubCount--;
    }
    s = &sSub[sSubCount++];
    strcpy(s->name, name);
    strcpy(s->text, text);
    s->colour = (name[0] != '\0') ? coopSubColour(name) : 0xFFFFFFFF;
    s->ms = now;
    s->show = COOPSUB_MIN_MS + (u32) strlen(text) * COOPSUB_CHAR_MS;
    s->show = (s->show > COOPSUB_MAX_MS) ? COOPSUB_MAX_MS : s->show;
    portConLogf("ge: captions: %s%s%s\n", name, name[0] ? ": " : "", text);
    return 1;
}

/* a mission's captions go with it */
void coopSubtitlesClear(void)
{
    sSubCount = 0;
}

static s32 coopSubWidth(const char *text)
{
    s32 th = 0, tw = 0;
    char buf[COOPSUB_TEXT + 2];

    snprintf(buf, sizeof(buf), "%s\n", text);
    portTextScale(COOPSUB_SCALE);
    textMeasure(&th, &tw, buf, COOPSUB_FONT, 0);
    portTextScale(1.0f);
    return tw;
}

/* plain text a touch smaller than Rare's, with a drop shadow */
static Gfx *coopSubPrint(Gfx *gdl, s32 x, s32 y, const char *text, u32 colour)
{
    char buf[COOPSUB_TEXT + 2];
    s32 sx = x + 1, sy = y + 1;

    snprintf(buf, sizeof(buf), "%s\n", text);
    portTextScale(COOPSUB_SCALE);
    gdl = textRender(gdl, &sx, &sy, buf, COOPSUB_FONT, (colour & 0xFF) * 3 / 4, (s32) viGetX(), viGetY(), 0, 0);
    gdl = textRender(gdl, &x, &y, buf, COOPSUB_FONT, colour, (s32) viGetX(), viGetY(), 0, 0);
    portTextScale(1.0f);
    return gdl;
}

/* wrap text into rows no wider than maxw, the first starting `first` in;
 * returns the rows and each one's width */
static s32 coopSubWrap(const char *text, s32 first, s32 maxw, char rows[][COOPSUB_TEXT], s32 *widths)
{
    s32 n = 0;
    const char *p = text;

    while (*p != '\0' && n < COOPSUB_ROWS)
    {
        s32 room = maxw - (n == 0 ? first : 0);
        s32 len = 0, fit = 0;

        while (p[len] != '\0')
        {
            char word[COOPSUB_TEXT];
            s32 end = len;

            while (p[end] != '\0' && p[end] != ' ')
            {
                end++;
            }
            memcpy(word, p, end);
            word[end] = '\0';
            if (fit > 0 && coopSubWidth(word) > room)
            {
                break;
            }
            fit = end;
            len = (p[end] == ' ') ? end + 1 : end;
        }
        if (fit == 0)
        {
            break;
        }
        memcpy(rows[n], p, fit);
        rows[n][fit] = '\0';
        if (n == COOPSUB_ROWS - 1 && p[len] != '\0' && fit < COOPSUB_TEXT - 4)
        {
            strcpy(rows[n] + fit, "...");
        }
        widths[n] = coopSubWidth(rows[n]);
        n++;
        p += len;
    }
    return n;
}

static u32 coopSubAlpha(u32 colour, s32 a)
{
    return (colour & 0xFFFFFF00u) | (u32) (((colour & 0xFF) * a) / 0xFF);
}

/* one caption's box, bottom edge at `bottom`; returns its top */
static Gfx *coopSubBox(Gfx *gdl, s32 cx, s32 bottom, s32 maxw, const char *name, u32 ncolour, const char *text,
                       u32 tcolour, s32 a, s32 *top)
{
    char rows[COOPSUB_ROWS][COOPSUB_TEXT];
    s32 widths[COOPSUB_ROWS];
    char label[COOPSUB_NAME + 2];
    s32 namew = 0, w = 0, n, k, x0, y0;

    if (name[0] != '\0')
    {
        snprintf(label, sizeof(label), "%s:", name);
        namew = coopSubWidth(label) + 4;
    }
    n = coopSubWrap(text, namew, maxw, rows, widths);
    for (k = 0; k < n; k++)
    {
        s32 rw = widths[k] + (k == 0 ? namew : 0);

        w = (rw > w) ? rw : w;
    }
    x0 = cx - w / 2;
    y0 = bottom - COOPSUB_PAD_Y - n * COOPSUB_ROW_H;
    /* the soft box: a faint rim around a darker core */
    gdl = microcode_constructor_related_to_menus(gdl, x0 - COOPSUB_PAD_X - 1, y0 - COOPSUB_PAD_Y - 1,
                                                 x0 + w + COOPSUB_PAD_X + 1, bottom + 1, (u32) (a * 0x38 / 0xFF));
    gdl = microcode_constructor_related_to_menus(gdl, x0 - COOPSUB_PAD_X, y0 - COOPSUB_PAD_Y, x0 + w + COOPSUB_PAD_X,
                                                 bottom, (u32) (a * 0x88 / 0xFF));
    if (namew > 0)
    {
        gdl = coopSubPrint(gdl, x0, y0, label, coopSubAlpha(ncolour, a));
    }
    for (k = 0; k < n; k++)
    {
        gdl = coopSubPrint(gdl, x0 + (k == 0 ? namew : 0), y0 + k * COOPSUB_ROW_H, rows[k], coopSubAlpha(tcolour, a));
    }
    *top = y0 - COOPSUB_PAD_Y - 1;
    return gdl;
}

/* bondview2.c, after Rare's messages: the captions, bottom centre, newest
 * lowest, with Sound Captions above them */
Gfx *coopSubtitlesRender(Gfx *gdl)
{
    u32 now = coopSubNowMs();
    s32 cx = viGetViewLeft() + viGetViewWidth() / 2;
    s32 bottom = viGetViewTop() + viGetViewHeight() - 0x1C;
    s32 maxw = viGetViewWidth() * 3 / 4;
    char sound[4][48];
    s32 nsound;
    s32 i;

    coopSubtitlesHarness();
    while (sSubCount > 0 && now - sSub[0].ms >= sSub[0].show)
    {
        memmove(&sSub[0], &sSub[1], sizeof(sSub[0]) * (COOPSUB_LINES - 1));
        sSubCount--;
    }
    if (!coopSubtitlesOn() || g_CurrentPlayer == NULL || g_CurrentPlayer->mpmenuon || g_CurrentPlayer->hudmessoff
        || g_CurrentPlayer->watch_animation_state != 0)
    {
        return gdl;
    }
    nsound = coopCaptionSoundLines(sound, 4);
    if (sSubCount == 0 && nsound == 0)
    {
        return gdl;
    }
    gdl = coopTextScaleBegin(gdl, (f32) cx, (f32) bottom);
    gdl = microcode_constructor(gdl);
    for (i = sSubCount - 1; i >= 0; i--)
    {
        CoopSub *s = &sSub[i];
        u32 age = now - s->ms;
        s32 a = 0xFF;
        s32 top;

        if (age < COOPSUB_IN_MS)
        {
            a = (s32) (age * 0xFF / COOPSUB_IN_MS);
        }
        else if (age > s->show - COOPSUB_OUT_MS)
        {
            a = (s32) ((s->show - age) * 0xFF / COOPSUB_OUT_MS);
        }
        gdl = coopSubBox(gdl, cx, bottom, maxw, s->name, s->colour, s->text, 0xFFFFFFFF, a, &top);
        bottom = top - COOPSUB_GAP;
    }
    for (i = nsound - 1; i >= 0; i--)
    {
        s32 top;

        gdl = coopSubBox(gdl, cx, bottom, maxw, "", 0, sound[i], 0xC8C8C8FF, 0xE0, &top);
        bottom = top - COOPSUB_GAP;
    }
    gdl = combiner_bayer_lod_perspective(gdl);
    gdl = coopTextScaleEnd(gdl);
    return gdl;
}

/* harness: GE_SUBTEST=<s> shows a scripted exchange <s> s (wall clock) after
 * the first caption frame, one line every 1.6 s, for the recordings */
static void coopSubtitlesHarness(void)
{
    static const char *const lines[] = {
        "Natalya: We could escape through the\n           library windows.\n",
        "Bond: Ourumov stole it - he's a traitor.\n",
        "Covert modem installed.\n",
        "Mishkin: Do you need the black box\n           from the stolen helicopter?\n",
        "Warning: Civilian casualties high.\n",
        "Trevelyan: For England, James?\n",
    };
    static s32 at = -2, step;
    static u32 base;
    u32 now = coopSubNowMs();

    if (at == -2)
    {
        const char *e = getenv("GE_SUBTEST");

        at = (e != NULL) ? atoi(e) : -1;
        base = now;
    }
    if (at < 0 || step >= (s32) (sizeof(lines) / sizeof(lines[0])) || now - base < (u32) at * 1000u + (u32) step * 1600u)
    {
        return;
    }
    if (!coopSubtitleShow(lines[step]))
    {   /* Classic: where Rare prints it */
        if (strchr(lines[step], ':') != NULL)
        {
            hudmsgTopShow((char *) lines[step]);
        }
        else
        {
            hudmsgBottomShow((char *) lines[step]);
        }
    }
    step++;
}
