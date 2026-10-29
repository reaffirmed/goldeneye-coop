/**
 * Partners' audio (port only). Everything uses the game's own sounds, at the
 * game's own sound effect and music volumes, and goes through the same
 * positional path as the guards' sounds (propobj.c chrobjSndCreatePostEvent:
 * distance volume, and the port's stereo pan that follows the listener).
 *
 *   - a partner's gun handling: drawing a gun (the pickup clack, or the
 *     knife's, mine's or laser's own) and reloading (the rifle cock), played
 *     at their puppet. Our gunfire.c plays these for our own Bond and calls
 *     coopAudioCue; the cue rides each STATE in CoopLook.cue (the byte that
 *     was padding, so the packet is the same size and an older build sends
 *     0, which plays nothing). Their gunfire, throws and impacts were
 *     already positional (coop_look.c through the guards' gun sound,
 *     gun.c gunCoopSpawnThrown).
 *   - a ping's click (BEEP) from where the ping is, out to 300 m, never
 *     quieter than a third, so one behind us is heard from that side.
 *   - when a partner goes down: the game's own short death sting on the X
 *     music track (the slot the alarm and exit music use), only while that
 *     track is free, faded out after 3 s; the level music is not touched.
 *     When they are back up (revived or respawned): the softer regen shimmer
 *     (OBJ_REGEN_SFX) at half volume.
 *
 * GoldenEye has no footstep sounds at all (neither Bond nor the guards make
 * any), so partners have none either.
 */
#include <ultra64.h>
#include <string.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include <music.h>
#include <snd.h>
#include "player.h"
#include "propobj.h"
#include "mp_music.h"
#include "coop.h"

extern void portConLogf(const char *fmt, ...);
extern s32 g_musicXTrack2CurrentTrackNum;   /* music.c: the X track's tune, 0 when none */
s32 sub_GAME_7F0537B8(f32 distance, f32 min, f32 max);  /* propobj.c: a distance's volume */
extern float sqrtf(float);

#define COOP_STING_TICKS    180     /* the downed sting plays 3 s, then fades */
#define COOP_PING_NEAR      6000.0f /* the ping click: full volume to 60 m */
#define COOP_PING_FAR       30000.0f
#define COOP_PING_MIN_VOL   0x2AAA  /* a third: never lost */

static u8 sCueSeq;
static u8 sCueKind;
static s32 sStingTicks;     /* the downed sting's ticks left, 0 none */
static s32 sStingTrack;     /* the tune we put on the X track */
static s8 sWasDown[COOP_MAX_PLAYERS];   /* per player: -1 unknown, else down last tick */
static s32 sWasLost[COOP_MAX_PLAYERS];

/* ---- gun handling ---- */

void coopAudioCue(s32 kind, s32 item)
{
    (void) item;    /* the others know the gun from CoopLook.weapon */
    if (g_CurrentPlayer == NULL || g_CurrentPlayer->bonddead)
    {
        return;     /* a downed Bond's hands cycle through draws we don't hear either */
    }
    sCueSeq = (u8) ((sCueSeq + 1) & 0x1F);
    sCueKind = (u8) kind;
}

u8 coopAudioCueByte(void)
{
    return (u8) (sCueSeq | (sCueKind << 5));
}

/* the sound gunfire.c plays for this draw, NOTHING_SFX for none */
static s32 coopAudioDrawSound(s32 item)
{
    switch (item)
    {
        case ITEM_LASER:
            return PICKUP_LASER_SFX;
        case ITEM_KNIFE:
        case ITEM_THROWKNIFE:
            return PICKUP_KNIFE_SFX;
        case ITEM_TIMEDMINE:
        case ITEM_PROXIMITYMINE:
        case ITEM_REMOTEMINE:
            return PICKUP_MINE_SFX;
        case ITEM_UNARMED:
        case ITEM_FIST:
        case ITEM_WATCHLASER:
        case ITEM_GRENADE:
        case ITEM_TRIGGER:
        case ITEM_TASER:
        case ITEM_TANKSHELLS:
        case ITEM_BOMBCASE:
        case ITEM_PLASTIQUE:
        case ITEM_CAMERA:
        case ITEM_BUG:
        case ITEM_MICROCAMERA:
        case ITEM_WATCHMAGNETATTRACT:
        case ITEM_GOLDENEYEKEY:
        case ITEM_TOKEN:
            return NOTHING_SFX;
        default:
            return PICKUP_GUN_SFX;
    }
}

static void coopAudioAt(s32 sfx, coord3d *pos)
{
    ALSoundState *state = sndPlaySfx((struct ALBankAlt_s *) g_musicSfxBufferPtr, (s16) sfx, NULL);

    if (state != NULL)
    {
        chrobjSndCreatePostEventDefault(state, pos);    /* as a guard's: by distance, panned */
    }
}

void coopAudioPuppetCue(CoopLookPuppet *lp, ChrRecord *chr, const CoopLook *look)
{
    const u8 seq = look->cue & 0x1F;
    const s32 kind = (look->cue >> 5) & 3;
    s32 sfx = NOTHING_SFX;

    if (!lp->cuevalid)
    {   /* a cue from before we saw them is old news */
        lp->cuevalid = 1;
        lp->cue = seq;
        return;
    }
    if (seq == lp->cue || chr == NULL || chr->prop == NULL)
    {
        return;
    }
    lp->cue = seq;
    portConLogf("ge: coop: audio: chr %d %s\n", chr->chrnum, (kind == COOPCUE_DRAW) ? "draws" : "reloads");
    if (kind == COOPCUE_DRAW)
    {
        sfx = coopAudioDrawSound(look->weapon[0]);
    }
    else if (kind == COOPCUE_RELOAD)
    {
        sfx = GUN_RIFLECOCK_SFX;
    }
    if (sfx != NOTHING_SFX)
    {
        coopAudioAt(sfx, &chr->prop->pos);
    }
}

void coopAudioAtPos(s32 sfx, coord3d *pos)
{
    coopAudioAt(sfx, pos);
}

/* ---- pings ---- */

void coopAudioPing(coord3d *pos, s32 self)
{
    ALSoundState *state = sndPlaySfx((struct ALBankAlt_s *) g_musicSfxBufferPtr, BEEP_SFX, NULL);
    PropRecord *bond = getCurrentPlayerProp();
    s32 vol;
    f32 dx, dy, dz;

    if (state == NULL || self || bond == NULL)
    {
        return;     /* our own: as before, straight in both ears */
    }
    dx = bond->pos.f[0] - pos->f[0];
    dy = bond->pos.f[1] - pos->f[1];
    dz = bond->pos.f[2] - pos->f[2];
    vol = sub_GAME_7F0537B8(sqrtf(dx * dx + dy * dy + dz * dz), COOP_PING_NEAR, COOP_PING_FAR);
    if (vol < COOP_PING_MIN_VOL)
    {
        vol = COOP_PING_MIN_VOL;
    }
    sndCreatePostEvent(state, 8, vol);
    portSndApplyPan(state, pos);
}

/* ---- stings ---- */

void coopAudioPartnerDowned(s32 player)
{
    portConLogf("ge: coop: audio: player %d down, sting %s\n", player + 1,
                (g_musicXTrack2CurrentTrackNum == 0 && !g_CurrentPlayer->bonddead) ? "on the X track" : "skipped (X track busy)");
    if (g_musicXTrack2CurrentTrackNum != 0 || g_CurrentPlayer->bonddead)
    {
        return;     /* the alarm or exit music, or our own death music, has the slot */
    }
    musicTrack2ApplySeqpVol(sub_GAME_7F0C0BF0());   /* the music volume */
    musicTrack2Play(M_SHORT_SOLO_DEATH);
    sStingTrack = M_SHORT_SOLO_DEATH;
    sStingTicks = COOP_STING_TICKS;
}

void coopAudioPartnerRevived(s32 player)
{
    ALSoundState *state = sndPlaySfx((struct ALBankAlt_s *) g_musicSfxBufferPtr, OBJ_REGEN_SFX, NULL);

    portConLogf("ge: coop: audio: player %d back up, revived sting\n", player + 1);
    if (state != NULL)
    {
        sndCreatePostEvent(state, 8, 0x4000);   /* half: the softer of the two */
    }
}

void coopAudioStageStart(void)
{
    memset(sWasDown, -1, sizeof(sWasDown));
    memset(sWasLost, 0, sizeof(sWasLost));
    sStingTicks = 0;
    sStingTrack = 0;
}

void coopAudioTick(void)
{
    CoopHudPartner p;
    s32 i;

    if (sStingTicks > 0)
    {
        sStingTicks--;
        if (g_musicXTrack2CurrentTrackNum != sStingTrack)
        {
            sStingTicks = 0;    /* the game put its own tune there since: leave it */
        }
        else if (sStingTicks == 30)
        {
            musicTrack2FadeOut(0.5f);
        }
        else if (sStingTicks == 0)
        {
            musicTrack2Stop();
        }
    }

    memset(&p, 0, sizeof(p));
    for (i = 0; coopHudPartnerAt(i, &p); i++)
    {
        const s32 k = p.player;
        const s32 lost = coopHudPlayerLost(k);

        if (k < 0 || k >= COOP_MAX_PLAYERS)
        {
            continue;
        }
        if (sWasDown[k] >= 0 && !lost && !sWasLost[k])
        {   /* a dropped connection is not a death or a revive */
            if (p.dead && !sWasDown[k])
            {
                coopAudioPartnerDowned(k);
            }
            else if (!p.dead && sWasDown[k])
            {
                coopAudioPartnerRevived(k);
            }
        }
        sWasDown[k] = (s8) (p.dead != 0);
        sWasLost[k] = lost;
    }
}
