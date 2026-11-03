/**
 * port_vcdiff.c - a small VCDIFF (RFC 3284) decoder, enough for xdelta3
 * patches made without secondary compression (xdelta3 -S none, or the
 * default for most ROM-hack patches made with xdeltaUI / Delta Patcher).
 *
 * Why it exists: language packs (port_lang.c) ship as the translators' own
 * .xdelta patches for the stock (U) cart. The game applies one in memory to
 * the player's own cart image at start-up and reads the translated text banks
 * out of the result, so nothing from the cart is ever shipped.
 *
 * Supported: the default code table, VCD_SOURCE / VCD_TARGET windows, the
 * xdelta3 application header (skipped) and its per-window Adler-32 (checked).
 * Refused with a message: secondary compression (DJW/FGK/LZMA), a custom
 * code table, compressed sections.
 *
 * Every length and offset in the patch is checked without overflow on
 * 32-bit size_t (the i686 builds): a damaged or hostile patch is refused, it
 * never reads or writes outside its buffers. The output is capped at
 * VC_MAX_OUTPUT.
 *
 * NO SDL, no game headers: builds anywhere, and coop/ci/vcdiff_test.c runs it
 * on the host (CI, next to update_test).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "port_vcdiff.h"

enum { VC_NOOP = 0, VC_ADD = 1, VC_RUN = 2, VC_COPY = 3 };

/* the largest patched file accepted: a cart is at most 64 MB */
#define VC_MAX_OUTPUT (64u << 20)

struct vcinst {
    uint8_t type1, size1, mode1;
    uint8_t type2, size2, mode2;
};

/* RFC 3284 section 5.6, the default instruction code table */
static struct vcinst sTable[256];
static int sTableBuilt;

static void vcdiffBuildTable(void)
{
    int i = 0;
    int mode, size, asz, csz;

    memset(sTable, 0, sizeof(sTable));
    sTable[i].type1 = VC_RUN; sTable[i].size1 = 0; i++;
    for (size = 0; size <= 17; size++) {
        sTable[i].type1 = VC_ADD; sTable[i].size1 = (uint8_t) size; i++;
    }
    for (mode = 0; mode <= 8; mode++) {
        sTable[i].type1 = VC_COPY; sTable[i].size1 = 0; sTable[i].mode1 = (uint8_t) mode; i++;
        for (size = 4; size <= 18; size++) {
            sTable[i].type1 = VC_COPY; sTable[i].size1 = (uint8_t) size; sTable[i].mode1 = (uint8_t) mode; i++;
        }
    }
    for (mode = 0; mode <= 5; mode++) {
        for (asz = 1; asz <= 4; asz++) {
            for (csz = 4; csz <= 6; csz++) {
                sTable[i].type1 = VC_ADD; sTable[i].size1 = (uint8_t) asz;
                sTable[i].type2 = VC_COPY; sTable[i].size2 = (uint8_t) csz; sTable[i].mode2 = (uint8_t) mode;
                i++;
            }
        }
    }
    for (mode = 6; mode <= 8; mode++) {
        for (asz = 1; asz <= 4; asz++) {
            sTable[i].type1 = VC_ADD; sTable[i].size1 = (uint8_t) asz;
            sTable[i].type2 = VC_COPY; sTable[i].size2 = 4; sTable[i].mode2 = (uint8_t) mode;
            i++;
        }
    }
    for (mode = 0; mode <= 8; mode++) {
        sTable[i].type1 = VC_COPY; sTable[i].size1 = 4; sTable[i].mode1 = (uint8_t) mode;
        sTable[i].type2 = VC_ADD; sTable[i].size2 = 1;
        i++;
    }
    sTableBuilt = (i == 256);
}

struct vcbuf {
    const uint8_t *p;
    size_t n;
    size_t at;
};

static int vcByte(struct vcbuf *b, uint32_t *out)
{
    if (b->at >= b->n) {
        return 0;
    }
    *out = b->p[b->at++];
    return 1;
}

/* RFC 3284 section 2: base-128, most significant digit first */
static int vcVarint(struct vcbuf *b, uint32_t *out)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 5; i++) {
        uint32_t c;
        if (!vcByte(b, &c)) {
            return 0;
        }
        v = (v << 7) | (c & 0x7F);
        if (v > 0xFFFFFFFFu) {
            return 0;
        }
        if ((c & 0x80) == 0) {
            *out = (uint32_t) v;
            return 1;
        }
    }
    return 0;
}

static uint32_t vcAdler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

#define VC_NEAR 4
#define VC_SAME 3

struct vccache {
    uint32_t near_[VC_NEAR];
    int next;
    uint32_t same[VC_SAME * 256];
};

static void vcCacheUpdate(struct vccache *c, uint32_t addr)
{
    c->near_[c->next] = addr;
    c->next = (c->next + 1) % VC_NEAR;
    c->same[addr % (VC_SAME * 256)] = addr;
}

static int vcAddr(struct vccache *c, struct vcbuf *ab, uint32_t here, uint32_t mode, uint32_t *out)
{
    uint32_t v, addr;
    if (mode == 0) {
        if (!vcVarint(ab, &addr)) return 0;
    } else if (mode == 1) {
        if (!vcVarint(ab, &v) || v > here) return 0;
        addr = here - v;
    } else if (mode < 2 + VC_NEAR) {
        if (!vcVarint(ab, &v)) return 0;
        addr = c->near_[mode - 2] + v;
    } else if (mode < 2 + VC_NEAR + VC_SAME) {
        if (!vcByte(ab, &v)) return 0;
        addr = c->same[(mode - (2 + VC_NEAR)) * 256 + v];
    } else {
        return 0;
    }
    vcCacheUpdate(c, addr);
    *out = addr;
    return 1;
}

static int vcGrow(uint8_t **out, size_t *cap, size_t need)
{
    size_t nc;
    uint8_t *np;
    if (need <= *cap) {
        return 1;
    }
    if (need > VC_MAX_OUTPUT) {
        return 0;
    }
    nc = *cap ? *cap : (1u << 20);
    while (nc < need) {
        nc *= 2;   /* need <= 64 MB: no wrap */
    }
    np = (uint8_t *) realloc(*out, nc);
    if (np == NULL) {
        return 0;
    }
    *out = np;
    *cap = nc;
    return 1;
}

#define VCFAIL(...) do { if (err != NULL && errn > 0) { snprintf(err, errn, __VA_ARGS__); } goto fail; } while (0)

int portVcdiffApply(const uint8_t *src, size_t srclen, const uint8_t *patch, size_t patchlen,
                    uint8_t **out, size_t *outlen, char *err, size_t errn)
{
    struct vcbuf pb;
    uint32_t hdr, v;
    uint8_t *dst = NULL;
    size_t cap = 0, len = 0;
    int nwin = 0;

    *out = NULL;
    *outlen = 0;
    if (!sTableBuilt) {
        vcdiffBuildTable();
    }
    if (patchlen < 5 || patch[0] != 0xD6 || patch[1] != 0xC3 || patch[2] != 0xC4) {
        VCFAIL("not a VCDIFF / xdelta3 patch (bad magic)");
    }
    if (patch[3] != 0) {
        VCFAIL("VCDIFF version %u is not supported", patch[3]);
    }
    pb.p = patch;
    pb.n = patchlen;
    pb.at = 4;
    if (!vcByte(&pb, &hdr)) {
        VCFAIL("truncated header");
    }
    if (hdr & 0x01) {
        VCFAIL("the patch uses secondary compression; re-make it with xdelta3 -S none");
    }
    if (hdr & 0x02) {
        VCFAIL("the patch uses a custom code table, which is not supported");
    }
    if (hdr & 0x04) { /* xdelta3 application header: usually the file names */
        if (!vcVarint(&pb, &v) || v > pb.n - pb.at) {
            VCFAIL("truncated application header");
        }
        pb.at += v;
    }
    if (hdr & ~0x07u) {
        VCFAIL("unknown header bits 0x%02X", hdr);
    }

    while (pb.at < pb.n) {
        uint32_t win, srcsz = 0, srcpos = 0, enclen, tgtlen, delta, datalen, instlen, addrlen;
        uint32_t adler = 0;
        const uint8_t *seg = NULL;
        struct vcbuf db, ib, ab;
        struct vccache cache;
        size_t tbase, encend;
        uint32_t t = 0; /* bytes of this target window written */

        if (!vcByte(&pb, &win)) VCFAIL("truncated window %d", nwin);
        if (win & ~0x07u) VCFAIL("window %d: unknown indicator 0x%02X", nwin, win);
        if ((win & 0x03) == 0x03) VCFAIL("window %d: both source and target segment", nwin);
        if (win & 0x03) {
            if (!vcVarint(&pb, &srcsz) || !vcVarint(&pb, &srcpos)) VCFAIL("window %d: truncated segment", nwin);
            if (win & 0x01) {
                if (srcsz > srclen || srcpos > srclen - srcsz) {
                    VCFAIL("window %d: reads the source at 0x%X+0x%X, past its %u bytes (wrong base file?)", nwin,
                           srcpos, srcsz, (unsigned) srclen);
                }
                seg = src + srcpos;
            } else {
                if (srcsz > len || srcpos > len - srcsz) VCFAIL("window %d: target segment past the output", nwin);
                seg = NULL; /* resolved against dst below: dst can move on realloc */
            }
        }
        if (!vcVarint(&pb, &enclen)) VCFAIL("window %d: truncated", nwin);
        if (enclen > pb.n - pb.at) VCFAIL("window %d: delta runs past the end of the patch", nwin);
        encend = pb.at + enclen;
        if (!vcVarint(&pb, &tgtlen) || !vcByte(&pb, &delta)) VCFAIL("window %d: truncated", nwin);
        if (delta != 0) VCFAIL("window %d: compressed sections (0x%02X) are not supported", nwin, delta);
        if (!vcVarint(&pb, &datalen) || !vcVarint(&pb, &instlen) || !vcVarint(&pb, &addrlen)) {
            VCFAIL("window %d: truncated section lengths", nwin);
        }
        if (win & 0x04) {
            if (pb.n - pb.at < 4) VCFAIL("window %d: truncated checksum", nwin);
            adler = ((uint32_t) pb.p[pb.at] << 24) | ((uint32_t) pb.p[pb.at + 1] << 16) |
                    ((uint32_t) pb.p[pb.at + 2] << 8) | pb.p[pb.at + 3];
            pb.at += 4;
        }
        if (encend < pb.at || (uint64_t) datalen + instlen + addrlen != (uint64_t) (encend - pb.at)) {
            VCFAIL("window %d: section lengths do not add up", nwin);
        }
        db.p = pb.p + pb.at; db.n = datalen; db.at = 0;
        ib.p = db.p + datalen; ib.n = instlen; ib.at = 0;
        ab.p = ib.p + instlen; ab.n = addrlen; ab.at = 0;
        pb.at = encend;

        if (tgtlen > VC_MAX_OUTPUT || len > VC_MAX_OUTPUT - tgtlen) {
            VCFAIL("window %d: the patched file would be over %u MB", nwin, (unsigned) (VC_MAX_OUTPUT >> 20));
        }
        if (!vcGrow(&dst, &cap, len + tgtlen)) VCFAIL("out of memory");
        tbase = len;
        memset(cache.near_, 0, sizeof(cache.near_));
        memset(cache.same, 0, sizeof(cache.same));
        cache.next = 0;

        while (ib.at < ib.n) {
            uint32_t code;
            int k;
            if (!vcByte(&ib, &code)) VCFAIL("window %d: truncated instructions", nwin);
            for (k = 0; k < 2; k++) {
                const struct vcinst *in = &sTable[code];
                uint32_t type = k ? in->type2 : in->type1;
                uint32_t size = k ? in->size2 : in->size1;
                uint32_t mode = k ? in->mode2 : in->mode1;
                if (type == VC_NOOP) {
                    continue;
                }
                if (size == 0 && !vcVarint(&ib, &size)) VCFAIL("window %d: truncated size", nwin);
                if ((uint64_t) t + size > tgtlen) VCFAIL("window %d: writes past its target length", nwin);
                if (type == VC_ADD) {
                    if (size > db.n - db.at) VCFAIL("window %d: ADD past the data section", nwin);
                    memcpy(dst + tbase + t, db.p + db.at, size);
                    db.at += size;
                } else if (type == VC_RUN) {
                    uint32_t c;
                    if (!vcByte(&db, &c)) VCFAIL("window %d: RUN past the data section", nwin);
                    memset(dst + tbase + t, (int) c, size);
                } else { /* COPY */
                    uint32_t addr, i;
                    if (!vcAddr(&cache, &ab, srcsz + t, mode, &addr)) VCFAIL("window %d: bad COPY address", nwin);
                    if (addr < srcsz) {
                        const uint8_t *from;
                        if (size > srcsz - addr) VCFAIL("window %d: COPY runs past the source segment", nwin);
                        from = (win & 0x01) ? seg + addr : dst + srcpos + addr;
                        memcpy(dst + tbase + t, from, size);
                    } else {
                        uint32_t ta = addr - srcsz;
                        if (ta >= t) VCFAIL("window %d: COPY from ahead of the output", nwin);
                        for (i = 0; i < size; i++) { /* may overlap: byte by byte */
                            dst[tbase + t + i] = dst[tbase + ta + i];
                        }
                    }
                }
                t += size;
            }
        }
        if (t != tgtlen) VCFAIL("window %d: wrote %u of %u bytes", nwin, t, tgtlen);
        if ((win & 0x04) && vcAdler32(dst + tbase, tgtlen) != adler) {
            VCFAIL("window %d: checksum mismatch (the patch is for another file, or is damaged)", nwin);
        }
        len += tgtlen;
        nwin++;
    }
    if (nwin == 0) VCFAIL("the patch has no windows");
    *out = dst;
    *outlen = len;
    return 1;

fail:
    free(dst);
    return 0;
}
