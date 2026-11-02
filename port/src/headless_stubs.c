/**
 * Headless-build stubs (GE_HEADLESS=ON, e.g. the Linux AddressSanitizer
 * configuration): audio_pc.c and video.c need SDL2, which the -m32
 * sanitizer environment does not have. The game runs silent with the
 * null renderer; these satisfy the symbols their callers reference.
 */
#include <ultra64.h>
#include <PR/libaudio.h>

/* No sound out, but the synthesizer still runs: the sound player and the
 * sequencers only take their events and advance inside alAudioFrame, as
 * audio_pc.c's pump says for a missing device. Without it every event
 * posted stayed queued until the queue filled ("snd: event queue FULL"),
 * sounds never ended and music.c's wait for a stopped sequence spun out
 * its 512 tries. One frame per game field, as audio_pc.c with a device;
 * the command list is built and dropped, since nothing would hear it. */
static void headlessAudioFrame(void)
{
    extern u32 g_FrameSize;
    static Acmd cmdList[3072 * 4]; /* audio_pc.c's budget */
    static s16 frameBuf[1024 * 2];
    s32 cmdLen = 0;
    s32 samples = g_FrameSize != 0 ? (s32) g_FrameSize : 368;

    if (samples > 1024)
    {
        samples = 1024;
    }
    alAudioFrame(cmdList, &cmdLen, frameBuf, samples);
}

static s32 sInPump; /* as audio_pc.c: client handlers can re-enter the pump */

void portAudioPump(void)
{
    extern s32 currentFrameCounter;
    static s32 lastField, primed;
    s32 fields;

    if (sInPump || alGlobals == NULL || alGlobals->drvr.head == NULL)
    {
        return;
    }
    if (!primed)
    {
        primed = 1;
        lastField = currentFrameCounter;
    }
    fields = currentFrameCounter - lastField;
    lastField = currentFrameCounter;
    if (fields < 0 || fields > 16)
    {
        fields = 1;
    }
    sInPump = 1;
    while (fields-- > 0)
    {
        headlessAudioFrame();
    }
    sInPump = 0;
}

void portAudioPumpForce(void)
{
    if (sInPump || alGlobals == NULL || alGlobals->drvr.head == NULL)
    {
        return;
    }
    sInPump = 1;
    headlessAudioFrame();
    sInPump = 0;
}

/* blood_animation.c evicts CPU-animated textures from the fast3d cache;
 * no renderer, no cache. */
void gfx_texture_cache_delete(const unsigned char *orig_addr)
{
    (void)orig_addr;
}

/* texReset clears the whole fast3d texture cache at every stage load. */
void gfx_texture_cache_clear_c(void)
{
}

/* GE HW pacing: the renderer's per-frame RDP workload; no renderer, all zero. */
#include "../fast3d/gfx_api.h"
struct PortHwLoad g_PortHwLoadLast;

/* roc_hooks.c's voids for the room / part cache (gfx_pc.cpp); no renderer, no cache. */
void gfx_roc_queue_op(int op, int arg, const void *ptr)
{
    (void)op; (void)arg; (void)ptr;
}

/* GE_GFX_STATS tri counters, normally owned by video.c */
u32 ge_dbg0;
u32 ge_dbg1;
u32 ge_dbg2;

/* sky.c hands fast3d the sky-plane fan vertices via this to embed a G_NOOP
 * sequence marker; no renderer, no marker, just a stable non-zero sequence. */
uint32_t ge_sky_push(int n, const float *x, const float *y, const float *s, const float *t,
                      const float *iw, const float *r, const float *g, const float *b,
                      const float *a, float stmul)
{
    (void)n; (void)x; (void)y; (void)s; (void)t; (void)iw;
    (void)r; (void)g; (void)b; (void)a; (void)stmul;
    return 0;
}

/* midas.ini [display] getters, normally owned by video.c (GE_VIDEO-only).
 * Headless has no midas.ini to read, so return the same compiled-in defaults
 * video.c uses (see s_CfgFixShootThrough/s_CfgDistanceFading/etc. there). */
s32 portFixShootThrough(void)
{
    return 1;
}

s32 portDistanceFadingEnabled(void)
{
    return 0;
}

s32 portTrueEnemyRocketsEnabled(void)
{
    return 1;
}

s32 portGuardAiSmarter(void)
{
    return 0;
}

s32 portGuardBarks(void)
{
    return 1;
}

s32 portGuardAlertness(void)
{
    return 100;
}

s32 portGuardBudget(void)
{
    return 40;
}

s32 portCoopMenuClassic(void)
{
    return 0;
}

s32 portAdzyMode(void)
{
    return 0;
}

s32 portVideoScopeSteady(void)
{
    return 0;
}

/* midas.ini [input] mouse_sens_aim, normally owned by input_pc.c (GE_VIDEO-only). */
f32 portInputMouseAimScale(void)
{
    return 1.0f;
}

/* Added for the headless build to link again: each returns what video.c,
 * input_pc.c or gfx_sdl2.cpp give with no midas.ini and no pad attached. */

/* gfx_sdl2.cpp: a forced screenshot's path; no renderer, never taken */
int ge_shot_force;
char ge_shot_path[512];

/* video.c */
void portVideoKbdDigitalMove(s32 *dx, s32 *dy)
{
    *dx = 0;
    *dy = 0;
}

s32 portHudWideCfg(void)
{
    return -1; /* not set: the save's ratio stands */
}

void portHudWideAdopt(s32 wide)
{
    (void)wide;
}

s32 portCaptionsClassic(void)
{
    return 0;
}

s32 portPartnerFade(void)
{
    return 1;
}

void portVideoBindsResetDefaults(void)
{
}

s32 portPlayerCrosshairOwn(s32 player)
{
    (void)player;
    return -2; /* follow the global */
}

s32 portPlayerCrosshairColor(s32 player)
{
    (void)player;
    return -1; /* the global default */
}

void portPlayerCrosshairSet(s32 player, s32 rgb)
{
    (void)player; (void)rgb;
}

s32 portTextInputActive(void)
{
    return 0;
}

void portRequestQuit(void)
{
}

void portObjectivesHudToggle(void)
{
}

/* input_pc.c: no pads */
s32 portInputGlyphStyle(void)
{
    return 0;
}

s32 portInputDeviceModeRaw(s32 dev)
{
    (void)dev;
    return 0;
}

s32 portInputPadResetDefaults(s32 dev)
{
    (void)dev;
    return 0;
}

s32 portInputLayoutInUse(s32 dev)
{
    (void)dev;
    return -1;
}

const char *portInputLayoutShown(s32 dev)
{
    (void)dev;
    return "";
}
