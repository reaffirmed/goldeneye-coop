/**
 * Story co-op: how partners look on everyone else's game.
 *
 *   - the character each player picked on the lobby (the multiplayer roster,
 *     mp_chr_setup), or Bond in the mission's own outfit, the default. The
 *     pick is kept in coop_character.txt next to the game. A puppet is built
 *     from the body and head in its STATE, and rebuilt if they change.
 *   - the guns in their hands: the puppet holds the items its player holds
 *     (both for a pair), and moves with the multiplayer body animations
 *     (bondview2.c playerTick's remote-player block, ported here): the one
 *     handed, two handed, unarmed and dual groups, idle, walk, run,
 *     sidestep and crouch, aimed up, down and across like the player.
 *   - their shots: the muzzle flash while their own flash shows, and per
 *     shot the gun's sound, a tracer from the puppet's muzzle to where the
 *     bullet stopped, and the impact there: spark, ricochet and the
 *     surface's sound and bullet hole on walls, the flesh sound on chrs, the
 *     glass sound on unbroken glass. Damage stays where it always was (the
 *     shooter's game, and the host for guards); this is only what you see
 *     and hear.
 *
 * Each game fills a CoopLook for its own Bond into every STATE. The newest
 * COOP_SHOTS_KEPT shots ride along with a sequence number, so a lost STATE
 * costs nothing unless more shots than that were fired in between.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include "bondview.h"
#include "chr.h"
#include "chraction.h"
#include "model.h"
#include "initanitable.h"
#include "stan.h"
#include "math_atan2f.h"
#include "player.h"
#include "propobj.h"
#include "objecthandler.h"
#include "gun.h"
#include "glass.h"
#include "explosion.h"
#include <random.h>
#include "front.h"
#include "coop.h"

extern void portConLogf(const char *fmt, ...);
extern const char *portRootPath(const char *name);
extern s32 playerGetCrouchPos(s32 playernum);
extern PROP getPropForHeldItem(ITEM_IDS item);
extern void sub_GAME_7F02BFE4(ChrRecord *self, s32 hand, s32 play);
extern void chrSetFiring(ChrRecord *self, s32 hand, s32 firing);
extern struct firing_anim_struct firing_animation_groups[][6];
extern s32 g_bondviewBondDeathAnimations[];
extern s32 g_bondviewBondDeathAnimationsCount;
extern struct MP_selectable_chars mp_chr_setup[];
extern s32 portMpCharAt(s32 pos);
extern s32 portMpCharPos(s32 c);

/* the game's math.h has no isfinite: NaN fails the first test, infinity the second */
#define coopFinite(x) ((x) == (x) && ((x) - (x)) == 0.0f)

#define COOP_CHAR_FILE  "coop_character.txt"
#define COOP_CHAR_COUNT (PORT_MP_CHAR_COUNT + 1)    /* the mission's Bond, then the roster */
#define COOP_MISS_RANGE 3000.0f                     /* a shot that hit nothing: the tracer's length */

/* ---- the lobby's character pick ------------------------------------------ */

static s32 sCharacter = -1;     /* -1 not loaded yet */

s32 coopCharacter(void)
{
    if (sCharacter < 0)
    {
        const char *e = getenv("GE_COOP_CHAR");     /* harness: an mp_chr_setup index, -1 the mission's Bond */
        FILE *f;

        sCharacter = 0;
        if (e != NULL)
        {
            sCharacter = atoi(e) + 1;
        }
        else if ((f = fopen(portRootPath(COOP_CHAR_FILE), "r")) != NULL)
        {
            int v = 0;

            if (fscanf(f, "%d", &v) == 1)
            {
                sCharacter = v;
            }
            fclose(f);
        }
        if (sCharacter < 0 || sCharacter >= COOP_CHAR_COUNT)
        {
            sCharacter = 0;
        }
    }
    return sCharacter;
}

void coopSetCharacter(s32 c)
{
    FILE *f;

    if (c < 0 || c >= COOP_CHAR_COUNT)
    {
        c = 0;
    }
    sCharacter = c;
    if (getenv("GE_COOP_CHAR") != NULL)
    {
        return; /* the harness's pick is not saved */
    }
    if ((f = fopen(portRootPath(COOP_CHAR_FILE), "w")) != NULL)
    {
        fprintf(f, "%d\n", (int) c);
        fclose(f);
    }
}

/* the pickers' position of a pick: the mission's Bond first, then the MP order */
static s32 coopCharacterPos(s32 c)
{
    return (c <= 0) ? 0 : portMpCharPos(c - 1) + 1;
}

s32 coopCharacterStep(s32 c, s32 dir)
{
    s32 pos = (coopCharacterPos(c) + dir + COOP_CHAR_COUNT) % COOP_CHAR_COUNT;

    return (pos == 0) ? 0 : portMpCharAt(pos - 1) + 1;
}

void coopCharacterName(s32 c, char *out, s32 cap)
{
    if (c <= 0 || c >= COOP_CHAR_COUNT)
    {
        snprintf(out, cap, "Mission Bond\n");
        return;
    }
    snprintf(out, cap, "%s", portMpCharName(c - 1));
    if (strchr(out, '\n') == NULL && (s32) strlen(out) < cap - 1)
    {
        strcat(out, "\n");
    }
}

s32 coopCharacterBody(s32 c, s32 *head)
{
    if (c <= 0 || c >= COOP_CHAR_COUNT)
    {
        return -1;
    }
    *head = mp_chr_setup[c - 1].head;
    return mp_chr_setup[c - 1].body;
}

/* ---- our side: what goes in our STATE ------------------------------------ */

static CoopShot sShots[COOP_SHOTS_KEPT];  /* newest first */
static u8 sShotSeq;
static CoopShot sCur;
static s32 sCurOpen;

void coopLookFill(CoopLook *out)
{
    s32 h;

    memset(out, 0, sizeof(*out));
    for (h = GUNRIGHT; h <= GUNLEFT; h++)
    {
        out->weapon[h] = (s8) getCurrentPlayerWeaponId(h);
        if (g_CurrentPlayer->hands[h].field_87D)
        {
            out->flash |= 1 << h;
        }
    }
    out->crouch = (u8) playerGetCrouchPos(get_cur_playernum());
    out->speedforwards = g_CurrentPlayer->speedforwards;
    out->speedsideways = g_CurrentPlayer->speedsideways;
    out->speedtheta = g_CurrentPlayer->speedtheta;
    out->aimpitch = g_CurrentPlayer->field_2A08;
    out->aimyaw = g_CurrentPlayer->field_2A0C;
    out->headanim = g_CurrentPlayer->headanim != 0;
    out->character = (u8) coopCharacter();
    out->cue = coopAudioCueByte();      /* drawing and reloading, for the others to hear (coop_audio.c) */
    out->shotseq = sShotSeq;
    memcpy(out->shot, sShots, sizeof(sShots));
}

/* a bullet leaves our gun: by default it flies COOP_MISS_RANGE and hits nothing */
void coopLookShotBegin(s32 hand, s32 weapon, coord3d *from, coord3d *dir)
{
    if (!coopFinite(dir->f[0]) || !coopFinite(dir->f[1]) || !coopFinite(dir->f[2])
        || !coopFinite(from->f[0]) || !coopFinite(from->f[1]) || !coopFinite(from->f[2]))
    {
        sCurOpen = 0;   /* no path to show: a NaN tracer would blank the partners' screens */
        return;
    }
    coopObjNoteActor(coopExtSelf(), 1);   /* our own shot: a kill now is ours (coop_objective.c) */
    memset(&sCur, 0, sizeof(sCur));
    sCur.kind = COOPSHOT_MISS | ((hand == GUNLEFT) ? COOPSHOT_LEFT : 0);
    sCur.weapon = (s8) weapon;
    sCur.arg = -1;
    sCur.impact = -1;
    sCur.pos[0] = from->f[0] + dir->f[0] * COOP_MISS_RANGE;
    sCur.pos[1] = from->f[1] + dir->f[1] * COOP_MISS_RANGE;
    sCur.pos[2] = from->f[2] + dir->f[2] * COOP_MISS_RANGE;
    sCurOpen = 1;
}

/* where it stopped; like the tracer (gunSetTracerTarget), the last hit wins */
static void coopLookShotAt(s32 kind, coord3d *pos, s32 room)
{
    sCur.kind = (sCur.kind & COOPSHOT_LEFT) | kind;
    sCur.pos[0] = pos->f[0];
    sCur.pos[1] = pos->f[1];
    sCur.pos[2] = pos->f[2];
    sCur.room = (s16) room;
}

void coopLookShotBg(coord3d *pos, coord3d *normal, s32 impact, s32 room, s32 texture, s32 spark)
{
    s32 i;

    if (!sCurOpen)
    {
        return;
    }
    coopLookShotAt(COOPSHOT_BG, pos, room);
    sCur.arg = (s16) texture;
    sCur.impact = (s16) impact;
    sCur.flags = spark ? COOPSHOTF_SPARK : 0;
    for (i = 0; i < 3; i++)
    {
        f32 n = (normal != NULL) ? normal->f[i] : 0.0f;

        sCur.normal[i] = (s8) (n * 127.0f);
    }
}

void coopLookShotChr(PropRecord *prop, coord3d *pos)
{
    if (!sCurOpen || prop == NULL || prop->chr == NULL)
    {
        return;
    }
    coopLookShotAt(COOPSHOT_CHR, pos, (prop->stan != NULL) ? prop->stan->room : 0);
    sCur.arg = prop->chr->chrnum;
    sCur.impact = -1;
    sCur.flags = 0;
}

void coopLookShotObj(PropRecord *prop, coord3d *pos, s32 room, s32 texture, s32 glass)
{
    if (!sCurOpen)
    {
        return;
    }
    coopLookShotAt(COOPSHOT_OBJ, pos, room);
    sCur.arg = (s16) texture;
    sCur.impact = -1;
    sCur.flags = COOPSHOTF_SPARK | (glass ? COOPSHOTF_GLASS : 0);
    (void) prop;
}

void coopLookShotEnd(void)
{
    if (!sCurOpen)
    {
        return;
    }
    sCurOpen = 0;
    memmove(&sShots[1], &sShots[0], sizeof(sShots[0]) * (COOP_SHOTS_KEPT - 1));
    sShots[0] = sCur;
    sShotSeq++;
}

/* ---- their side: the puppet ---------------------------------------------- */

void coopLookReset(CoopLookPuppet *lp)
{
    memset(lp, 0, sizeof(*lp));
    lp->held[GUNRIGHT] = lp->held[GUNLEFT] = ITEM_UNARMED;
}

/* the item the gun in that hand is, ITEM_UNARMED for none */
static s32 coopLookHeld(ChrRecord *chr, s32 hand)
{
    PropRecord *prop = chr->weapons_held[hand];

    return (prop != NULL && prop->weapon != NULL) ? prop->weapon->weaponnum : ITEM_UNARMED;
}

/* give the puppet the guns its player holds; an old one goes first (its
 * chrTick frees it), the new one the tick after */
static void coopLookGuns(CoopLookPuppet *lp, ChrRecord *chr, const CoopLook *look, s32 dead)
{
    s32 h;

    for (h = GUNRIGHT; h <= GUNLEFT; h++)
    {
        s32 want = dead ? ITEM_UNARMED : look->weapon[h];
        s32 model = (want > ITEM_UNARMED) ? (s32) getPropForHeldItem(want) : -1;
        s32 have = coopLookHeld(chr, h);

        if (model < 0)
        {
            want = ITEM_UNARMED;  /* fists, the watch, mission gadgets with no held model */
        }
        if (have == want)
        {
            lp->held[h] = (s8) have;
            continue;
        }
        if (chr->weapons_held[h] != NULL)
        {
            chrSetWeaponFlag4(chr, h);
            continue;
        }
        if (want != ITEM_UNARMED)
        {
            chrGiveWeapon(chr, model, want, (h == GUNLEFT) ? PROPFLAG_WEAPON_LEFTHANDED : 0);
            portConLogf("ge: coop: puppet %d holds item %d in the %s hand\n", chr->chrnum, want, (h == GUNLEFT) ? "left" : "right");
        }
        lp->held[h] = (s8) coopLookHeld(chr, h);
    }
}

static s32 coopLookIsDeathAnim(s32 anim)
{
    s32 i;

    for (i = 0; i < g_bondviewBondDeathAnimationsCount; i++)
    {
        if (anim == g_bondviewBondDeathAnimations[i] + (s32) ptr_animation_table)
        {
            return 1;
        }
    }
    return 0;
}

/* bondview2.c playerTick's block for a remote multiplayer Bond, with the
 * player's movement from its STATE in place of its player struct */
static s32 sTestTicks;
static void coopLookAnimate(CoopLookPuppet *lp, ChrRecord *chr, const CoopLook *look, s32 dead)
{
    Model *model = chr->model;
    PropRecord *leftprop = chrGetEquippedWeaponProp(chr, GUNLEFT);
    PropRecord *rightprop = chrGetEquippedWeaponProp(chr, GUNRIGHT);
    WeaponObjRecord *leftobj = (leftprop != NULL) ? leftprop->weapon : NULL;
    WeaponObjRecord *rightobj = (rightprop != NULL) ? rightprop->weapon : NULL;
    struct weapon_firing_animation_table *firingtable = NULL;
    f32 pitch = look->aimpitch;
    f32 yaw = look->aimyaw;
    f32 rate = 1.0f;
    f32 frame = -1.0f;
    f32 endframe = -1.0f;
    s32 anim = 0;
    s32 group, sub;
    s32 setanim;

    if (dead)
    {
        anim = coopLookIsDeathAnim(lp->anim) ? lp->anim
             : g_bondviewBondDeathAnimations[randomGetNext() % g_bondviewBondDeathAnimationsCount] + (s32) ptr_animation_table;
        rate = 0.5f;
        pitch = 0.0f;
        yaw = 0.0f;
    }
    else
    {
        struct firing_anim_struct *fa;

        if (leftprop != NULL && rightprop != NULL)
        {
            group = 3;
        }
        else if (leftprop == NULL && rightprop == NULL)
        {
            group = 2;
        }
        else if (leftobj != NULL && !bondwalkItemCheckBitflags(leftobj->weaponnum, WEAPONSTATBITFLAG_HOLD_AS_GUN))
        {
            group = 2;
        }
        else if (rightobj != NULL && !bondwalkItemCheckBitflags(rightobj->weaponnum, WEAPONSTATBITFLAG_HOLD_AS_GUN))
        {
            group = 2;
        }
        else if (leftobj != NULL && bondwalkItemCheckBitflags(leftobj->weaponnum, WEAPONSTATBITFLAG_ONLY_1_HANDED))
        {
            group = 0;
        }
        else if (rightobj != NULL && bondwalkItemCheckBitflags(rightobj->weaponnum, WEAPONSTATBITFLAG_ONLY_1_HANDED))
        {
            group = 0;
        }
        else
        {
            group = 1;
        }

        if (look->crouch <= 1)
        {
            rate = 1.0f;
            sub = 5;
        }
        else if (look->speedsideways < 0.0f && firing_animation_groups[group][4].pointer != NULL)
        {
            sub = 4;
            rate = -look->speedsideways;
            if (lp->lean < 90.0f)
            {
                lp->lean += 15.0f;
            }
        }
        else if (look->speedsideways > 0.0f && firing_animation_groups[group][3].pointer != NULL)
        {
            sub = 3;
            rate = look->speedsideways;
            if (lp->lean > -90.0f)
            {
                lp->lean -= 15.0f;
            }
        }
        else
        {
            f32 turn = (look->speedtheta < 0.0f) ? -look->speedtheta : look->speedtheta;
            f32 fwd = look->speedforwards;

            if (fwd < -0.05f)
            {   /* backwards: the walk or run played in reverse */
                f32 f = (fwd < -turn) ? fwd : -turn;

                if (look->headanim && fwd <= -0.4f)
                {
                    sub = 2;
                    rate = f;
                }
                else
                {
                    sub = 1;
                    rate = f + f;
                    if (rate < -1.0f)
                    {
                        rate = -1.0f;
                    }
                }
            }
            else if (fwd > 0.05f || turn > 0.05f)
            {
                f32 f = (turn < fwd) ? fwd : turn;

                if (look->headanim && fwd >= 0.4f)
                {
                    sub = 2;
                    rate = f;
                }
                else
                {
                    sub = 1;
                    rate = f + f;
                    if (rate > 1.0f)
                    {
                        rate = 1.0f;
                    }
                }
            }
            else
            {
                sub = 0;
                rate = 1.0f;
            }
            /* the lean comes back to the centre */
            if (lp->lean > 0.0f)
            {
                lp->lean -= 15.0f;
            }
            if (lp->lean < 0.0f)
            {
                lp->lean += 15.0f;
            }
        }

        fa = &firing_animation_groups[group][sub];
        firingtable = fa->pointer;
        if (fa->anim != 0)
        {
            anim = (s32) fa->anim + (s32) ptr_animation_table;
        }
        rate *= fa->x;
        endframe = fa->z;
        frame = fa->y;
        if (firingtable != NULL && anim == 0)
        {
            anim = *(s32 *) firingtable;
        }
    }

    setanim = (anim != lp->anim);
    if (frame >= 0.0f && (!model->animlooping || frame != model->animloopframe))
    {
        setanim = 1;
    }
    if (frame < 0.0f && model->animlooping)
    {
        setanim = 1;
    }
    if (setanim && anim != 0)
    {
        if (model->anim2 == NULL)
        {
            modelSetAnimation(model, (ModelAnimation *) anim, 0, (frame >= 0.0f) ? frame : 0.0f, rate, 16.0f);
            lp->anim = anim;
            lp->rate = rate;
            if (frame >= 0.0f)
            {
                modelSetAnimLooping(model, frame, 16.0f);
            }
            if (endframe >= 0.0f)
            {
                modelSetAnimEndFrame(model, endframe);
            }
        }
    }
    else if (rate != lp->rate)
    {
        modelSetAnimSpeed(model, rate, 1.0f);
        lp->rate = rate;
    }

    if (anim == lp->anim)
    {
        if (firingtable != NULL)
        {
            chr->hidden &= ~CHRHIDDEN_0400;
            chrlvUpdateAimendbackShoulders(chr, firingtable, 0, 1, pitch);
        }
        else
        {
            chr->aimendrshoulder = 0.0f;
            chr->aimendlshoulder = 0.0f;
            chr->hidden |= CHRHIDDEN_0400;
            chr->aimendback = pitch;
        }
    }
    chr->aimendsideback = yaw;
    chr->aimendcount = 10;
    chr->actiontype = ACT_BONDMULTI;
    chr->act_bondmulti.unk2c = firingtable;
}

/* one of their shots: sound, tracer and what it hit */
static void coopLookPlayShot(ChrRecord *chr, const CoopShot *s, s32 self)
{
    s32 hand = (s->kind & COOPSHOT_LEFT) ? GUNLEFT : GUNRIGHT;
    s32 kind = s->kind & ~COOPSHOT_LEFT;
    PropRecord *gun = chrGetEquippedWeaponProp(chr, hand);
    coord3d from, end, dir, spark, normal;
    f32 len;

    if (gun == NULL)
    {   /* a dual shot before the second gun is in its hand: the right one's */
        hand = GUNRIGHT;
        gun = chrGetEquippedWeaponProp(chr, hand);
    }
    if (gun == NULL || chr->prop == NULL)
    {
        return;
    }
    if (!coopFinite(s->pos[0]) || !coopFinite(s->pos[1]) || !coopFinite(s->pos[2]))
    {
        return;
    }
    end.f[0] = s->pos[0];
    end.f[1] = s->pos[1];
    end.f[2] = s->pos[2];
    if (!sub_GAME_7F02D630(chr, hand, &from))
    {
        from.f[0] = chr->prop->pos.f[0];
        from.f[1] = chr->prop->pos.f[1] + 30.0f;
        from.f[2] = chr->prop->pos.f[2];
    }

    sub_GAME_7F02BFE4(chr, hand, 1);        /* the gun's sound, as a guard's */
    CapBeamLengthAndDecideIfRendered(&chr->beams[hand], s->weapon, &from, &end);

    dir.f[0] = end.f[0] - from.f[0];
    dir.f[1] = end.f[1] - from.f[1];
    dir.f[2] = end.f[2] - from.f[2];
    len = sqrtf(dir.f[0] * dir.f[0] + dir.f[1] * dir.f[1] + dir.f[2] * dir.f[2]);
    if (len < 1.0f)
    {
        len = 1.0f;
    }
    /* the spark sits a little in front of the surface, toward the shooter, as the shooter's does */
    spark.f[0] = end.f[0] - 26.0f * dir.f[0] / len;
    spark.f[1] = end.f[1] - 26.0f * dir.f[1] / len;
    spark.f[2] = end.f[2] - 26.0f * dir.f[2] / len;

    switch (kind)
    {
        case COOPSHOT_BG:
            if (s->flags & COOPSHOTF_SPARK)
            {
                recall_joy2_hits_edit_flag(s->weapon, &end, s->arg);
                if (s->impact >= 0)
                {
                    normal.f[0] = s->normal[0] / 127.0f;
                    normal.f[1] = s->normal[1] / 127.0f;
                    normal.f[2] = s->normal[2] / 127.0f;
                    explosionCreateBulletImpact(&end, &normal, s->impact, s->room, NULL, -1, 0);
                }
                bullet_spark_create(&spark, 1, 26.0f, s->room);
            }
            break;
        case COOPSHOT_CHR:
        {   /* the flesh sound and a puff on whoever they hit, unless that is us */
            ChrRecord *hit = (s->arg == self) ? NULL : chrFindByLiteralId(s->arg);

            if (hit != NULL && hit->prop != NULL)
            {
                recall_joy2_hits_edit_detail_edit_flag(s->weapon, hit->prop, -1);
                bullet_spark_create(&end, 2, 20.0f, s->room);
            }
            break;
        }
        case COOPSHOT_OBJ:
            if (s->flags & COOPSHOTF_GLASS)
            {
                sub_GAME_7F064720(&end);
            }
            else
            {
                recall_joy2_hits_edit_flag(s->weapon, &end, s->arg);
            }
            bullet_spark_create(&end, 1, 26.0f, s->room);
            break;
        default:
            break;
    }
}

static void coopLookShots(CoopLookPuppet *lp, ChrRecord *chr, const CoopLook *look, s32 self)
{
    s32 n, i;

    if (!lp->seqvalid)
    {   /* shots from before we saw them are old news */
        lp->seqvalid = 1;
        lp->shotseq = look->shotseq;
        return;
    }
    n = (u8) (look->shotseq - lp->shotseq);
    if (n > 128)
    {
        n = 0; /* an older STATE than the last: nothing new */
    }
    if (n > COOP_SHOTS_KEPT)
    {
        n = COOP_SHOTS_KEPT;  /* more than a STATE keeps: the newest */
    }
    for (i = n - 1; i >= 0; i--)
    {
        coopLookPlayShot(chr, &look->shot[i], self);
    }
    if (n > 0)
    {
        lp->shotseq = look->shotseq;
    }
}

/* every tick, for each puppet; self is the chrnum our own Bond goes by on
 * the other games, so their shots at us draw no puff on our screen */
void coopLookPuppetTick(CoopLookPuppet *lp, ChrRecord *chr, const CoopLook *look, s32 dead, s32 self)
{
    s32 h;

    if (chr == NULL || chr->model == NULL)
    {
        return;
    }
    chr->hidden &= ~CHRHIDDEN_FIRE_TRACER;  /* the sound's repeat gate (sub_GAME_7F02BFE4), per tick */
    coopLookGuns(lp, chr, look, dead);
    coopLookAnimate(lp, chr, look, dead);
    for (h = GUNRIGHT; h <= GUNLEFT; h++)
    {
        chrSetFiring(chr, h, !dead && (look->flash & (1 << h)) != 0);
    }
    if (!dead)
    {
        coopLookShots(lp, chr, look, self);
    }
    else
    {
        lp->shotseq = look->shotseq;
    }
    coopAudioPuppetCue(lp, chr, look);  /* their draw and reload sounds, from where they stand (coop_audio.c) */
}

/* ---- test harness (coop.c coopHarnessTick, bondview2.c MoveBond) ---------
 * GE_COOP_TESTFIRE=<item>:<s>[:<len>]: draw that gun with 100 more rounds 3 s
 * before <s> seconds into the mission, then hold fire for <len> seconds
 * (default 4), pulsed so a pistol fires too.
 * GE_COOP_TESTLOOK=<s>: at <s> seconds step 4 m back from the first partner,
 * and from then on keep facing them. */
static s32 sTestFireItem = -2;
static s32 sTestFireAt;
static s32 sTestFireLen;
static s32 sTestLookAt = -2;
static s32 sTestLookDone;

static void coopLookTestParse(void)
{
    const char *f = getenv("GE_COOP_TESTFIRE");
    const char *l = getenv("GE_COOP_TESTLOOK");

    sTestFireItem = -1;
    if (f != NULL && strchr(f, ':') != NULL)
    {
        const char *t = strchr(f, ':') + 1;

        sTestFireItem = atoi(f);
        sTestFireAt = atoi(t) * 60;
        sTestFireLen = (strchr(t, ':') != NULL) ? atoi(strchr(t, ':') + 1) * 60 : 240;
    }
    sTestLookAt = (l != NULL) ? atoi(l) * 60 : -1;
    portConLogf("ge: coop: testfire item %d at %d, testlook at %d\n", sTestFireItem, sTestFireAt, sTestLookAt);
}

extern void coopGiveTestDraw(s32 weapon);

void coopLookTestTick(s32 missionticks)
{
    CoopHudPartner p;
    PropRecord *bond = getCurrentPlayerProp();

    if (sTestFireItem == -2)
    {
        coopLookTestParse();
    }
    sTestTicks = missionticks;
    if (sTestFireItem > 0 && missionticks == sTestFireAt - 180)
    {
        coopGiveTestDraw(sTestFireItem);
    }
    if (sTestFireItem > 0 && missionticks == sTestFireAt)
    {
        portConLogf("ge: coop: testfire: firing item %d for %d ticks\n", sTestFireItem, sTestFireLen);
    }
    if (sTestLookAt < 0 || missionticks < sTestLookAt || bond == NULL || !coopHudPartner(&p))
    {
        return;
    }
    if (!sTestLookDone)
    {
        sTestLookDone = 1;
        f32 dx = bond->pos.f[0] - p.pos.f[0];
        f32 dz = bond->pos.f[2] - p.pos.f[2];
        f32 d = sqrtf(dx * dx + dz * dz);
        coord3d to;
        StandTile *stan;

        if (d < 1.0f)
        {
            dx = 0.0f;
            dz = 1.0f;
            d = 1.0f;
        }
        s32 k;

        for (k = 0, stan = NULL; k < 4 && stan == NULL; k++)
        {   /* back from them, else to a side, else forward */
            f32 ax = (k == 0) ? dx / d : (k == 1) ? dz / d : (k == 2) ? -dz / d : -dx / d;
            f32 az = (k == 0) ? dz / d : (k == 1) ? -dx / d : (k == 2) ? dx / d : -dz / d;

            to.f[0] = p.pos.f[0] + ax * 400.0f;
            to.f[1] = p.pos.f[1] + 50.0f;
            to.f[2] = p.pos.f[2] + az * 400.0f;
            stan = stanFindTileBelowPos(&to, NULL, NULL);
        }
        if (stan != NULL)
        {
            coopRespawnPlayerAt(&to, stan, 0.0f, getCurrentPlayerWeaponId(GUNRIGHT));
            portConLogf("ge: coop: testlook: stepped back to %.0f %.0f %.0f\n", to.f[0], to.f[1], to.f[2]);
        }
        else
        {
            portConLogf("ge: coop: testlook: no floor at %.0f %.0f %.0f\n", to.f[0], to.f[1], to.f[2]);
        }
    }
    {
        f32 dx = p.pos.f[0] - bond->pos.f[0];
        f32 dz = p.pos.f[2] - bond->pos.f[2];

        g_CurrentPlayer->vv_theta = -(atan2f(dx, dz) * 180.0f) / M_PI_F;
        if (g_CurrentPlayer->vv_theta < 0.0f)
        {
            g_CurrentPlayer->vv_theta += 360.0f;
        }
    }
    if ((missionticks - sTestLookAt) % 60 == 30)
    {   /* what auto-aim holds while the partner is centred (none, partner or chr <n>) */
        PropRecord *ax = g_CurrentPlayer->autoaim_target_x;
        PropRecord *ay = g_CurrentPlayer->autoaim_target_y;

        portConLogf("ge: coop: testlook: autoaim x %s %d y %s %d (enabled %d)\n",
            ax == NULL ? "none" : coopPropIsPartner(ax) ? "partner" : "chr", (ax != NULL && ax->chr != NULL) ? ax->chr->chrnum : -1,
            ay == NULL ? "none" : coopPropIsPartner(ay) ? "partner" : "chr", (ay != NULL && ay->chr != NULL) ? ay->chr->chrnum : -1,
            (s32) currentPlayerGetYAutoAimEnabledRedirect());
    }
}

u16 coopLookTestButtons(u16 buttons)
{
    if (sTestFireItem > 0 && sTestTicks >= sTestFireAt && sTestTicks < sTestFireAt + sTestFireLen
        && ((sTestTicks - sTestFireAt) % 16) < 8)
    {
        buttons |= Z_TRIG;
    }
    return buttons;
}
