/**
 * Co-op character figures (port only): the lobby's characters as 3D models
 * instead of photos, for the character select (front.c coopCharsel*) and the
 * players' cards in the lobby strip.
 *
 * A figure is one co-op pick (coopCharacter's numbering: 0 the mission's
 * Bond, else 1 + an mp_chr_setup index) loaded the way the solo Bond's body
 * is (bondview2.c): private copies of the body and head headers, the files,
 * a texture pool and the Model itself in a buffer of its own, so nothing the
 * game shares (c_item_entries' headers, the model slots, the wallet's
 * buffer) is touched. A figure is loaded the first time it is drawn and kept:
 * the renderer's caches know textures and vertices by their address, so a
 * buffer is never handed to another character. Each is drawn into a
 * rectangle of the 440 x 330 menu screen by a projection of its own,
 * standing in its idle animation, turned by a yaw.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <macro.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include <fr.h>
#include "front.h"
#include "chr_b.h"
#include "chrobjdata.h"
#include "initanitable.h"
#include "matrixmath.h"
#include "model.h"
#include "image.h"
#include "dyn.h"
#include "lv.h"
#include "ob.h"
#include "player.h"
#include "othermodemicrocode.h"
#include "coop.h"
#include "textrelated.h"

extern void portConLogf(const char *fmt, ...);
extern struct MP_selectable_chars mp_chr_setup[];
extern ModelRenderData cast_renderdata_default;
void load_object_fill_header(struct ModelFileHeader *objheader, u8 *name, u8 *dst, s32 size, struct texpool *buffer);

#define FIG_PICKS   (PORT_MP_CHAR_COUNT + 1)
#define FIG_BUF     0x20000     /* the texture pool, then the files and the Model: 41 KB at most (all 71 tried) */
#define FIG_TEX     0x1000      /* the port keeps model textures in its own pool; this one stays empty */
#define SCREEN_W    440         /* the menus' screen */
#define SCREEN_H    330

typedef struct
{
    u8 *buf;
    ModelFileHeader body;
    ModelFileHeader head;
    Model *model;
    s32 failed;
    s32 measured;
    f32 feet, top;              /* its height in the idle, at its scale */
    u32 ticked;                 /* the frame its animation last moved (sFrame) */
} CoopFigure;

static CoopFigure sFig[FIG_PICKS];
static u32 sFrame = 1;

/* the body and head a pick wears in the lobby: the mission's Bond is Rare's tuxedo Bond */
static s32 coopFigureParts(s32 pick, s32 *head)
{
    s32 body = coopCharacterBody(pick, head);

    if (body < 0)
    {
        body = mp_chr_setup[0].body;
        *head = mp_chr_setup[0].head;
    }
    if (c_item_entries[body].hasHead)
    {
        *head = -1;
    }
    return body;
}

static s32 coopFigureLoad(CoopFigure *f, s32 pick)
{
    struct texpool pool;
    ModelFileHeader *headheader = NULL;
    u32 *animdata;
    s32 head;
    const s32 body = coopFigureParts(pick, &head);
    s32 cursor, nrec;

    if ((f->buf = (u8 *) calloc(1, FIG_BUF)) == NULL)
    {
        return FALSE;
    }
    texInitPool(&pool, f->buf, FIG_TEX);

    f->body = *c_item_entries[body].header;
    f->body.RootNode = NULL;
    load_object_fill_header(&f->body, (u8 *) c_item_entries[body].filename, f->buf + FIG_TEX, FIG_BUF - FIG_TEX, &pool);
    cursor = FIG_TEX + ALIGN64_V2(get_pc_buffer_remaining_value((u8 *) c_item_entries[body].filename));
    if (head >= 0)
    {
        f->head = *c_item_entries[head].header;
        f->head.RootNode = NULL;
        load_object_fill_header(&f->head, (u8 *) c_item_entries[head].filename, f->buf + cursor, FIG_BUF - cursor, &pool);
        cursor += ALIGN64_V2(get_pc_buffer_remaining_value((u8 *) c_item_entries[head].filename));
        headheader = &f->head;
    }
    if (f->body.RootNode == NULL || (headheader != NULL && headheader->RootNode == NULL))
    {
        portConLogf("ge: coop: character %d did not load\n", pick);
        return FALSE;
    }

    f->model = (Model *) (f->buf + cursor);
    cursor = ALIGN64_V2(cursor + sizeof(Model));
    modelCalculateRwDataLen(&f->body);
    nrec = f->body.numRecords + 0xA;
    if (headheader != NULL)
    {
        modelCalculateRwDataLen(headheader);
        nrec += headheader->numRecords;
    }
    animdata = (u32 *) (f->buf + cursor);
    cursor = ALIGN64_V2(cursor + nrec * 4);
    if (cursor > FIG_BUF)
    {
        portConLogf("ge: coop: character %d is too big for its figure\n", pick);
        f->model = NULL;
        return FALSE;
    }
    animInit(f->model, &f->body, animdata);
    f->model->rwdatalen = nrec;
    makeonebody(body, head, &f->body, headheader, 0, f->model);

    {
        coord3d zero = { 0.0f, 0.0f, 0.0f };

        setsuboffset(f->model, &zero);
        setsubroty(f->model, 0.0f);
    }
    modelSetAnimPlaySpeed(f->model, 0.5f, 0);
    modelSetAnimation(f->model, (ModelAnimation *) animation_table_ptrs1[ANIM_idle], 0, 0.0f, 1.0f, 0.0f);
    modelSetAnimLooping(f->model, 0.0f, 0.0f);
    f->ticked = 0;
    portConLogf("ge: coop: character %d loaded for the lobby (%d KB)\n", pick, cursor / 1024);
    return TRUE;
}

static CoopFigure *coopFigureGet(s32 pick)
{
    CoopFigure *f;

    if (pick < 0 || pick >= FIG_PICKS)
    {
        return NULL;
    }
    f = &sFig[pick];
    if (f->model == NULL && !f->failed && !coopFigureLoad(f, pick))
    {
        f->failed = TRUE;   /* once: the log says why */
    }
    return (f->model != NULL) ? f : NULL;
}

void coopFigureBeginFrame(void)
{
    sFrame++;
}

/* the lowest and highest joints in the idle pose: from the feet to the
 * top of the head (the head's joint is at the neck, so a little over) */
static void coopFigureMeasure(CoopFigure *f)
{
    ModelRenderData rd = cast_renderdata_default;
    Mtxf base;
    s32 i, n = f->model->obj->numMatrices;
    f32 lo = 1e9f, hi = -1e9f;

    matrix_4x4_set_identity(&base);
    rd.basemtx = &base;
    rd.mtxlist = (Mtxf *) dynAllocate(n << 6);
    subcalcmatrices(&rd, f->model);
    for (i = 0; i < n; i++)
    {
        const f32 y = ((Mtxf *) f->model->render_pos)[i].m[3][1];   /* subcalcmatrices moves rd.mtxlist on */

        lo = (y < lo) ? y : lo;
        hi = (y > hi) ? y : hi;
    }
    if (hi <= lo)
    {
        lo = 0.0f;
        hi = 150.0f;
    }
    f->feet = lo;
    f->top = hi + (hi - lo) * 0.14f;
    f->measured = TRUE;
}

/**
 * Draw pick's figure into the rectangle x, y, w, h (menu coordinates), turned
 * by yaw. bust: head and shoulders filling the rectangle, else the whole
 * body standing in it.
 */
Gfx *coopFigureDraw(Gfx *DL, s32 pick, s32 x, s32 y, s32 w, s32 h, f32 yaw, s32 bust)
{
    CoopFigure *f = coopFigureGet(pick);
    ModelRenderData rd = cast_renderdata_default;
    Mtx *proj;
    Mtxf *cam;
    Mtxf tmp;
    u16 norm;
    f32 ty, dist, tall;
    const f32 fovy = 30.0f;
    coord3d zero = { 0.0f, 0.0f, 0.0f };
    s32 i;

    if (f == NULL || w <= 0 || h <= 0)
    {
        return DL;
    }
    if (f->ticked != sFrame)
    {
        f->ticked = sFrame;
        modelTickAnim(f->model, g_ClockTimer, 1);
    }
    setsuboffset(f->model, &zero);    /* the idle sways in place; never walks off */
    setsubroty(f->model, yaw);

    /* the menus' text state, but with perspective-correct, mipmapped
     * texturing as the game's models expect: the text's flat sampling
     * smears a face's texture across the head */
    DL = combiner_bayer_lod_perspective(microcode_constructor(DL));
    /* the menu's whole-screen viewport stays; the projection itself puts
     * the picture in the rectangle (the port's viewport handling is the
     * folder's and the HUD's, tuned for whole screens) */
    {
        f32 pf[4][4];
        const f32 sx = (f32) w / (f32) SCREEN_W, sy = (f32) h / (f32) SCREEN_H;
        const f32 tx = (2.0f * x + w) / (f32) SCREEN_W - 1.0f;
        const f32 ty2 = 1.0f - (2.0f * y + h) / (f32) SCREEN_H;
        s32 r;

        guPerspectiveF(pf, &norm, fovy, (f32) w / (f32) h, 10.0f, 4000.0f, 1.0f);
        for (r = 0; r < 4; r++)
        {   /* row vectors: v * P, then x' = x sx + w tx, y' = y sy + w ty */
            pf[r][0] = pf[r][0] * sx + pf[r][3] * tx;
            pf[r][1] = pf[r][1] * sy + pf[r][3] * ty2;
        }
        proj = dynAllocateMatrix();
        guMtxF2L(pf, proj);
    }
    gSPMatrix(DL++, OS_K0_TO_PHYSICAL(proj), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gSPPerspNormalize(DL++, norm);
    gDPSetScissor(DL++, G_SC_NON_INTERLACE, x, y, x + w, y + h);

    {
        Light *look = dynAllocateLights(2);
        Mtx lookat;

        guLookAtReflect(&lookat, (LookAt *) look, 0.0f, 0.0f, 4000.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
        gSPNumLights(DL++, 1);
        gSPLight(DL++, &gelogolight.l[0], 1);
        gSPLight(DL++, &gelogolight.a, 2);
        gSPLookAt(DL++, look);
    }

    modelSetDistanceDisabled(1);
    sub_GAME_7F073FC8(0);
    subcalcpos(f->model);
    if (!f->measured)
    {
        coopFigureMeasure(f);
    }

    /* the camera: a whole body stands in the frame with a little floor
     * showing; a bust is the head and shoulders */
    tall = f->top - f->feet;
    if (bust)
    {
        ty = f->top - tall * 0.21f;
        dist = (tall * 0.44f) / (2.0f * 0.2679f);     /* tan(15 degrees): fovy 30 */
    }
    else
    {
        ty = f->feet + tall * 0.5f;
        dist = (tall * 1.18f) / (2.0f * 0.2679f);
    }

    cam = (Mtxf *) dynAllocate(sizeof(Mtxf));
    matrix_4x4_set_lookat_target(cam, 0.0f, ty + tall * (bust ? 0.02f : 0.08f), dist, 0.0f, ty, 0.0f, 0.0f, 1.0f, 0.0f);
    rd.basemtx = cam;
    rd.mtxlist = (Mtxf *) dynAllocate(f->model->obj->numMatrices << 6);
    subcalcmatrices(&rd, f->model);
    rd.PropType = PROP_TYPE_EXPLOSION;
    rd.zbufferenabled = TRUE;
    rd.gdl = DL;
    rd.flags = 3;
    subdraw(&rd, f->model);
    DL = rd.gdl;
    modelSetDistanceDisabled(0);

    for (i = 0; i < f->model->obj->numMatrices; i++)
    {   /* the RSP reads them fixed point (as the cast screen does) */
        matrix_4x4_copy(&((Mtxf *) f->model->render_pos)[i], &tmp);
        matrix_4x4_f32_to_s32(tmp.m, (s32 (*)[4]) &((Mtxf *) f->model->render_pos)[i]);
    }

    /* the menu's own view back */
    gDPPipeSync(DL++);
    DL = viSetupCurrentPlayerView(DL);
    gDPSetScissor(DL++, G_SC_NON_INTERLACE, 0, 0, viGetX(), viGetY());
    return microcode_constructor(DL);
}
