/**
 * port_lang.c - in-game languages. Overview: lang/README.md.
 *
 * Two halves, both optional per language:
 *
 *  1. The game's own text. A language may name the translators' .xdelta
 *     patch for the stock (U) cart (lang/<code>/language.ini patch=). At
 *     start-up it is applied in memory to a copy of the player's own cart
 *     (port_vcdiff.c); the patched cart's text banks (L*E) are kept and
 *     served instead of the cart's (romdata.c romdataFilesApply), and its two
 *     Latin fonts are kept for the accented letters the translators drew over
 *     unused punctuation (language.ini glyphs=). Those bytes are recoded to
 *     Latin-1 as each bank loads, so '/' stays a slash everywhere and 0xFA
 *     draws the patch's u-acute (textrelated.c portFontGlyph).
 *
 *  2. The port's own text: lang/<code>/port.po, a gettext catalogue keyed by
 *     the English text. Strings are translated where they are drawn
 *     (textrelated.c calls portLangDraw on every textRender / textMeasure),
 *     so a fixed label needs no code change; text built with printf goes
 *     through portTr at its format string.
 *
 * Nothing here is sent to other players: each player sees their own language,
 * and co-op messages carry ids and numbers, never display text, so players
 * with different languages play together.
 *
 * NO SDL, no game headers: builds headless.
 */
#ifndef _WIN32
#define _FILE_OFFSET_BITS 64 /* see romdata.c: stat on 64-bit inode filesystems */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "port_lang.h"
#include "port_vcdiff.h"

extern void portConLogf(const char *fmt, ...);
extern const char *portRootPath(const char *name); /* main.c: <exe dir>/name, a static buffer */
extern const char *portLogPath(const char *name);  /* main.c: Data/Logs/name */
/* romdata.c: walk another image's file table / find a ROM segment */
extern int romdataImageFiles(const uint8_t *img, uint32_t size,
                             void (*cb)(const char *name, uint32_t ofs, uint32_t size, void *ctx), void *ctx);
extern int romdataSegRange(const char *name, uint32_t *ofs, uint32_t *size);

#ifdef _WIN32
#define LANG_SEP "\\"
#else
#define LANG_SEP "/"
#endif

/* ==== the installed languages ============================================ */

struct langinfo {
    char code[16];
    char name[48];     /* game encoding + '\n' (the menu row) */
};

static struct langinfo sLangs[PORT_LANG_MAX];
static int sLangCount;
static int sCurrent;   /* index in force this run */
static int sPending;   /* the setting (the next run's) */
static int sScanned;

/* language.ini of the language in force */
static char sPatchFile[128];
static char sStringsFile[128] = "port.po";
static uint8_t sGlyphSrc[256];   /* Latin-1 byte -> the patched font's ASCII glyph */
static uint8_t sRecode[256];     /* a translated bank's byte -> the game encoding */

#define LANG_BANKPATCH_MAX 16
static struct {
    char bank[16];
    uint32_t slot;
    char text[48];
} sBankPatches[LANG_BANKPATCH_MAX];
static int sBankPatchCount;

/* ASCII only, and our own: <ctype.h>'s would pull mingw's msvcrt isalpha
 * into the link, which clashes with the game's (src/str.c) */
static int langLower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static char *langTrim(char *s)
{
    size_t n;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    n = strlen(s);
    while (n > 0 && strchr(" \t\r\n", s[n - 1]) != NULL) {
        n--;
    }
    s[n] = '\0';
    return s;
}

static int langIsDir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static FILE *langOpen(const char *code, const char *file, const char *mode)
{
    char p[1400];
    snprintf(p, sizeof(p), "%s", portRootPath("lang"));
    snprintf(p + strlen(p), sizeof(p) - strlen(p), LANG_SEP "%s" LANG_SEP "%s", code, file);
    return fopen(p, mode);
}

/* one UTF-8 sequence -> a code point; advances *s (a bad byte reads as itself) */
static unsigned langUtf8Next(const unsigned char **s)
{
    const unsigned char *p = *s;
    unsigned c = p[0];
    int n, i;

    if (c < 0x80) {
        *s = p + 1;
        return c;
    }
    n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    if (n == 0) {
        *s = p + 1;
        return c; /* a stray continuation byte: Latin-1 already? take it as is */
    }
    c &= (0x3F >> n);
    for (i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *s = p + 1;
            return p[0];
        }
        c = (c << 6) | (p[i] & 0x3F);
    }
    *s = p + n + 1;
    return c;
}

void portLangUtf8ToGame(const char *src, char *dst, size_t n)
{
    const unsigned char *s = (const unsigned char *) src;
    size_t o = 0;

    if (n == 0) {
        return;
    }
    while (*s != '\0' && o + 1 < n) {
        unsigned c = langUtf8Next(&s);
        const char *rep = NULL;
        char one[2] = { 0, 0 };

        if (c == 0xA0) {
            c = ' ';
        }
        if (c < 0x100 && (c < 0x80 || c >= 0xA0)) {
            one[0] = (char) c;
            rep = one;
        } else if (c == 0x2018 || c == 0x2019) {
            rep = "'";
        } else if (c == 0x201C || c == 0x201D) {
            rep = "\"";
        } else if (c == 0x2013 || c == 0x2014) {
            rep = "-";
        } else if (c == 0x2026) {
            rep = "...";
        } else {
            rep = "?";
        }
        while (*rep != '\0' && o + 1 < n) {
            dst[o++] = *rep++;
        }
    }
    dst[o] = '\0';
}

/* read lang/<code>/language.ini: name= only (full = 0), or everything (full = 1) */
static int langReadIni(const char *code, char *name, size_t namen, int full)
{
    FILE *f = langOpen(code, "language.ini", "r");
    char line[512];

    if (f == NULL) {
        return 0;
    }
    if (name != NULL) {
        snprintf(name, namen, "%s\n", code);
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *s = langTrim(line);
        char *eq, *k, *v;
        if ((unsigned char) s[0] == 0xEF && (unsigned char) s[1] == 0xBB && (unsigned char) s[2] == 0xBF) {
            s += 3; /* a UTF-8 byte-order mark */
        }
        if (s[0] == '\0' || s[0] == ';' || s[0] == '#' || s[0] == '[') {
            continue;
        }
        eq = strchr(s, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        k = langTrim(s);
        v = langTrim(eq + 1);
        if (strcmp(k, "name") == 0 && name != NULL) {
            char g[64];
            portLangUtf8ToGame(v, g, sizeof(g));
            snprintf(name, namen, "%s\n", g);
        }
        if (!full) {
            continue;
        }
        if (strcmp(k, "patch") == 0) {
            snprintf(sPatchFile, sizeof(sPatchFile), "%s", v);
        } else if (strcmp(k, "strings") == 0) {
            snprintf(sStringsFile, sizeof(sStringsFile), "%s", v);
        } else if (strcmp(k, "glyphs") == 0) {
            /* pairs: an ASCII byte the patch drew over, then the letter it is now */
            const unsigned char *p = (const unsigned char *) v;
            while (*p != '\0') {
                unsigned a, l;
                while (*p == ' ' || *p == '\t') {
                    p++;
                }
                if (*p == '\0') {
                    break;
                }
                a = *p++;
                if (*p == '\0') {
                    break;
                }
                l = langUtf8Next(&p);
                if (a > 0x20 && a < 0x7F && l >= 0xA0 && l <= 0xFF) {
                    sGlyphSrc[l] = (uint8_t) a;
                    sRecode[a] = (uint8_t) l;
                } else {
                    portConLogf("ge: lang: %s: glyphs= pair 0x%02X -> U+%04X ignored (ASCII 0x21..0x7E onto Latin-1 only)\n",
                                code, a, l);
                }
            }
        } else if (strncmp(k, "bank.", 5) == 0 && sBankPatchCount < LANG_BANKPATCH_MAX) {
            /* bank.<name>.<slot> = text: a port relabel in this language */
            char *dot = strrchr(k + 5, '.');
            if (dot != NULL && dot > k + 5) {
                char g[48];
                *dot = '\0';
                snprintf(sBankPatches[sBankPatchCount].bank, sizeof(sBankPatches[0].bank), "%s", k + 5);
                sBankPatches[sBankPatchCount].slot = (uint32_t) strtoul(dot + 1, NULL, 10);
                portLangUtf8ToGame(v, g, sizeof(g));
                snprintf(sBankPatches[sBankPatchCount].text, sizeof(sBankPatches[0].text), "%s\n", g);
                sBankPatchCount++;
            }
        }
    }
    fclose(f);
    return 1;
}

static int langCmpCode(const void *a, const void *b)
{
    return strcmp(((const struct langinfo *) a)->code, ((const struct langinfo *) b)->code);
}

static void langScan(void)
{
    char dir[1300];
    DIR *d;
    struct dirent *de;

    if (sScanned) {
        return;
    }
    sScanned = 1;
    snprintf(sLangs[0].code, sizeof(sLangs[0].code), "en");
    snprintf(sLangs[0].name, sizeof(sLangs[0].name), "English\n");
    sLangCount = 1;
    snprintf(dir, sizeof(dir), "%s", portRootPath("lang"));
    d = opendir(dir);
    if (d == NULL) {
        portConLogf("ge: lang: no lang folder at %s - English only\n", dir);
        return;
    }
    while ((de = readdir(d)) != NULL && sLangCount < PORT_LANG_MAX) {
        char sub[1600];
        struct langinfo *li = &sLangs[sLangCount];
        if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof(li->code) || strcmp(de->d_name, "en") == 0) {
            continue;
        }
        snprintf(sub, sizeof(sub), "%s" LANG_SEP "%s", dir, de->d_name);
        if (!langIsDir(sub)) {
            continue;
        }
        snprintf(li->code, sizeof(li->code), "%s", de->d_name);
        if (langReadIni(li->code, li->name, sizeof(li->name), 0)) {
            sLangCount++;
        }
    }
    closedir(d);
    qsort(sLangs + 1, (size_t) (sLangCount - 1), sizeof(sLangs[0]), langCmpCode);
}

static int langFind(const char *code)
{
    int i;
    if (strcmp(code, "jp") == 0 || strcmp(code, "ja") == 0) {
        return -2; /* the cart's own Japanese banks: romdata.c romdataLanguageJp */
    }
    for (i = 0; i < sLangCount; i++) {
        if (strcmp(sLangs[i].code, code) == 0) {
            return i;
        }
    }
    return -1;
}

int portLangCount(void)
{
    langScan();
    return sLangCount;
}

const char *portLangNameAt(int i)
{
    langScan();
    return (i >= 0 && i < sLangCount) ? sLangs[i].name : "?\n";
}

const char *portLangCodeAt(int i)
{
    langScan();
    return (i >= 0 && i < sLangCount) ? sLangs[i].code : "en";
}

const char *portLangCode(void)
{
    return sLangs[sCurrent].code[0] != '\0' ? sLangs[sCurrent].code : "en";
}

int portLangRestartPending(void)
{
    return sPending != sCurrent;
}

int portLangIsIniKey(const char *key)
{
    return strcmp(key, "language") == 0;
}

int portLangOptionGet(const char *key, int32_t *out)
{
    if (strcmp(key, "language") != 0) {
        return 0;
    }
    langScan();
    *out = sPending;
    return 1;
}

int portLangOptionSet(const char *key, int32_t value)
{
    if (strcmp(key, "language") != 0) {
        return 0;
    }
    langScan();
    if (value < 0 || value >= sLangCount) {
        return 0;
    }
    if (value != sPending) {
        portConLogf("ge: lang: language set to %s (%s); it takes effect the next time the game starts\n",
                    sLangs[value].code, value == sCurrent ? "the one in force" : "restart to switch");
    }
    sPending = (int) value;
    return 1;
}

/* ==== the catalogue (port.po) ============================================ */

struct trentry {
    const char *key;     /* English, no trailing '\n' */
    const char *tr;      /* game encoding, no trailing '\n' */
    const char *trnl;    /* game encoding + '\n' */
    const char *utf8;    /* as written, no trailing '\n' */
    const char *upper;   /* tr in capitals (lazily), and + '\n' */
    const char *uppernl;
    uint32_t hash;
    uint32_t chash;      /* of the key folded to lower case */
};

static struct trentry *sEntries;
static int sEntryCount;
static int *sTable;       /* exact: index + 1, 0 = empty */
static int *sCaseTable;   /* case-folded */
static uint32_t sTableMask;
static char *sPool;       /* every translated string lives here (portLangDraw's "already translated" test) */
static size_t sPoolUsed, sPoolCap;

static uint32_t langHash(const char *s, size_t n, int fold)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char) s[i];
        if (fold && c >= 'A' && c <= 'Z') {
            c = (unsigned char) (c - 'A' + 'a');
        }
        h = (h ^ c) * 16777619u;
    }
    return h;
}

static const char *langPoolAdd(const char *s, size_t n, int nl)
{
    char *p;
    if (sPoolUsed + n + 2 > sPoolCap) {
        return NULL;
    }
    p = sPool + sPoolUsed;
    memcpy(p, s, n);
    if (nl) {
        p[n++] = '\n';
    }
    p[n] = '\0';
    sPoolUsed += n + 1;
    return p;
}

static int langInPool(const char *s)
{
    return sPool != NULL && s >= sPool && s < sPool + sPoolCap;
}

/* the printf conversions of s, as text ("%d|%s|"), for the format check */
static void langFormatSig(const char *s, char *sig, size_t n)
{
    size_t o = 0;
    sig[0] = '\0';
    while (*s != '\0') {
        if (*s++ != '%') {
            continue;
        }
        if (*s == '%') {
            s++;
            continue;
        }
        while (*s != '\0' && strchr("-+ #0123456789.*hlLqjzt", *s) != NULL) {
            if (o + 2 < n) sig[o++] = *s;
            s++;
        }
        if (*s != '\0') {
            if (o + 2 < n) sig[o++] = *s;
            s++;
        }
        if (o + 1 < n) sig[o++] = '|';
        sig[o] = '\0';
    }
}

/* C escapes inside a PO string literal; appends to buf */
static void langPoUnquote(const char *s, char *buf, size_t n)
{
    size_t o = strlen(buf);
    s = strchr(s, '"');
    if (s == NULL) {
        return;
    }
    s++;
    while (*s != '\0' && *s != '"' && o + 1 < n) {
        char c = *s++;
        if (c == '\\' && *s != '\0') {
            c = *s++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '0': c = '\0'; break;
            default: break; /* \" \\ and anything else: the character */
            }
            if (c == '\0') {
                break;
            }
        }
        buf[o++] = c;
    }
    buf[o] = '\0';
}

#define LANG_STR_MAX 2048

static int sBadFormats;

static void langAddEntry(const char *id, const char *str)
{
    struct trentry *e;
    size_t idn = strlen(id), sn = strlen(str);
    char game[LANG_STR_MAX];
    char sa[128], sb[128];
    size_t gn;

    if (idn > 0 && id[idn - 1] == '\n') {
        idn--;
    }
    if (sn > 0 && str[sn - 1] == '\n') {
        sn--;
    }
    if (idn == 0 || sn == 0) {
        return;
    }
    langFormatSig(id, sa, sizeof(sa));
    langFormatSig(str, sb, sizeof(sb));
    if (strcmp(sa, sb) != 0) {
        if (sBadFormats++ < 20) {
            portConLogf("ge: lang: \"%.60s\": the translation's %% conversions (%s) differ from the English (%s) - left in English\n",
                        id, sb, sa);
        }
        return;
    }
    {
        char tmp[LANG_STR_MAX];
        snprintf(tmp, sizeof(tmp), "%.*s", (int) sn, str);
        portLangUtf8ToGame(tmp, game, sizeof(game));
    }
    gn = strlen(game);
    e = &sEntries[sEntryCount];
    memset(e, 0, sizeof(*e));
    e->key = langPoolAdd(id, idn, 0);
    e->utf8 = langPoolAdd(str, sn, 0);
    e->tr = langPoolAdd(game, gn, 0);
    e->trnl = langPoolAdd(game, gn, 1);
    if (e->key == NULL || e->utf8 == NULL || e->tr == NULL || e->trnl == NULL) {
        return; /* pool full: counted when sized, cannot happen */
    }
    e->hash = langHash(id, idn, 0);
    e->chash = langHash(id, idn, 1);
    sEntryCount++;
}

static void langInsert(int *table, uint32_t h, int idx)
{
    uint32_t i = h & sTableMask;
    while (table[i] != 0) {
        i = (i + 1) & sTableMask;
    }
    table[i] = idx + 1;
}

static void langLoadCatalogue(const char *code)
{
    FILE *f = langOpen(code, sStringsFile, "rb");
    long len;
    char *text, *line, *next;
    static char id[LANG_STR_MAX], str[LANG_STR_MAX];
    int state = 0;   /* 0 none, 1 msgctxt, 2 msgid, 3 msgstr */
    int fuzzy = 0, have = 0, maxentries = 0, i;
    uint32_t cap;

    if (f == NULL) {
        portConLogf("ge: lang: %s: no %s - the port's own text stays English\n", code, sStringsFile);
        return;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (8L << 20)) {
        fclose(f);
        return;
    }
    text = (char *) malloc((size_t) len + 1);
    if (text == NULL || fread(text, 1, (size_t) len, f) != (size_t) len) {
        fclose(f);
        free(text);
        return;
    }
    fclose(f);
    text[len] = '\0';
    for (i = 0; i < len; i++) {
        if (strncmp(text + i, "msgid ", 6) == 0) {
            maxentries++;
        }
    }
    sEntries = (struct trentry *) calloc((size_t) maxentries + 1, sizeof(*sEntries));
    /* key + utf8 + tr + trnl + upper + uppernl: each at most the file's bytes, x6 */
    sPoolCap = (size_t) len * 6 + 64;
    sPool = (char *) malloc(sPoolCap);
    if (sEntries == NULL || sPool == NULL) {
        free(text);
        return;
    }

    id[0] = str[0] = '\0';
    for (line = text; line != NULL; line = next) {
        char *s;
        next = strchr(line, '\n');
        if (next != NULL) {
            *next++ = '\0';
        }
        s = langTrim(line);
        if (s[0] == '#') {
            if (s[1] == ',' && strstr(s, "fuzzy") != NULL) {
                fuzzy = 1;
            }
            continue;
        }
        if (strncmp(s, "msgctxt", 7) == 0 || strncmp(s, "msgid ", 6) == 0 || s[0] == '\0') {
            if (have && state == 3 && !fuzzy && id[0] != '\0') {
                langAddEntry(id, str);
            }
            if (s[0] == '\0' || strncmp(s, "msgctxt", 7) == 0) {
                have = 0;
                fuzzy = (s[0] != '\0') ? fuzzy : 0;
                state = (s[0] == '\0') ? 0 : 1;
                id[0] = str[0] = '\0';
                continue;
            }
        }
        if (strncmp(s, "msgid ", 6) == 0) {
            id[0] = str[0] = '\0';
            langPoUnquote(s + 5, id, sizeof(id));
            state = 2;
            have = 1;
        } else if (strncmp(s, "msgid_plural", 12) == 0) {
            state = 4; /* plurals: not used by the port */
        } else if (strncmp(s, "msgstr", 6) == 0) {
            if (state == 2) {
                langPoUnquote(s + 6, str, sizeof(str));
                state = 3;
            } else {
                state = 4;
            }
        } else if (s[0] == '"') {
            if (state == 2) {
                langPoUnquote(s, id, sizeof(id));
            } else if (state == 3) {
                langPoUnquote(s, str, sizeof(str));
            }
        }
    }
    if (have && state == 3 && !fuzzy && id[0] != '\0') {
        langAddEntry(id, str);
    }
    free(text);

    for (cap = 64; cap < (uint32_t) sEntryCount * 2 + 1; cap <<= 1) {
    }
    sTableMask = cap - 1;
    sTable = (int *) calloc(cap, sizeof(int));
    sCaseTable = (int *) calloc(cap, sizeof(int));
    if (sTable == NULL || sCaseTable == NULL) {
        sEntryCount = 0;
        return;
    }
    for (i = 0; i < sEntryCount; i++) {
        langInsert(sTable, sEntries[i].hash, i);
        langInsert(sCaseTable, sEntries[i].chash, i);
    }
    portConLogf("ge: lang: %s: %d translated strings from %s%s\n", code, sEntryCount, sStringsFile,
                sBadFormats ? " (some left in English: see above)" : "");
}

static const struct trentry *langLookup(const char *s, size_t n, int fold)
{
    uint32_t h, i;
    int *table = fold ? sCaseTable : sTable;

    if (sEntryCount == 0 || table == NULL) {
        return NULL;
    }
    h = langHash(s, n, fold);
    for (i = h & sTableMask; table[i] != 0; i = (i + 1) & sTableMask) {
        const struct trentry *e = &sEntries[table[i] - 1];
        if ((fold ? e->chash : e->hash) == h && strlen(e->key) == n) {
            if (fold) {
                size_t k;
                for (k = 0; k < n; k++) {
                    if (langLower((unsigned char) e->key[k]) != langLower((unsigned char) s[k])) {
                        break;
                    }
                }
                if (k == n) {
                    return e;
                }
            } else if (memcmp(e->key, s, n) == 0) {
                return e;
            }
        }
    }
    return NULL;
}

/* Latin-1 capitals: the vowels' accents drop, as the translators did in
 * their own capitals ("ELEGIR MISION"); the fonts have no capital glyphs
 * for them (textrelated.c draws the plain letter), and n-tilde stays */
static unsigned char langUpper(unsigned char c)
{
    if (c >= 'a' && c <= 'z') {
        return (unsigned char) (c - 'a' + 'A');
    }
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) {
        return (unsigned char) (c - 0x20);
    }
    return c;
}

static void langMakeUpper(struct trentry *e)
{
    char buf[LANG_STR_MAX];
    size_t i, n = strlen(e->tr);
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    for (i = 0; i < n; i++) {
        buf[i] = (char) langUpper((unsigned char) e->tr[i]);
    }
    e->upper = langPoolAdd(buf, n, 0);
    e->uppernl = langPoolAdd(buf, n, 1);
    if (e->upper == NULL || e->uppernl == NULL) {
        e->upper = e->tr;
        e->uppernl = e->trnl;
    }
}

/* a missed string, once (GE_LANG_MISSING=1) */
static int sMissingOn = -1;
static void langNoteMissing(const char *s, size_t n)
{
    static uint32_t seen[4096];
    static int nseen;
    static FILE *out;
    uint32_t h;
    int i, letters = 0;
    size_t k;

    if (sMissingOn < 0) {
        const char *e = getenv("GE_LANG_MISSING");
        sMissingOn = (e != NULL && e[0] != '\0' && e[0] != '0');
    }
    if (!sMissingOn || nseen >= (int) (sizeof(seen) / sizeof(seen[0]))) {
        return;
    }
    for (k = 0; k < n; k++) {
        if ((unsigned char) s[k] >= 0x80) {
            return;   /* already in the language (the patch's text, a translation being wrapped) */
        }
        if (langLower((unsigned char) s[k]) >= 'a' && langLower((unsigned char) s[k]) <= 'z') {
            letters++;
        }
    }
    if (letters < 2) {
        return;
    }
    h = langHash(s, n, 0);
    for (i = 0; i < nseen; i++) {
        if (seen[i] == h) {
            return;
        }
    }
    seen[nseen++] = h;
    if (out == NULL) {
        out = fopen(portLogPath("lang-missing.txt"), "w");
        if (out == NULL) {
            sMissingOn = 0;
            return;
        }
    }
    fputs("msgid \"", out);
    for (k = 0; k < n; k++) {
        unsigned char c = (unsigned char) s[k];
        if (c == '\n') fputs("\\n", out);
        else if (c == '"' || c == '\\') { fputc('\\', out); fputc(c, out); }
        else fputc(c, out);
    }
    fputs("\"\nmsgstr \"\"\n\n", out);
    fflush(out);
}

static const char *langTranslate(const char *en, int draw, int utf8)
{
    size_t n;
    int nl;
    const struct trentry *e;

    if (en == NULL || sEntryCount == 0 || en[0] == '\0') {
        return en;
    }
    if (langInPool(en)) {
        return en;
    }
    n = strlen(en);
    nl = (n > 0 && en[n - 1] == '\n');
    if (nl) {
        n--;
    }
    if (n == 0) {
        return en;
    }
    e = langLookup(en, n, 0);
    if (e != NULL) {
        return utf8 ? e->utf8 : (nl ? e->trnl : e->tr);
    }
    if (!utf8) {
        /* an all-capitals rendering of a catalogue string ("PLAY" for "Play") */
        size_t k;
        int upper = 0, lower = 0;
        for (k = 0; k < n; k++) {
            unsigned char c = (unsigned char) en[k];
            if (c >= 'A' && c <= 'Z') upper++;
            if (c >= 'a' && c <= 'z') lower++;
        }
        if (upper > 0 && lower == 0) {
            struct trentry *ce = (struct trentry *) langLookup(en, n, 1);
            if (ce != NULL) {
                if (ce->upper == NULL) {
                    langMakeUpper(ce);
                }
                return nl ? ce->uppernl : ce->upper;
            }
        }
    }
    if (draw) {
        langNoteMissing(en, n);
    }
    return en;
}

const char *portTr(const char *en)
{
    return langTranslate(en, 0, 0);
}

const char *portTrUtf8(const char *en)
{
    return langTranslate(en, 0, 1);
}

const char *portLangDraw(const char *s)
{
    if (sEntryCount == 0) {
        return s;
    }
    return langTranslate(s, 1, 0);
}

/* ==== picking the language ================================================ */

static void langReadSetting(char *code, size_t n)
{
    FILE *f = fopen(portRootPath("midas.ini"), "r");
    char line[512];
    int indisplay = 0;

    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *s = langTrim(line);
        char *eq;
        if (s[0] == '[') {
            indisplay = (strncmp(s, "[display]", 9) == 0);
            continue;
        }
        if (!indisplay || s[0] == ';' || s[0] == '#') {
            continue;
        }
        eq = strchr(s, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        if (strcmp(langTrim(s), "language") == 0) {
            snprintf(code, n, "%s", langTrim(eq + 1));
        }
    }
    fclose(f);
}

static int sActive;

int portLangActive(void)
{
    return sActive;
}

void portLangInit(int argc, char **argv)
{
    char code[32] = "";
    const char *why = "default";
    const char *e;
    int i, idx;

    for (i = 0; i < 256; i++) {
        sRecode[i] = (uint8_t) i;
    }
    memset(sGlyphSrc, 0, sizeof(sGlyphSrc));
    langScan();

    langReadSetting(code, sizeof(code));
    if (code[0] != '\0') {
        why = "midas.ini [display] language";
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
            snprintf(code, sizeof(code), "%s", argv[i + 1]);
            why = "--lang";
        } else if (strncmp(argv[i], "--lang=", 7) == 0) {
            snprintf(code, sizeof(code), "%s", argv[i] + 7);
            why = "--lang";
        }
    }
    e = getenv("GE_LANG");
    if (e != NULL && e[0] != '\0') {
        snprintf(code, sizeof(code), "%s", e);
        why = "GE_LANG";
    }
    for (i = 0; code[i] != '\0'; i++) {
        code[i] = (char) langLower((unsigned char) code[i]);
    }
    idx = code[0] != '\0' ? langFind(code) : 0;
    if (idx == -2) {
        portConLogf("ge: lang: Japanese (%s): the cart's own Japanese text; the port's own text stays English\n", why);
        idx = 0;
    } else if (idx < 0) {
        portConLogf("ge: lang: \"%s\" (%s) is not installed (no lang/%s/language.ini) - English\n", code, why, code);
        idx = 0;
    }
    sCurrent = sPending = idx;
    {
        char list[PORT_LANG_MAX * 18] = "";
        for (i = 0; i < sLangCount; i++) {
            snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", i ? ", " : "", sLangs[i].code);
        }
        portConLogf("ge: lang: %d installed (%s); this run: %s (%s)\n", sLangCount, list, sLangs[idx].code,
                    idx == 0 && code[0] == '\0' ? "default" : why);
    }
    if (idx == 0) {
        return;
    }
    langReadIni(sLangs[idx].code, NULL, 0, 1);
    langLoadCatalogue(sLangs[idx].code);
    sActive = 1;
}

/* ==== the patched cart ===================================================== */

#define LANG_BANK_MAX 64
static struct {
    char name[16];
    uint8_t *data;
    uint32_t size;
} sBanks[LANG_BANK_MAX];
static int sBankCount;
static uint8_t *sFontSeg[2];
static uint32_t sFontSize[2];

struct bankctx {
    const uint8_t *img;
    uint32_t size;
};

static void langKeepBank(const char *name, uint32_t ofs, uint32_t size, void *vctx)
{
    struct bankctx *ctx = (struct bankctx *) vctx;
    size_t l = strlen(name);

    /* the English text banks, L<name>E: what the patch translated */
    if (name[0] != 'L' || l < 3 || l >= sizeof(sBanks[0].name) || name[l - 1] != 'E' || strchr(name, '/') != NULL) {
        return;
    }
    if (size == 0 || size > ctx->size || ofs > ctx->size - size || sBankCount >= LANG_BANK_MAX) {
        return;
    }
    sBanks[sBankCount].data = (uint8_t *) malloc(size);
    if (sBanks[sBankCount].data == NULL) {
        return;
    }
    memcpy(sBanks[sBankCount].data, ctx->img + ofs, size);
    sBanks[sBankCount].size = size;
    snprintf(sBanks[sBankCount].name, sizeof(sBanks[0].name), "%s", name);
    sBankCount++;
}

/* the same host layout segments.c portFontFromRom makes: the kerning table
 * and the fontchar table become native words, the glyph bytes stay */
static uint8_t *langFontHost(const uint8_t *rom, uint32_t size)
{
    const uint32_t nativewords = 169 + 94 * 6;
    uint8_t *dst;
    uint32_t w;

    if (size < nativewords * 4) {
        return NULL;
    }
    dst = (uint8_t *) malloc(size);
    if (dst == NULL) {
        return NULL;
    }
    memcpy(dst, rom, size);
    for (w = 0; w < nativewords; w++) {
        uint32_t v = ((uint32_t) rom[w * 4] << 24) | ((uint32_t) rom[w * 4 + 1] << 16) |
                     ((uint32_t) rom[w * 4 + 2] << 8) | rom[w * 4 + 3];
        memcpy(dst + w * 4, &v, 4);
    }
    return dst;
}

void portLangRomLoaded(const uint8_t *rom, size_t size)
{
    FILE *f;
    long len;
    uint8_t *patch, *img = NULL;
    size_t imgsize = 0;
    char err[200];
    struct bankctx ctx;
    static const char *const segs[2] = { "fontbankgothic", "fontzurichbold" };
    int i;

    if (!sActive || sPatchFile[0] == '\0' || rom == NULL) {
        return;
    }
    f = langOpen(sLangs[sCurrent].code, sPatchFile, "rb");
    if (f == NULL) {
        portConLogf("ge: lang: %s: patch %s is missing - the game's own text stays English\n", sLangs[sCurrent].code,
                    sPatchFile);
        return;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    patch = (len > 0 && len < (32L << 20)) ? (uint8_t *) malloc((size_t) len) : NULL;
    if (patch == NULL || fread(patch, 1, (size_t) len, f) != (size_t) len) {
        fclose(f);
        free(patch);
        portConLogf("ge: lang: %s: cannot read %s\n", sLangs[sCurrent].code, sPatchFile);
        return;
    }
    fclose(f);
    if (!portVcdiffApply(rom, size, patch, (size_t) len, &img, &imgsize, err, sizeof(err))) {
        free(patch);
        portConLogf("ge: lang: %s: %s does not apply to this cart: %s - the game's own text stays English\n",
                    sLangs[sCurrent].code, sPatchFile, err);
        return;
    }
    free(patch);

    ctx.img = img;
    ctx.size = (uint32_t) imgsize;
    if (!romdataImageFiles(img, (uint32_t) imgsize, langKeepBank, &ctx)) {
        portConLogf("ge: lang: %s: the patched cart's file table did not parse - the game's own text stays English\n",
                    sLangs[sCurrent].code);
    }
    for (i = 0; i < 2; i++) {
        uint32_t ofs, sz;
        if (romdataSegRange(segs[i], &ofs, &sz) && sz <= imgsize && ofs <= imgsize - sz) {
            sFontSeg[i] = langFontHost(img + ofs, sz);
            sFontSize[i] = sFontSeg[i] != NULL ? sz : 0;
        }
    }
    free(img);
    {
        int glyphs = 0;
        for (i = 0xA0; i < 0x100; i++) {
            glyphs += sGlyphSrc[i] != 0;
        }
        portConLogf("ge: lang: %s: %s applied to the cart in memory: %d text banks, %d extra letters in the fonts\n",
                    sLangs[sCurrent].code, sPatchFile, sBankCount, (sFontSeg[0] != NULL && sFontSeg[1] != NULL) ? glyphs : 0);
    }
}

const uint8_t *portLangBank(const char *name, uint32_t *size)
{
    int i;
    for (i = 0; i < sBankCount; i++) {
        if (strcmp(sBanks[i].name, name) == 0) {
            *size = sBanks[i].size;
            return sBanks[i].data;
        }
    }
    return NULL;
}

void portLangBankRecode(uint8_t *strings, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        strings[i] = sRecode[strings[i]];
    }
}

const char *portLangBankPatch(const char *bank, uint32_t slot)
{
    int i;
    for (i = 0; i < sBankPatchCount; i++) {
        if (sBankPatches[i].slot == slot && strcmp(sBankPatches[i].bank, bank) == 0) {
            return sBankPatches[i].text;
        }
    }
    return NULL;
}

const uint8_t *portLangFontSeg(int which, uint32_t *size)
{
    if (which < 0 || which > 1 || sFontSeg[which] == NULL) {
        return NULL;
    }
    *size = sFontSize[which];
    return sFontSeg[which];
}

uint8_t portLangGlyphSource(uint8_t latin1)
{
    return sGlyphSrc[latin1];
}

uint8_t portLangAsciiFallback(uint8_t c)
{
    /* 0xA0..0xFF -> the plain letter */
    static const char map[] =
        " !c$oY|S\"ca<--R-"   /* A0..AF */
        "o+23'uP.,1o>   ?"    /* B0..BF */
        "AAAAAAACEEEEIIII"    /* C0..CF */
        "DNOOOOOxOUUUUYTs"    /* D0..DF */
        "aaaaaaaceeeeiiii"    /* E0..EF */
        "dnooooo/ouuuuyty";   /* F0..FF */
    if (c < 0xA0) {
        return c;
    }
    return (uint8_t) map[c - 0xA0];
}
