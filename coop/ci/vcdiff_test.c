/*
 * vcdiff_test.c - port/src/port_vcdiff.c on crafted patches, no cart needed.
 *
 *   gcc -m32 -O1 -Wall -Iport/include -o vcdiff_test coop/ci/vcdiff_test.c port/src/port_vcdiff.c
 *   ./vcdiff_test [real.xdelta base.bin sha1-of-nothing]
 *
 * Built -m32 on purpose: the i686 game has a 32-bit size_t, where an
 * unchecked offset + length wraps. Each hostile patch below must be refused
 * (and, under -fsanitize=address, without touching memory outside its
 * buffers); the good ones must give the expected bytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "port_vcdiff.h"

static int sFail;

#define CHECK(cond, ...) do { if (!(cond)) { sFail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct pbuf {
    uint8_t b[512];
    size_t n;
};

static void put(struct pbuf *p, uint8_t c)
{
    if (p->n < sizeof(p->b)) {
        p->b[p->n++] = c;
    }
}

static void putv(struct pbuf *p, uint32_t v)
{   /* VCDIFF varint: 7 bits a byte, most significant first */
    uint8_t t[5];
    int n = 0;
    do {
        t[n++] = (uint8_t) (v & 0x7F);
        v >>= 7;
    } while (v != 0);
    while (n > 0) {
        n--;
        put(p, (uint8_t) (t[n] | (n > 0 ? 0x80 : 0)));
    }
}

static void putbytes(struct pbuf *p, const void *d, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        put(p, ((const uint8_t *) d)[i]);
    }
}

static uint32_t adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/* one window: the raw fields, so a test can lie in any of them */
struct win {
    uint32_t ind, srcsz, srcpos, tgtlen;
    uint32_t datalen, instlen, addrlen;   /* as written; real sections follow */
    const uint8_t *data, *inst, *addr;
    size_t ndata, ninst, naddr;
    int adler;                            /* 1 = add one over `want` */
    const uint8_t *want;
    uint32_t wantlen;
    int badadler;
};

static void putwin(struct pbuf *p, const struct win *w)
{
    struct pbuf d;
    memset(&d, 0, sizeof(d));
    putv(&d, w->tgtlen);
    put(&d, 0);   /* delta indicator: no compressed sections */
    putv(&d, w->datalen);
    putv(&d, w->instlen);
    putv(&d, w->addrlen);
    if (w->adler) {
        uint32_t a = adler32(w->want, w->wantlen) ^ (w->badadler ? 1u : 0u);
        put(&d, (uint8_t) (a >> 24)); put(&d, (uint8_t) (a >> 16)); put(&d, (uint8_t) (a >> 8)); put(&d, (uint8_t) a);
    }
    putbytes(&d, w->data, w->ndata);
    putbytes(&d, w->inst, w->ninst);
    putbytes(&d, w->addr, w->naddr);
    put(p, (uint8_t) (w->ind | (w->adler ? 0x04 : 0)));
    if (w->ind & 0x03) {
        putv(p, w->srcsz);
        putv(p, w->srcpos);
    }
    putv(p, (uint32_t) d.n);
    putbytes(p, d.b, d.n);
}

static void header(struct pbuf *p)
{
    memset(p, 0, sizeof(*p));
    put(p, 0xD6); put(p, 0xC3); put(p, 0xC4); put(p, 0x00);
    put(p, 0x00);   /* no secondary compression, no code table, no app header */
}

static uint8_t sSrc[4096];

static int apply(const struct pbuf *p, uint8_t **out, size_t *outlen, char *err, size_t errn)
{
    return portVcdiffApply(sSrc, sizeof(sSrc), p->b, p->n, out, outlen, err, errn);
}

/* default code table: 1 = ADD (size follows), 19 = COPY mode 0 (size follows), 0 = RUN */
static const uint8_t kInstCopyAdd[] = { 19, 32, 1, 2 };   /* COPY 32 from addr 0, then ADD 2 */
static const uint8_t kAddr0[] = { 0 };
static const uint8_t kData[] = { 'X', 'Y' };

static void good(int withadler)
{
    struct pbuf p;
    struct win w;
    uint8_t want[34];
    uint8_t *out = NULL;
    size_t outlen = 0;
    char err[200] = "";

    memcpy(want, sSrc + 100, 32);
    memcpy(want + 32, kData, 2);
    header(&p);
    memset(&w, 0, sizeof(w));
    w.ind = 0x01; w.srcsz = 64; w.srcpos = 100; w.tgtlen = 34;
    w.data = kData; w.ndata = w.datalen = 2;
    w.inst = kInstCopyAdd; w.ninst = w.instlen = sizeof(kInstCopyAdd);
    w.addr = kAddr0; w.naddr = w.addrlen = 1;
    w.adler = withadler; w.want = want; w.wantlen = 34;
    putwin(&p, &w);
    CHECK(apply(&p, &out, &outlen, err, sizeof(err)), "good patch%s refused: %s", withadler ? " (adler)" : "", err);
    CHECK(outlen == 34 && out != NULL && memcmp(out, want, 34) == 0, "good patch%s: wrong output", withadler ? " (adler)" : "");
    free(out);

    w.badadler = 1;
    if (withadler) {
        header(&p);
        putwin(&p, &w);
        out = NULL;
        CHECK(!apply(&p, &out, &outlen, err, sizeof(err)) && out == NULL, "a bad Adler-32 was accepted");
        printf("  bad adler: %s\n", err);
    }
}

static void refuse(const char *what, const struct pbuf *p)
{
    uint8_t *out = (uint8_t *) 1;
    size_t outlen = 1;
    char err[200] = "";
    int ok = apply(p, &out, &outlen, err, sizeof(err));

    CHECK(!ok, "%s: accepted", what);
    CHECK(out == NULL && outlen == 0, "%s: output left set", what);
    if (ok) {
        free(out);
    }
    printf("  %s: %s\n", what, err);
}

static struct win copywin(uint32_t srcsz, uint32_t srcpos)
{
    struct win w;
    memset(&w, 0, sizeof(w));
    w.ind = 0x01; w.srcsz = srcsz; w.srcpos = srcpos; w.tgtlen = 34;
    w.data = kData; w.ndata = w.datalen = 2;
    w.inst = kInstCopyAdd; w.ninst = w.instlen = sizeof(kInstCopyAdd);
    w.addr = kAddr0; w.naddr = w.addrlen = 1;
    return w;
}

int main(int argc, char **argv)
{
    struct pbuf p;
    struct win w;
    size_t i;

    for (i = 0; i < sizeof(sSrc); i++) {
        sSrc[i] = (uint8_t) (i * 7 + 3);
    }
    good(0);
    good(1);

    memset(&p, 0, sizeof(p));
    put(&p, 0xD6); put(&p, 0xC3); put(&p, 0xC4); put(&p, 0x00);
    refuse("truncated header", &p);

    header(&p);
    p.b[0] = 0xD7;
    refuse("bad magic", &p);

    header(&p);
    refuse("no windows", &p);

    /* the source segment wraps a 32-bit size_t: 0xFFFFFF00 + 0x200 */
    header(&p);
    w = copywin(0x200, 0xFFFFFF00u);
    putwin(&p, &w);
    refuse("source segment past the end (wraps)", &p);

    header(&p);
    w = copywin(0xFFFFFFFFu, 1);
    putwin(&p, &w);
    refuse("source segment longer than the source", &p);

    /* a target segment (VCD_TARGET) before there is any output */
    header(&p);
    w = copywin(0x200, 0xFFFFFF00u);
    w.ind = 0x02;
    putwin(&p, &w);
    refuse("target segment past the output (wraps)", &p);

    /* COPY 32 from a 16-byte segment */
    header(&p);
    w = copywin(16, 0);
    putwin(&p, &w);
    refuse("COPY past the source segment", &p);

    /* section lengths whose 32-bit sum wraps round to the real size */
    header(&p);
    w = copywin(64, 100);
    w.datalen = 0xFFFFFFF0u;
    w.instlen = (uint32_t) (0x10 + 2 + sizeof(kInstCopyAdd));
    putwin(&p, &w);
    refuse("section lengths that wrap", &p);

    /* a target length that would wrap the output size, and one over the cap */
    header(&p);
    w = copywin(64, 100);
    w.tgtlen = 0xFFFFFFFFu;
    putwin(&p, &w);
    refuse("target length 4 GB", &p);

    header(&p);
    w = copywin(64, 100);
    w.tgtlen = 65u << 20;
    putwin(&p, &w);
    refuse("target length over 64 MB", &p);

    /* the window claims more delta bytes than the patch has */
    header(&p);
    w = copywin(64, 100);
    putwin(&p, &w);
    p.n -= 3;
    refuse("truncated window", &p);

    /* an ADD that writes past the window's target length */
    header(&p);
    w = copywin(64, 100);
    w.tgtlen = 33;
    putwin(&p, &w);
    refuse("ADD past the target length", &p);

    if (argc >= 3) {
        /* a real patch against its base, e.g. lang/es/GE-ESP-NTSC.xdelta and the cart */
        FILE *fp = fopen(argv[1], "rb"), *fb = fopen(argv[2], "rb");
        uint8_t *pt, *bs, *out = NULL;
        long np, nb;
        size_t outlen = 0;
        char err[200] = "";
        if (fp == NULL || fb == NULL) {
            printf("FAIL: can't open %s or %s\n", argv[1], argv[2]);
            return 1;
        }
        fseek(fp, 0, SEEK_END); np = ftell(fp); fseek(fp, 0, SEEK_SET);
        fseek(fb, 0, SEEK_END); nb = ftell(fb); fseek(fb, 0, SEEK_SET);
        pt = (uint8_t *) malloc((size_t) np);
        bs = (uint8_t *) malloc((size_t) nb);
        if (pt == NULL || bs == NULL || fread(pt, 1, (size_t) np, fp) != (size_t) np || fread(bs, 1, (size_t) nb, fb) != (size_t) nb) {
            printf("FAIL: can't read the files\n");
            return 1;
        }
        CHECK(portVcdiffApply(bs, (size_t) nb, pt, (size_t) np, &out, &outlen, err, sizeof(err)), "%s: %s", argv[1], err);
        printf("  %s: %lu bytes out\n", argv[1], (unsigned long) outlen);
        free(out);
        free(pt);
        free(bs);
        fclose(fp);
        fclose(fb);
    }
    printf(sFail ? "%d failures\n" : "ok (0 failures)\n", sFail);
    return sFail ? 1 : 0;
}
