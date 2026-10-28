/**
 * Photo mode: P in a mission (solo or co-op) hides the HUD and the gun and
 * frees the camera from Bond's eyes; P or Escape gives the view back.
 *
 *   - mouse (or the look stick) turns the camera, movement flies it the way
 *     it looks; it stays over floor Bond could walk on, up to 4 m above it,
 *     and within 8 m of Bond, so it is for framing a shot, not scouting
 *   - Fire saves the view as a .bmp in Photos next to the game (the hint is
 *     off the screen for the frames around it)
 *   - Bond's controls are locked meanwhile, but the world keeps going (in
 *     co-op it cannot stop for one player): guards still shoot. Dying, the
 *     watch, or the mission ending leaves photo mode.
 *
 * The level is unloaded by the time the end-of-mission screen is up, so the
 * photos are taken in the mission; that screen says how many and where. A
 * replay of the mission was the other idea: the game keeps no record of the
 * world to replay from, and recording every guard, door and shot for it is
 * far heavier than this.
 *
 * bondview2.c asks coopPhotoCamera / coopPhotoRoom / coopPhotoPos the same
 * way it asks coop_spectate.c, and maybe_mp_interface draws only
 * coopPhotoRender while it is on.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include <boss.h>
#include "textrelated.h"
#include "fr.h"
#include "player.h"
#include "othermodemicrocode.h"
#include "bondview.h"
#include "lv.h"
#include "stan.h"
#include "joy.h"
#include "coop.h"

#ifdef _WIN32
#include <direct.h>
#define PHOTO_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define PHOTO_MKDIR(p) mkdir(p, 0777)
#endif

extern void portConLogf(const char *fmt, ...);
extern const char *portRootPath(const char *name);
extern s32 portJoyPortPhotoPressed(void);
extern s32 checkGamePaused(void);
extern s32 getMissiontimer(void);
extern char *getenv(const char *);
extern int atoi(const char *);
extern f32 sinf(f32);
extern f32 cosf(f32);
extern f32 asinf(f32);
extern f32 atan2f(f32, f32);
extern f32 sqrtf(f32);
extern s32 ge_shot_force;       /* gfx_sdl2.cpp: save the next presented frame */
extern char ge_shot_path[512];

#define PHOTO_SPEED     5.0f    /* units a tick at full stick */
#define PHOTO_UP        400.0f  /* the most above the floor */
#define PHOTO_DOWN      15.0f   /* the least */
#define PHOTO_REACH     800.0f  /* from Bond's eyes */
#define PHOTO_HIDE      4       /* ticks the hint is off before the shot */
#define PHOTO_AFTER     4       /* and after */
#define PHOTO_SAVED     120     /* "Photo saved" */
#define DEG             0.017453292f

static s32 sOn;
static s32 sLocked;             /* we locked the controls (and unlock them) */
static coord3d sPos, sPos3;
static StandTile *sTile;
static f32 sTheta, sPitch;      /* degrees; theta as Bond's vv_theta, pitch up-positive */
static f32 sPitchSign = 1.0f;   /* Bond's vv_verta against up */
static s32 sShot;               /* ticks into a shot, 0 none */
static s32 sSaved;
static s32 sCount;              /* photos this mission */
static s32 sCountStage = -1;
static char sLast[96];

static s32 coopPhotoCanStart(void)
{
    return g_CurrentPlayer != NULL && g_CurrentPlayer->prop != NULL && !g_CurrentPlayer->bonddead
        && g_CurrentPlayer->cameramode == 0 && g_CameraMode == CAMERAMODE_FP
        && g_CurrentPlayer->watch_animation_state == 0 && !checkGamePaused()
        && !lvlGetControlsLockedFlag() && !coopChatTyping() && getPlayerCount() == 1;
}

static void coopPhotoStart(void)
{
    coord3d *v = &g_CurrentPlayer->field_488.applied_view;
    f32 len = sqrtf(v->f[0] * v->f[0] + v->f[1] * v->f[1] + v->f[2] * v->f[2]);

    sOn = 1;
    sShot = 0;
    sPos = g_CurrentPlayer->field_488.pos;
    sPos3 = g_CurrentPlayer->field_488.pos3;
    sTile = g_CurrentPlayer->field_488.current_tile_ptr_for_portals;
    if (sTile == NULL)
    {
        sTile = g_CurrentPlayer->prop->stan;
    }
    sTheta = g_CurrentPlayer->vv_theta;
    sPitch = (len > 0.01f) ? asinf(v->f[1] / len) / DEG : 0.0f;
    if (g_CurrentPlayer->vv_verta > 3.0f || g_CurrentPlayer->vv_verta < -3.0f)
    {
        sPitchSign = (g_CurrentPlayer->vv_verta * sPitch > 0.0f) ? 1.0f : -1.0f;
    }
    lvlSetControlsLockedFlag(TRUE);
    sLocked = 1;
    portConLogf("ge: photo: on at %.0f %.0f %.0f\n", sPos.f[0], sPos.f[1], sPos.f[2]);
}

void coopPhotoStop(void)
{
    if (!sOn)
    {
        return;
    }
    sOn = 0;
    sShot = 0;
    if (sLocked)
    {
        lvlSetControlsLockedFlag(FALSE);
        sLocked = 0;
    }
    portConLogf("ge: photo: off\n");
}

s32 coopPhotoActive(void)
{
    if (sOn && (g_CurrentPlayer == NULL || g_CurrentPlayer->bonddead || g_CameraMode != CAMERAMODE_FP
                || g_CurrentPlayer->cameramode != 0))
    {
        portConLogf("ge: photo: ended (dead %d, camera %d/%d)\n", g_CurrentPlayer ? g_CurrentPlayer->bonddead : -1,
                    (int) g_CameraMode, g_CurrentPlayer ? g_CurrentPlayer->cameramode : -1);
        coopPhotoStop();   /* down, or the mission is ending */
    }
    return sOn;
}

s32 coopPhotoCount(void)
{
    return (sCountStage >= 0) ? sCount : 0;
}

/* Photos/GE-<stage>-<date>-<time>.bmp beside the game */
static void coopPhotoSnap(void)
{
    char dir[400];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    static s32 serial;

    snprintf(dir, sizeof(dir), "%s", portRootPath("Photos"));
    PHOTO_MKDIR(dir);
    snprintf(sLast, sizeof(sLast), "GE-%02d-%04d%02d%02d-%02d%02d%02d-%d.bmp", (int) bossGetStageNum(),
             t ? t->tm_year + 1900 : 0, t ? t->tm_mon + 1 : 0, t ? t->tm_mday : 0, t ? t->tm_hour : 0,
             t ? t->tm_min : 0, t ? t->tm_sec : 0, (int) (serial++ % 10));
    snprintf(ge_shot_path, sizeof(ge_shot_path), "%s/%s", dir, sLast);
    ge_shot_force = 1;
    sCount++;
    portConLogf("ge: photo: saved Photos/%s\n", sLast);
}

/* the camera flies; xz only over floor it can walk to from where it is */
static void coopPhotoMove(f32 fwd, f32 side)
{
    const f32 ct = cosf(sTheta * DEG), st = sinf(sTheta * DEG);
    const f32 cp = cosf(sPitch * DEG), sp = sinf(sPitch * DEG);
    coord3d want = sPos;
    coord3d *eye = &g_CurrentPlayer->field_488.pos;
    StandTile *tile = sTile;
    f32 floor, dx, dy, dz;

    /* the view's right is (-cos, -sin) of theta: the world's x runs to the
     * left of a camera looking along +z */
    want.f[0] += (-st * cp * fwd - ct * side) * PHOTO_SPEED;
    want.f[1] += sp * fwd * PHOTO_SPEED;
    want.f[2] += (ct * cp * fwd - st * side) * PHOTO_SPEED;
    dx = want.f[0] - eye->f[0];
    dy = want.f[1] - eye->f[1];
    dz = want.f[2] - eye->f[2];
    if (dx * dx + dy * dy + dz * dz > PHOTO_REACH * PHOTO_REACH)
    {
        return;
    }
    if (tile != NULL && walkTilesBetweenPoints_NoCallback(&tile, sPos.f[0], sPos.f[2], want.f[0], want.f[2]) && tile != NULL)
    {
        sTile = tile;
        sPos.f[0] = want.f[0];
        sPos.f[2] = want.f[2];
    }
    floor = bondviewYPositionRelated(sTile, sPos.f[0], sPos.f[2]);
    sPos.f[1] = want.f[1];
    if (sPos.f[1] < floor + PHOTO_DOWN)
    {
        sPos.f[1] = floor + PHOTO_DOWN;
    }
    if (sPos.f[1] > floor + PHOTO_UP)
    {
        sPos.f[1] = floor + PHOTO_UP;
    }
    sPos3 = sPos;
    sPos3.f[1] = floor;
}

/* GE_PHOTO_TEST=<s>: <s> seconds into a mission, photo mode on; turn and fly
 * a little, take a photo, and leave again 3 s later (harness only) */
static void coopPhotoTestTick(f32 *dx, f32 *fwd, s32 *fire, s32 *toggle)
{
    static s32 at = -2;
    static s32 n;

    if (at == -2)
    {
        const char *e = getenv("GE_PHOTO_TEST");
        at = (e != NULL) ? atoi(e) * 60 : -1;
    }
    if (at < 0 || g_CameraMode != CAMERAMODE_FP)
    {
        return;
    }
    n++;
    if (n == at || n == at + 240)
    {
        *toggle = 1;
    }
    else if (n > at && n < at + 60)
    {
        *dx = 1.0f;     /* turn a degree a tick */
    }
    else if (n >= at + 60 && n < at + 120)
    {
        *fwd = -1.0f;   /* back off */
    }
    else if (n == at + 150)
    {
        *fire = 1;
    }
}

/* each tick (bondview2.c, player 1's look sample): the photo key, and while
 * on, the look and movement are the camera's */
void coopPhotoTick(f32 lookdx, f32 lookdy, s32 direct, f32 yawdeg, f32 pitchdeg)
{
    static s32 lasttimer;
    s32 toggle = portJoyPortPhotoPressed();
    s32 fire = 0;
    f32 fwd, side, tdx = 0.0f, tfwd = 0.0f;

    if (getMissiontimer() < lasttimer || sCountStage != bossGetStageNum())
    {   /* a new mission: its own count */
        sCountStage = bossGetStageNum();
        sCount = 0;
    }
    lasttimer = getMissiontimer();

    coopPhotoTestTick(&tdx, &tfwd, &fire, &toggle);
    if (sSaved > 0)
    {
        sSaved--;
    }
    if (!coopPhotoActive())
    {
        if (toggle && coopPhotoCanStart())
        {
            coopPhotoStart();
        }
        return;
    }
    if (toggle && sShot == 0)
    {
        coopPhotoStop();
        return;
    }
    if (sShot > 0)
    {   /* the hint is away; the shot in the middle of that */
        sShot++;
        if (sShot == PHOTO_HIDE)
        {
            coopPhotoSnap();
        }
        else if (sShot >= PHOTO_HIDE + PHOTO_AFTER)
        {
            sShot = 0;
            sSaved = PHOTO_SAVED;
        }
    }
    else if (fire || joyGetButtonsPressedThisFrame(0, Z_TRIG))
    {
        sShot = 1;
    }

    /* look: the direct mouse in degrees, else the scaled delta and the stick */
    if (direct)
    {
        sTheta += yawdeg;
        sPitch += pitchdeg * sPitchSign;
    }
    else
    {
        sTheta += lookdx * 2.0f;
        sPitch += lookdy * 2.0f * sPitchSign;
    }
    sTheta += tdx;
    if (sPitch > 85.0f)
    {
        sPitch = 85.0f;
    }
    if (sPitch < -85.0f)
    {
        sPitch = -85.0f;
    }

    /* movement: the stick, or the keys on the C buttons (WASD by default) */
    fwd = joyGetStickY(0) / 70.0f;
    side = joyGetStickX(0) / 70.0f;
    if (joyGetButtons(0, U_CBUTTONS)) fwd = 1.0f;
    if (joyGetButtons(0, D_CBUTTONS)) fwd = -1.0f;
    if (joyGetButtons(0, L_CBUTTONS)) side = -1.0f;
    if (joyGetButtons(0, R_CBUTTONS)) side = 1.0f;
    fwd += tfwd;
    fwd = (fwd > 1.0f) ? 1.0f : (fwd < -1.0f) ? -1.0f : fwd;
    side = (side > 1.0f) ? 1.0f : (side < -1.0f) ? -1.0f : side;
    if (fwd > 0.05f || fwd < -0.05f || side > 0.05f || side < -0.05f)
    {
        coopPhotoMove(fwd, side);
    }
}

s32 coopPhotoCamera(coord3d *pos, coord3d *look, coord3d *up)
{
    f32 cp;

    if (!coopPhotoActive())
    {
        return 0;
    }
    cp = cosf(sPitch * DEG);
    look->f[0] = -sinf(sTheta * DEG) * cp;
    look->f[1] = sinf(sPitch * DEG);
    look->f[2] = cosf(sTheta * DEG) * cp;
    up->f[0] = 0.0f;
    up->f[1] = 1.0f;
    up->f[2] = 0.0f;
    *pos = sPos;
    return 1;
}

s32 coopPhotoRoom(u8 *room)
{
    if (!coopPhotoActive() || sTile == NULL)
    {
        return 0;
    }
    *room = sTile->room;
    return 1;
}

coord3d *coopPhotoPos(s32 three)
{
    if (!coopPhotoActive())
    {
        return NULL;
    }
    return three ? &sPos3 : &sPos;
}

static Gfx *coopPhotoPrint(Gfx *gdl, s32 y, const char *text)
{
    s32 th = 0, tw = 0;
    s32 x;

    textMeasure(&th, &tw, (char *) text, ptrFontBankGothicChars, ptrFontBankGothic, 0);
    x = viGetViewLeft() + (viGetViewWidth() - tw) / 2;
    return textRenderOutlined(gdl, &x, &y, (char *) text, ptrFontBankGothicChars, ptrFontBankGothic, 0xFFFFFFC0, 0x000000A0,
                              (s32) viGetX(), viGetY(), 0, 0);
}

/* maybe_mp_interface while photo mode is on: the hint, nothing else */
Gfx *coopPhotoRender(Gfx *gdl)
{
    char text[128];
    const s32 y = viGetViewTop() + viGetViewHeight() - 0x18;

    if (sShot > 0)
    {
        return gdl;     /* in the photo: nothing on it */
    }
    gdl = microcode_constructor(gdl);
    if (sSaved > 0)
    {
        snprintf(text, sizeof(text), "PHOTO SAVED IN PHOTOS (%d THIS MISSION)\n", (int) coopPhotoCount());
        gdl = coopPhotoPrint(gdl, y - 0x0C, text);
    }
    gdl = coopPhotoPrint(gdl, y, "PHOTO MODE - FIRE: TAKE A PHOTO - P: BACK\n");
    return combiner_bayer_lod_perspective(gdl);
}
