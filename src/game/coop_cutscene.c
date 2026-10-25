/**
 * Co-op cutscenes (port only): the mission intro and the scripted cinemas
 * (the endings, and the few mid-mission ones) with every player in them.
 *
 * Each game plays its own intro, and its own copy of a scripted cinema
 * through the host's relayed camera commands (coop.c coopAiCommand). While
 * either runs, coop.c keeps the session going (coopCinemaTick: STATE, guard
 * snapshots, world, puppets). It used to stop until first-person play, so
 * the actors the host's script spawned, moved or animated stood still or
 * were missing on the joiners' screens, and nobody's partners were in the
 * shot.
 *
 *  - Intro: the host leaving its intro (its timer or a skip) ends everyone's
 *    (COOPEV_CINEMA), so the team starts playing together. During the Bond
 *    swirl the partners stand in a line beside our Bond and play his intro
 *    animation, where they used to be hidden inside him on the shared pad.
 *  - Scripted cinemas (CAMERAMODE_POSEND): the star is the player nearest the
 *    cinema's pad, the one the script reacted to. Every other game moves its
 *    Bond to a slot beside the star (standing up if it was down), so the
 *    cameras, placed around the star, frame everyone on every screen. The
 *    partners play our Bond's cinema animation, and hold no guns.
 *
 * Harness: GE_COOP_TESTCINEMA=<s>: the host runs a cinema <s> seconds into
 * the mission: a camera orbiting the joiner's Bond, so the joiner is the star,
 * for 10 s. GE_COOP_CINEMASHOT=<ticks>: a screenshot that often while the
 * swirl or a cinema runs, cine_p<player>_<n>.bmp beside the game.
 */
#include <ultra64.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include <boss.h>
#include "bondview.h"
#include "model.h"
#include "player.h"
#include "stan.h"
#include "coop.h"

extern void portConLogf(const char *fmt, ...);
extern int ge_shot_force;
extern char ge_shot_path[512];
f32 bondviewGetPlayerYawRadians(void);
void bondviewCalcIntroSwirlCamera(s32 index, f32 time, coord3d *pos, coord3d *lookat);
void chrSetWeaponFlag4(ChrRecord *self, s32 hand);
void coopCutsceneStandUp(coord3d *pos, StandTile *stan, f32 yaw);

/* COOPEV_CINEMA */
enum { COOPCINEMA_INTRODONE = 1 };
typedef struct
{
    u8 what;
    u8 pad[3];
    s32 stage;
} CoopCinemaMsg;

static s32 sPrevMode = -1;
static s32 sIntroSent;      /* host: told everyone our intro is over, this stage */
static s32 sHostIntroDone;  /* joiner: the host's intro is over, in this stage */
static s32 sStarSet;        /* this cinema's star picked (and we moved to it) */
static s32 sTestCinema = -2;
static s32 sTestCinemaEnd;

/* where the i-th partner stands beside the one in the middle: right, left,
 * then behind them on both sides (units, along their right and forward) */
static const f32 sSlotRight[3] = { 110.0f, -110.0f, 0.0f };
static const f32 sSlotBack[3] = { 0.0f, 0.0f, 130.0f };

static s32 coopModeIsIntro(s32 mode)
{
    return mode == CAMERAMODE_INTRO || mode == CAMERAMODE_FADESWIRL || mode == CAMERAMODE_SWIRL;
}

/* a model yaw (CoopStatePkt's) and a slot: the spot beside pos */
static void coopSlotPos(coord3d *out, const coord3d *pos, f32 yaw, s32 slot)
{
    f32 fx = sinf(yaw), fz = cosf(yaw);     /* forward */
    f32 rx = -fz, rz = fx;                  /* right */
    f32 r, b;

    slot = (slot < 0) ? 0 : slot % 3;
    r = sSlotRight[slot];
    b = sSlotBack[slot];
    *out = *pos;
    out->f[0] += rx * r - fx * b;
    out->f[2] += rz * r - fz * b;
}

/* a floor under the spot, not too far above or below where we stand */
static StandTile *coopSlotFloor(coord3d *spot, const coord3d *from)
{
    coord3d up = *spot;
    StandTile *stan;
    f32 y;

    up.f[1] = from->f[1] + 30.0f;
    stan = stanFindTileBelowPos(&up, NULL, &y);
    if (stan == NULL || from->f[1] - y > 400.0f)
    {
        return NULL;
    }
    spot->f[1] = from->f[1];
    return stan;
}

/* players in this stage, ours included, by player number: their positions
 * and yaws; returns how many */
static s32 coopCinemaPlayers(s32 *who, coord3d *pos, f32 *yaw)
{
    CoopHudPartner p;
    PropRecord *bond = getCurrentPlayerProp();
    s32 n = 0, i, j;

    if (bond != NULL)
    {
        who[n] = coopDesignSelf();
        pos[n] = bond->pos;
        yaw[n] = bondviewGetPlayerYawRadians();
        n++;
    }
    for (i = 0; n < COOP_MAX_PLAYERS && coopHudPartnerAt(i, &p); i++)
    {
        CoopExtView v;

        who[n] = p.player;
        pos[n] = p.pos;
        yaw[n] = coopExtRemoteView(p.player, &v) ? v.yaw : 0.0f;
        n++;
    }
    for (i = 1; i < n; i++)
    {   /* by player number, the same order on every game */
        for (j = i; j > 0 && who[j] < who[j - 1]; j--)
        {
            s32 tw = who[j];
            coord3d tp = pos[j];
            f32 ty = yaw[j];

            who[j] = who[j - 1];
            pos[j] = pos[j - 1];
            yaw[j] = yaw[j - 1];
            who[j - 1] = tw;
            pos[j - 1] = tp;
            yaw[j - 1] = ty;
        }
    }
    return n;
}

/* a scripted cinema began: if another player is its star, stand beside them
 * (1 once done, 0 to try again next tick: partners show up a tick late) */
static s32 coopCinemaJoinStar(void)
{
    s32 who[COOP_MAX_PLAYERS];
    coord3d pos[COOP_MAX_PLAYERS];
    f32 yaw[COOP_MAX_PLAYERS];
    coord3d focus, spot;
    s32 n, i, star = -1, slot = 0, self = coopDesignSelf(), k;
    f32 bestd = 0.0f;

    if (!coopCinemaFocus(&focus))
    {
        return 1;
    }
    n = coopCinemaPlayers(who, pos, yaw);
    for (i = 0; i < n; i++)
    {
        f32 dx = pos[i].f[0] - focus.f[0];
        f32 dy = pos[i].f[1] - focus.f[1];
        f32 dz = pos[i].f[2] - focus.f[2];
        f32 d = dx * dx + dy * dy + dz * dz;

        if (star < 0 || d < bestd)
        {
            star = i;
            bestd = d;
        }
    }
    if (star < 0 || n < 2)
    {
        return 0;
    }
    portConLogf("ge: coop: cutscene: player %d is the star (%d in it)\n", who[star] + 1, n);
    if (who[star] == self)
    {
        if (g_CurrentPlayer->bonddead)
        {
            coopCutsceneStandUp(&pos[star], getCurrentPlayerProp()->stan, 6.2831855f - yaw[star]);
        }
        return 1;
    }
    for (i = 0; i < n && who[i] != self; i++)
    {
        if (i != star)
        {
            slot++;     /* our place among the others */
        }
    }
    for (k = 0; k < 3; k++)
    {   /* our slot, else the next free-standing one */
        StandTile *stan;

        coopSlotPos(&spot, &pos[star], yaw[star], slot + k);
        stan = coopSlotFloor(&spot, &pos[star]);
        if (stan != NULL)
        {
            coopCutsceneStandUp(&spot, stan, 6.2831855f - yaw[star]);
            portConLogf("ge: coop: cutscene: moved beside the star, slot %d, at %.0f %.0f %.0f\n",
                        (slot + k) % 3, spot.f[0], spot.f[1], spot.f[2]);
            return 1;
        }
    }
    portConLogf("ge: coop: cutscene: no floor beside the star, staying put\n");
    return 1;
}

/* harness: the host shows a cinema orbiting the first partner */
static void coopCinemaTest(void)
{
    static u8 list[16];
    CoopHudPartner p;
    s32 best = -1, i, padnum;
    f32 bestd = 0.0f;

    if (sTestCinema == -2)
    {
        const char *e = getenv("GE_COOP_TESTCINEMA");

        sTestCinema = (e != NULL) ? atoi(e) * 60 : -1;
    }
    if (sTestCinemaEnd > 0 && g_CameraMode == CAMERAMODE_POSEND && --sTestCinemaEnd == 0)
    {   /* back to play: AI_CameraReturnToBond, AI_BondEnableControl */
        i = 0;
        list[i++] = AI_CameraReturnToBond;
        list[i++] = AI_BondEnableControl;
        list[i++] = AI_EndList;
        portConLogf("ge: coop: testcinema: back to play\n");
        coopRunHostScript(list);
        return;
    }
    if (sTestCinema <= 0 || !coopDesignIsHost() || g_CameraMode != CAMERAMODE_FP
        || (s32) coopExtMissionTicks() < sTestCinema || !coopHudPartner(&p))
    {
        return;
    }
    sTestCinema = -1;
    for (i = 0; g_CurrentSetup.pads != NULL && g_CurrentSetup.pads[i].plink != NULL && i < 0x2000; i++)
    {   /* the pad nearest the partner */
        f32 dx = g_CurrentSetup.pads[i].pos.f[0] - p.pos.f[0];
        f32 dz = g_CurrentSetup.pads[i].pos.f[2] - p.pos.f[2];

        if (best < 0 || dx * dx + dz * dz < bestd)
        {
            best = i;
            bestd = dx * dx + dz * dz;
        }
    }
    if (best < 0)
    {
        return;
    }
    padnum = best;
    portConLogf("ge: coop: testcinema: orbiting pad %d by player %d\n", padnum, p.player + 1);
    /* AI_CameraOrbitPad: lat dist, vert dist, speed, pad, y offset, start rotation; then AI_BondDisableControl */
    i = 0;
    list[i++] = AI_BondDisableControl;
    list[i++] = 0;
    list[i++] = AI_CameraOrbitPad;
    list[i++] = 0x01; list[i++] = 0x90;     /* 400 across */
    list[i++] = 0x00; list[i++] = 0x50;     /* 80 up */
    list[i++] = 0x00; list[i++] = 0x80;     /* slowly round */
    list[i++] = (u8) (padnum >> 8); list[i++] = (u8) padnum;
    list[i++] = 0x00; list[i++] = 0x00;
    list[i++] = 0x00; list[i++] = 0x00;
    list[i++] = AI_EndList;
    sTestCinemaEnd = 300;
    coopRunHostScript(list);
}

/* every tick in a stage (coop.c coopTickBody): 1 while the intro or a
 * scripted cinema runs */
s32 coopCutsceneTick(void)
{
    s32 mode = g_CameraMode;

    if (bossGetStageNum() == LEVELID_TITLE)
    {
        sPrevMode = -1;
        sIntroSent = 0;
        sHostIntroDone = 0;
        sStarSet = 0;
        return 0;
    }
    if (coopDesignIsHost() && !sIntroSent && coopModeIsIntro(sPrevMode) && !coopModeIsIntro(mode))
    {
        CoopCinemaMsg m;

        memset(&m, 0, sizeof(m));
        m.what = COOPCINEMA_INTRODONE;
        m.stage = bossGetStageNum();
        coopDesignSend(COOPEV_CINEMA, COOP_ALL_PLAYERS, &m, sizeof(m));
        sIntroSent = 1;
        portConLogf("ge: coop: cutscene: our intro is over, so is everyone's\n");
    }
    if (mode == CAMERAMODE_POSEND && sStarSet < 60)
    {   /* solo cinemas give up after a second */
        sStarSet = coopCinemaJoinStar() ? 60 : sStarSet + 1;
    }
    if (mode != CAMERAMODE_POSEND)
    {
        sStarSet = 0;
    }
    sPrevMode = mode;
    coopCinemaTest();
    {   /* GE_COOP_CINEMASHOT=<ticks> (harness only) */
        static s32 every = -2, t, n;

        if (every == -2)
        {
            const char *e = getenv("GE_COOP_CINEMASHOT");

            every = (e != NULL) ? atoi(e) : -1;
        }
        if (every > 0 && (mode == CAMERAMODE_SWIRL || mode == CAMERAMODE_POSEND) && (++t % every) == 0)
        {
            snprintf(ge_shot_path, sizeof(ge_shot_path), "cine_p%d_%02d.bmp", coopDesignSelf() + 1, n++);
            ge_shot_force = 1;
        }
    }
    return coopModeIsIntro(mode) || mode == CAMERAMODE_POSEND;
}

void coopCutsceneApply(const u8 *raw, s32 from)
{
    CoopCinemaMsg m;

    memcpy(&m, raw, sizeof(m));
    if (m.what == COOPCINEMA_INTRODONE && from == 0)
    {
        sHostIntroDone = m.stage;
        portConLogf("ge: coop: cutscene: the host's intro of stage %d is over\n", m.stage);
    }
}

s32 coopIntroHostDone(void)
{
    return !coopDesignIsHost() && sHostIntroDone != 0 && sHostIntroDone == bossGetStageNum()
        && coopModeIsIntro(g_CameraMode);
}

/* partners in the shot: the swirl and the scripted cinemas */
s32 coopCutsceneActive(void)
{
    return bossGetStageNum() != LEVELID_TITLE && (g_CameraMode == CAMERAMODE_SWIRL || g_CameraMode == CAMERAMODE_POSEND);
}

/* the swirl: the i-th partner stands in the i-th slot beside our Bond (they
 * all start on our pad); a scripted cinema already has everyone beside its
 * star, where they really are */
s32 coopCutscenePuppetPos(s32 player, coord3d *pos, f32 *yaw)
{
    PropRecord *bond = getCurrentPlayerProp();
    CoopHudPartner p;
    coord3d spot;
    s32 i;

    if (g_CameraMode != CAMERAMODE_SWIRL || bond == NULL)
    {
        return 0;
    }
    for (i = 0; coopHudPartnerAt(i, &p) && p.player != player; i++)
    {
    }
    *yaw = bondviewGetPlayerYawRadians();
    coopSlotPos(&spot, &bond->pos, *yaw, i);
    if (coopSlotFloor(&spot, &bond->pos) == NULL)
    {
        return 0;
    }
    *pos = spot;
    return 1;
}

/* the swirl's camera passes close to where a partner stands: hide them
 * rather than fill the screen with the inside of their jacket */
s32 coopCutsceneCameraNear(const coord3d *pos)
{
    coord3d cam, look;
    f32 dx, dy, dz;

    if (g_CameraMode != CAMERAMODE_SWIRL || g_IntroSwirl == NULL)
    {
        return 0;
    }
    bondviewCalcIntroSwirlCamera(intro_camera_index, camera_transition_timer, &cam, &look);
    dx = cam.f[0] - pos->f[0];
    dy = cam.f[1] - pos->f[1];
    dz = cam.f[2] - pos->f[2];
    return dx * dx + dz * dz < 80.0f * 80.0f && dy < 200.0f && dy > -200.0f;
}

/* a partner's puppet plays what our Bond's body plays in the cinema */
void coopCutscenePuppetAnim(ChrRecord *chr)
{
    Model *body = g_CurrentPlayer->bodyModel;
    ModelAnimation *anim;
    s32 h;

    for (h = GUNRIGHT; h <= GUNLEFT; h++)
    {
        if (chr->weapons_held[h] != NULL)
        {
            chrSetWeaponFlag4(chr, h);
        }
    }
    if (body == NULL || chr->model == NULL || (anim = objecthandlerGetModelAnim(body)) == NULL)
    {
        return;
    }
    if (objecthandlerGetModelAnim(chr->model) != anim)
    {
        modelSetAnimation(chr->model, anim, 0, modelGetAnimFrame(body), modelGetAnimSpeed(body), 0.0f);
    }
    else if (modelGetAnimFrame(chr->model) - modelGetAnimFrame(body) > 3.0f
             || modelGetAnimFrame(body) - modelGetAnimFrame(chr->model) > 3.0f)   /* no fabsf: see NOTES (x87) */
    {
        modelSetAnimFrame(chr->model, modelGetAnimFrame(body));
    }
}

void coopDesignApply(s32 kind, const u8 *raw, s32 from)
{
    switch (kind)
    {
        case COOPEV_CINEMA:
            coopCutsceneApply(raw, from);
            break;
        case COOPEV_RESULTS:
            coopResultsApply(raw, from);
            break;
    }
}
