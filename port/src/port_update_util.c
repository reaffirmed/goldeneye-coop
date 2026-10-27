/**
 * port_update_util.c - what the updater (port_update.c) needs that touches no
 * game state, file or network: reading GitHub's release JSON, comparing
 * versions, the rule for which files an update may write, SHA-256, CRC-32,
 * inflate and a reader for a zip held in memory.
 *
 * Plain C with no SDL or OS calls, so coop/ci/update_test.c builds it alone.
 * The inflate follows RFC 1951 the way Mark Adler's puff.c lays it out
 * (canonical Huffman tables decoded a bit at a time); it is only ever fed
 * the one zip of a release, so speed doesn't matter.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_update.h"

/* the game's include/ comes first and its string.h is the N64 one, which
 * lacks these: declared here, or they would be ints under -w */
extern int strcmp(const char *a, const char *b);
extern int strncmp(const char *a, const char *b, size_t n);
extern char *strstr(const char *s, const char *find);
extern size_t strcspn(const char *s, const char *reject);
extern void *memchr(const void *s, int c, size_t n);

/* our own: the game defines isdigit and isalpha itself (src/str.c), and
 * pulling the C library's in as well breaks the Windows link */
static int updDigit(int c) { return c >= '0' && c <= '9'; }
static int updAlpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int updLower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }
static int updXDigit(int c) { return updDigit(c) || (updLower(c) >= 'a' && updLower(c) <= 'f'); }

/* ---- release JSON ----------------------------------------------------------- */

struct Json {
    const char *p;
    const char *end;
    int err;
};

static void jsonSpace(struct Json *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n'))
    {
        j->p++;
    }
}

static int jsonPeek(struct Json *j)
{
    jsonSpace(j);
    return (j->p < j->end) ? (unsigned char) *j->p : -1;
}

static void jsonExpect(struct Json *j, char c)
{
    if (jsonPeek(j) != c)
    {
        j->err = 1;
        return;
    }
    j->p++;
}

/* a string into out (cut to fit; \u escapes past ASCII become '?') */
static void jsonString(struct Json *j, char *out, size_t cap)
{
    size_t n = 0;

    jsonExpect(j, '"');
    while (!j->err)
    {
        int c;

        if (j->p >= j->end)
        {
            j->err = 1;
            break;
        }
        c = (unsigned char) *j->p++;
        if (c == '"')
        {
            break;
        }
        if (c == '\\')
        {
            if (j->p >= j->end)
            {
                j->err = 1;
                break;
            }
            c = (unsigned char) *j->p++;
            switch (c)
            {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u':
                {
                    unsigned v = 0;
                    int k;

                    for (k = 0; k < 4; k++)
                    {
                        if (j->p >= j->end || !updXDigit((unsigned char) *j->p))
                        {
                            j->err = 1;
                            return;
                        }
                        v = v * 16 + (unsigned) (updDigit((unsigned char) *j->p) ? *j->p - '0' : (updLower((unsigned char) *j->p) - 'a' + 10));
                        j->p++;
                    }
                    c = (v >= 0x20 && v < 0x7F) ? (int) v : '?';
                    break;
                }
                default: break;   /* \" \\ \/ */
            }
        }
        if (out != NULL && n + 1 < cap)
        {
            out[n++] = (char) c;
        }
    }
    if (out != NULL && cap > 0)
    {
        out[n] = '\0';
    }
}

static long jsonNumber(struct Json *j)
{
    long v = 0;
    int neg = 0;

    jsonSpace(j);
    if (j->p < j->end && *j->p == '-')
    {
        neg = 1;
        j->p++;
    }
    if (j->p >= j->end || !updDigit((unsigned char) *j->p))
    {
        j->err = 1;
        return 0;
    }
    while (j->p < j->end && (updDigit((unsigned char) *j->p) || *j->p == '.' || *j->p == 'e' || *j->p == 'E' || *j->p == '+' || *j->p == '-'))
    {
        if (updDigit((unsigned char) *j->p) && v < 100000000000L / 10)
        {
            v = v * 10 + (*j->p - '0');
        }
        j->p++;
    }
    return neg ? -v : v;
}

static void jsonSkip(struct Json *j, int depth)
{
    int c = jsonPeek(j);

    if (depth > 64)
    {
        j->err = 1;
        return;
    }
    if (c == '"')
    {
        jsonString(j, NULL, 0);
    }
    else if (c == '{' || c == '[')
    {
        const char close = (c == '{') ? '}' : ']';

        j->p++;
        if (jsonPeek(j) == close)
        {
            j->p++;
            return;
        }
        while (!j->err)
        {
            if (c == '{')
            {
                jsonString(j, NULL, 0);
                jsonExpect(j, ':');
            }
            jsonSkip(j, depth + 1);
            if (jsonPeek(j) == ',')
            {
                j->p++;
                continue;
            }
            jsonExpect(j, close);
            break;
        }
    }
    else if (c == '-' || (c >= '0' && c <= '9'))
    {
        (void) jsonNumber(j);
    }
    else if (c == 't' || c == 'f' || c == 'n')
    {   /* true, false, null */
        while (j->p < j->end && updAlpha((unsigned char) *j->p))
        {
            j->p++;
        }
    }
    else
    {
        j->err = 1;
    }
}

static void jsonAsset(struct Json *j, struct PortUpdAsset *a)
{
    char key[32];
    char digest[96];

    memset(a, 0, sizeof(*a));
    jsonExpect(j, '{');
    if (jsonPeek(j) == '}')
    {
        j->p++;
        return;
    }
    while (!j->err)
    {
        jsonString(j, key, sizeof(key));
        jsonExpect(j, ':');
        if (strcmp(key, "name") == 0 && jsonPeek(j) == '"')
        {
            jsonString(j, a->name, sizeof(a->name));
        }
        else if (strcmp(key, "browser_download_url") == 0 && jsonPeek(j) == '"')
        {
            jsonString(j, a->url, sizeof(a->url));
        }
        else if (strcmp(key, "digest") == 0 && jsonPeek(j) == '"')
        {   /* "sha256:<64 hex>" (GitHub lists it for every asset uploaded since mid 2025) */
            jsonString(j, digest, sizeof(digest));
            if (strncmp(digest, "sha256:", 7) == 0 && strlen(digest + 7) == 64)
            {
                int k;

                for (k = 0; k < 64; k++)
                {
                    a->sha256[k] = (char) updLower((unsigned char) digest[7 + k]);
                }
                a->sha256[64] = '\0';
            }
        }
        else if (strcmp(key, "size") == 0 && jsonPeek(j) != '"')
        {
            a->size = jsonNumber(j);
        }
        else
        {
            jsonSkip(j, 2);
        }
        if (jsonPeek(j) == ',')
        {
            j->p++;
            continue;
        }
        jsonExpect(j, '}');
        break;
    }
}

int portUpdParseRelease(const char *json, size_t len, struct PortUpdRelease *out)
{
    struct Json j;
    char key[32];

    memset(out, 0, sizeof(*out));
    j.p = json;
    j.end = json + len;
    j.err = 0;
    jsonExpect(&j, '{');
    if (jsonPeek(&j) == '}')
    {
        return -1;
    }
    while (!j.err)
    {
        jsonString(&j, key, sizeof(key));
        jsonExpect(&j, ':');
        if (strcmp(key, "tag_name") == 0 && jsonPeek(&j) == '"')
        {
            jsonString(&j, out->tag, sizeof(out->tag));
        }
        else if (strcmp(key, "html_url") == 0 && jsonPeek(&j) == '"')
        {
            jsonString(&j, out->page, sizeof(out->page));
        }
        else if (strcmp(key, "assets") == 0 && jsonPeek(&j) == '[')
        {
            j.p++;
            if (jsonPeek(&j) == ']')
            {
                j.p++;
            }
            else
            {
                while (!j.err)
                {
                    struct PortUpdAsset a;

                    jsonAsset(&j, &a);
                    if (!j.err && out->nassets < PORTUPD_MAX_ASSETS)
                    {
                        out->assets[out->nassets++] = a;
                    }
                    if (jsonPeek(&j) == ',')
                    {
                        j.p++;
                        continue;
                    }
                    jsonExpect(&j, ']');
                    break;
                }
            }
        }
        else
        {
            jsonSkip(&j, 1);
        }
        if (jsonPeek(&j) == ',')
        {
            j.p++;
            continue;
        }
        jsonExpect(&j, '}');
        break;
    }
    return (j.err || out->tag[0] == '\0') ? -1 : 0;
}

/* ---- versions ------------------------------------------------------------------ */

/* "v1.2.3" (the v optional, missing parts 0, anything after a '-' ignored) */
static int versionParts(const char *s, long part[3])
{
    int i;

    if (s == NULL)
    {
        return -1;
    }
    if (*s == 'v' || *s == 'V')
    {
        s++;
    }
    for (i = 0; i < 3; i++)
    {
        part[i] = 0;
    }
    for (i = 0; i < 3; i++)
    {
        char *end;

        if (!updDigit((unsigned char) *s))
        {
            return -1;
        }
        part[i] = strtol(s, &end, 10);
        s = end;
        if (*s == '.')
        {
            s++;
            continue;
        }
        break;
    }
    return (*s == '\0' || *s == '-' || *s == '+') ? 0 : -1;
}

int portUpdCompareVersions(const char *a, const char *b)
{
    long pa[3], pb[3];
    int i;

    if (versionParts(a, pa) != 0 || versionParts(b, pb) != 0)
    {
        return -2;
    }
    for (i = 0; i < 3; i++)
    {
        if (pa[i] != pb[i])
        {
            return (pa[i] < pb[i]) ? -1 : 1;
        }
    }
    return 0;
}

/* ---- what an update may write ------------------------------------------------------ */

static int endsWithNoCase(const char *s, const char *tail)
{
    const size_t n = strlen(s), t = strlen(tail);
    size_t i;

    if (n < t)
    {
        return 0;
    }
    for (i = 0; i < t; i++)
    {
        if (updLower((unsigned char) s[n - t + i]) != updLower((unsigned char) tail[i]))
        {
            return 0;
        }
    }
    return 1;
}

/* A path inside the zip, after "GoldenEye-Coop/". The program files beside the
 * exe may be replaced; nothing the player made or brought may: the ROM (any
 * cart image extension, wherever it is), settings (*.ini), saves and the old
 * co-op files beside the exe, and everything under Data/ except the plain text
 * files that sit directly in it (README.txt, shaders_base.txt). Saves,
 * profiles, logs, crash and bug reports all live in folders under Data/. */
int portUpdPathAllowed(const char *rel)
{
    static const char *const kept[] = { ".z64", ".n64", ".v64", ".rom", ".bin", ".ini", ".sav", ".eep" };
    const char *slash;
    size_t i;

    if (rel == NULL || rel[0] == '\0' || rel[0] == '/' || rel[0] == '.')
    {
        return 0;
    }
    if (strstr(rel, "..") != NULL || strchr(rel, '\\') != NULL || strchr(rel, ':') != NULL || strstr(rel, "//") != NULL)
    {
        return 0;
    }
    for (i = 0; rel[i] != '\0'; i++)
    {
        if ((unsigned char) rel[i] < 0x20 || (unsigned char) rel[i] >= 0x7F)
        {
            return 0;
        }
    }
    if (rel[strlen(rel) - 1] == '/')
    {
        return 0;
    }
    for (i = 0; i < sizeof(kept) / sizeof(kept[0]); i++)
    {
        if (endsWithNoCase(rel, kept[i]))
        {
            return 0;
        }
    }
    if (endsWithNoCase(rel, ".old") || endsWithNoCase(rel, ".new"))
    {   /* the updater's own spare names */
        return 0;
    }
    slash = strchr(rel, '/');
    if (slash == NULL)
    {   /* beside the exe: the co-op files that used to be saves stay */
        return strncmp(rel, "coop_", 5) != 0;
    }
    if (strncmp(rel, "Data/", 5) == 0)
    {
        return strchr(rel + 5, '/') == NULL && endsWithNoCase(rel, ".txt");
    }
    return 0;   /* no other folders in the zip today; a new one needs a rule here */
}

/* ---- SHA-256 ----------------------------------------------------------------------- */

static const unsigned long kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR32(x, n) ((((x) >> (n)) | ((x) << (32 - (n)))) & 0xFFFFFFFFUL)

static void sha256Block(unsigned long h[8], const unsigned char *b)
{
    unsigned long w[64], a, bb, c, d, e, f, g, hh;
    int i;

    for (i = 0; i < 16; i++)
    {
        w[i] = ((unsigned long) b[i * 4] << 24) | ((unsigned long) b[i * 4 + 1] << 16)
             | ((unsigned long) b[i * 4 + 2] << 8) | b[i * 4 + 3];
    }
    for (i = 16; i < 64; i++)
    {
        const unsigned long s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const unsigned long s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) & 0xFFFFFFFFUL;
    }
    a = h[0]; bb = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (i = 0; i < 64; i++)
    {
        const unsigned long s1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        const unsigned long ch = (e & f) ^ (~e & g);
        const unsigned long t1 = (hh + s1 + ch + kSha256K[i] + w[i]) & 0xFFFFFFFFUL;
        const unsigned long s0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        const unsigned long maj = (a & bb) ^ (a & c) ^ (bb & c);
        const unsigned long t2 = (s0 + maj) & 0xFFFFFFFFUL;

        hh = g; g = f; f = e;
        e = (d + t1) & 0xFFFFFFFFUL;
        d = c; c = bb; bb = a;
        a = (t1 + t2) & 0xFFFFFFFFUL;
    }
    h[0] = (h[0] + a) & 0xFFFFFFFFUL;  h[1] = (h[1] + bb) & 0xFFFFFFFFUL;
    h[2] = (h[2] + c) & 0xFFFFFFFFUL;  h[3] = (h[3] + d) & 0xFFFFFFFFUL;
    h[4] = (h[4] + e) & 0xFFFFFFFFUL;  h[5] = (h[5] + f) & 0xFFFFFFFFUL;
    h[6] = (h[6] + g) & 0xFFFFFFFFUL;  h[7] = (h[7] + hh) & 0xFFFFFFFFUL;
}

void portUpdSha256(const void *data, size_t len, char hex[65])
{
    unsigned long h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const unsigned char *p = (const unsigned char *) data;
    unsigned char last[128];
    size_t done = 0, rest, i;
    const unsigned long long bits = (unsigned long long) len * 8;

    while (len - done >= 64)
    {
        sha256Block(h, p + done);
        done += 64;
    }
    rest = len - done;
    memset(last, 0, sizeof(last));
    memcpy(last, p + done, rest);
    last[rest] = 0x80;
    {
        const size_t total = (rest + 1 + 8 <= 64) ? 64 : 128;

        for (i = 0; i < 8; i++)
        {
            last[total - 1 - i] = (unsigned char) (bits >> (8 * i));
        }
        sha256Block(h, last);
        if (total == 128)
        {
            sha256Block(h, last + 64);
        }
    }
    for (i = 0; i < 8; i++)
    {
        snprintf(hex + i * 8, 9, "%08lx", h[i]);
    }
    hex[64] = '\0';
}

/* ---- CRC-32 (the zip's) ------------------------------------------------------------- */

unsigned long portUpdCrc32(const void *data, size_t len)
{
    static unsigned long table[256];
    static int ready;
    const unsigned char *p = (const unsigned char *) data;
    unsigned long c = 0xFFFFFFFFUL;
    size_t i;

    if (!ready)
    {
        unsigned long n;

        for (n = 0; n < 256; n++)
        {
            unsigned long v = n;
            int k;

            for (k = 0; k < 8; k++)
            {
                v = (v & 1) ? (0xEDB88320UL ^ (v >> 1)) : (v >> 1);
            }
            table[n] = v;
        }
        ready = 1;
    }
    for (i = 0; i < len; i++)
    {
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFUL;
}

/* ---- inflate (RFC 1951) ----------------------------------------------------------- */

struct Inf {
    const unsigned char *in;
    size_t inlen, inpos;
    unsigned long bitbuf;
    int bitcnt;
    unsigned char *out;
    size_t outcap, outpos;
    int err;
};

struct Huff {
    short count[16];    /* codes of each length */
    short symbol[288];  /* symbols in canonical order */
};

static int infBits(struct Inf *s, int need)
{
    unsigned long val = s->bitbuf;

    while (s->bitcnt < need)
    {
        if (s->inpos >= s->inlen)
        {
            s->err = 1;
            return 0;
        }
        val |= (unsigned long) s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int) (val & ((1UL << need) - 1));
}

static int infDecode(struct Inf *s, const struct Huff *h)
{
    int code = 0, first = 0, index = 0, len;

    for (len = 1; len < 16; len++)
    {
        int count;

        code |= infBits(s, 1);
        if (s->err)
        {
            return -1;
        }
        count = h->count[len];
        if (code - count < first)
        {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* 0 complete, >0 incomplete, <0 over-subscribed */
static int infBuild(struct Huff *h, const short *length, int n)
{
    short offs[16];
    int sym, len, left;

    for (len = 0; len < 16; len++)
    {
        h->count[len] = 0;
    }
    for (sym = 0; sym < n; sym++)
    {
        h->count[length[sym]]++;
    }
    if (h->count[0] == n)
    {
        return 0;
    }
    left = 1;
    for (len = 1; len < 16; len++)
    {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
        {
            return left;
        }
    }
    offs[1] = 0;
    for (len = 1; len < 15; len++)
    {
        offs[len + 1] = (short) (offs[len] + h->count[len]);
    }
    for (sym = 0; sym < n; sym++)
    {
        if (length[sym] != 0)
        {
            h->symbol[offs[length[sym]]++] = (short) sym;
        }
    }
    return left;
}

static int infCodes(struct Inf *s, const struct Huff *lencode, const struct Huff *distcode)
{
    static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                     35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                     257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                     8193, 12289, 16385, 24577 };
    static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

    for (;;)
    {
        int sym = infDecode(s, lencode);

        if (sym < 0 || s->err)
        {
            return -1;
        }
        if (sym < 256)
        {
            if (s->outpos >= s->outcap)
            {
                return -1;
            }
            s->out[s->outpos++] = (unsigned char) sym;
        }
        else if (sym == 256)
        {
            return 0;
        }
        else
        {
            size_t len, dist;

            sym -= 257;
            if (sym >= 29)
            {
                return -1;
            }
            len = (size_t) (lbase[sym] + infBits(s, lext[sym]));
            sym = infDecode(s, distcode);
            if (sym < 0 || sym >= 30 || s->err)
            {
                return -1;
            }
            dist = (size_t) (dbase[sym] + infBits(s, dext[sym]));
            if (s->err || dist > s->outpos || len > s->outcap - s->outpos)
            {
                return -1;
            }
            while (len-- > 0)
            {
                s->out[s->outpos] = s->out[s->outpos - dist];
                s->outpos++;
            }
        }
    }
}

static int infStored(struct Inf *s)
{
    size_t len;

    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->inpos + 4 > s->inlen)
    {
        return -1;
    }
    len = s->in[s->inpos] | ((size_t) s->in[s->inpos + 1] << 8);
    if (s->in[s->inpos + 2] != (unsigned char) ~len || s->in[s->inpos + 3] != (unsigned char) (~len >> 8))
    {
        return -1;
    }
    s->inpos += 4;
    if (len > s->inlen - s->inpos || len > s->outcap - s->outpos)
    {
        return -1;
    }
    memcpy(s->out + s->outpos, s->in + s->inpos, len);
    s->inpos += len;
    s->outpos += len;
    return 0;
}

static int infFixed(struct Inf *s)
{
    static struct Huff lencode, distcode;
    static int ready;

    if (!ready)
    {
        short lengths[288];
        int sym;

        for (sym = 0; sym < 144; sym++) lengths[sym] = 8;
        for (; sym < 256; sym++) lengths[sym] = 9;
        for (; sym < 280; sym++) lengths[sym] = 7;
        for (; sym < 288; sym++) lengths[sym] = 8;
        infBuild(&lencode, lengths, 288);
        for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
        infBuild(&distcode, lengths, 30);
        ready = 1;
    }
    return infCodes(s, &lencode, &distcode);
}

static int infDynamic(struct Inf *s)
{
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    short lengths[320];
    struct Huff lencode, distcode;
    int nlen, ndist, ncode, index, err;

    nlen = infBits(s, 5) + 257;
    ndist = infBits(s, 5) + 1;
    ncode = infBits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30)
    {
        return -1;
    }
    for (index = 0; index < ncode; index++)
    {
        lengths[order[index]] = (short) infBits(s, 3);
    }
    for (; index < 19; index++)
    {
        lengths[order[index]] = 0;
    }
    if (s->err || infBuild(&lencode, lengths, 19) != 0)
    {
        return -1;
    }
    index = 0;
    while (index < nlen + ndist)
    {
        int symbol = infDecode(s, &lencode);
        int len;

        if (symbol < 0 || s->err)
        {
            return -1;
        }
        if (symbol < 16)
        {
            lengths[index++] = (short) symbol;
            continue;
        }
        len = 0;
        if (symbol == 16)
        {
            if (index == 0)
            {
                return -1;
            }
            len = lengths[index - 1];
            symbol = 3 + infBits(s, 2);
        }
        else if (symbol == 17)
        {
            symbol = 3 + infBits(s, 3);
        }
        else
        {
            symbol = 11 + infBits(s, 7);
        }
        if (s->err || index + symbol > nlen + ndist)
        {
            return -1;
        }
        while (symbol-- > 0)
        {
            lengths[index++] = (short) len;
        }
    }
    if (lengths[256] == 0)
    {
        return -1;
    }
    err = infBuild(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1))
    {
        return -1;
    }
    err = infBuild(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1))
    {
        return -1;
    }
    return infCodes(s, &lencode, &distcode);
}

long portUpdInflate(const unsigned char *in, size_t inlen, unsigned char *out, size_t outcap)
{
    struct Inf s;
    int last;

    memset(&s, 0, sizeof(s));
    s.in = in;
    s.inlen = inlen;
    s.out = out;
    s.outcap = outcap;
    do
    {
        int type, rc;

        last = infBits(&s, 1);
        type = infBits(&s, 2);
        if (s.err)
        {
            return -1;
        }
        rc = (type == 0) ? infStored(&s) : (type == 1) ? infFixed(&s) : (type == 2) ? infDynamic(&s) : -1;
        if (rc != 0)
        {
            return -1;
        }
    } while (!last);
    return (long) s.outpos;
}

/* ---- zip in memory ------------------------------------------------------------------ */

static unsigned rd16(const unsigned char *p)
{
    return p[0] | ((unsigned) p[1] << 8);
}

static unsigned long rd32(const unsigned char *p)
{
    return p[0] | ((unsigned long) p[1] << 8) | ((unsigned long) p[2] << 16) | ((unsigned long) p[3] << 24);
}

/* the end-of-central-directory record: entries, and where the directory starts */
static int zipDirectory(const unsigned char *zip, size_t len, unsigned *count, size_t *start, size_t *size)
{
    size_t i;

    if (len < 22)
    {
        return -1;
    }
    for (i = len - 22;; i--)
    {
        if (rd32(zip + i) == 0x06054b50UL)
        {
            *count = rd16(zip + i + 10);
            *size = rd32(zip + i + 12);
            *start = rd32(zip + i + 16);
            if (*start > len || *size > len - *start)
            {
                return -1;
            }
            return 0;
        }
        if (i == 0 || len - i > 22 + 65535)
        {
            return -1;
        }
    }
}

/* the i'th central record */
static const unsigned char *zipRecord(const unsigned char *zip, size_t len, int i)
{
    unsigned count;
    size_t start, size, pos;
    int k;

    if (zipDirectory(zip, len, &count, &start, &size) != 0 || i < 0 || (unsigned) i >= count)
    {
        return NULL;
    }
    pos = start;
    for (k = 0;; k++)
    {
        const unsigned char *r = zip + pos;

        if (pos + 46 > start + size || rd32(r) != 0x02014b50UL)
        {
            return NULL;
        }
        if (pos + 46 + rd16(r + 28) > start + size)
        {
            return NULL;
        }
        if (k == i)
        {
            return r;
        }
        pos += 46 + rd16(r + 28) + rd16(r + 30) + rd16(r + 32);
    }
}

int portUpdZipCount(const unsigned char *zip, size_t len)
{
    unsigned count;
    size_t start, size;

    return (zipDirectory(zip, len, &count, &start, &size) == 0) ? (int) count : -1;
}

int portUpdZipEntry(const unsigned char *zip, size_t len, int i, struct PortUpdZipEntry *e)
{
    const unsigned char *r = zipRecord(zip, len, i);
    unsigned n;

    if (r == NULL)
    {
        return -1;
    }
    n = rd16(r + 28);
    if (n == 0 || n >= sizeof(e->name))
    {
        return -1;
    }
    memcpy(e->name, r + 46, n);
    e->name[n] = '\0';
    if (memchr(e->name, '\0', n) != NULL)
    {
        return -1;
    }
    e->dir = (e->name[n - 1] == '/');
    e->size = rd32(r + 24);
    return (e->size == 0xFFFFFFFFUL) ? -1 : 0;   /* zip64: never ours */
}

int portUpdZipExtract(const unsigned char *zip, size_t len, int i, unsigned char *out)
{
    const unsigned char *r = zipRecord(zip, len, i);
    unsigned method, flags;
    unsigned long crc, csize, usize, local;
    size_t data;

    if (r == NULL)
    {
        return -1;
    }
    flags = rd16(r + 8);
    method = rd16(r + 10);
    crc = rd32(r + 16);
    csize = rd32(r + 20);
    usize = rd32(r + 24);
    local = rd32(r + 42);
    if ((flags & 1) != 0 || local + 30 > len || rd32(zip + local) != 0x04034b50UL)
    {   /* encrypted, or the local header isn't where it says */
        return -1;
    }
    data = local + 30 + rd16(zip + local + 26) + rd16(zip + local + 28);
    if (data > len || csize > len - data)
    {
        return -1;
    }
    if (method == 0)
    {
        if (csize != usize)
        {
            return -1;
        }
        memcpy(out, zip + data, usize);
    }
    else if (method == 8)
    {
        if (portUpdInflate(zip + data, csize, out, usize) != (long) usize)
        {
            return -1;
        }
    }
    else
    {
        return -1;
    }
    return (portUpdCrc32(out, usize) == crc) ? 0 : -1;
}
