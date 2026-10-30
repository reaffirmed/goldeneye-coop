/**
 * Story co-op: things thrown, planted and dropped, in every player's world.
 *
 * Thrown items: a mine, grenade, covert modem, tracker bug, camera, plastique,
 * bomb case or key that we throw is replayed in each partner's game. The
 * throw (start, velocity, orientation, spin and fuse) rides two reliable
 * events (COOPEV_THROW, COOPEV_THROWMORE) and gun.c's gunCoopSpawnThrown
 * launches the same object from the same point, so it flies, sticks to the
 * same wall and counts for a deposit objective there too. Copies are
 * ordinary objects of that world: a guard walking onto a copy of a proximity
 * mine sets it off, a shot detonates it, and the host's copy is the one whose
 * blast hurts guards (a joiner's blasts never do).
 *
 * Dropped guns: the host owns the guards, so when one drops its gun there
 * (death, surrender, a shot to the hand) COOPEV_DROP has every joiner's
 * mirrored guard drop the same gun; a joiner's guards never drop on their
 * own.
 *
 * A shot that knocks one of these items about (objBounce) happens only in the
 * shooter's game, with a random kick, so COOPEV_ITEMKNOCK sends the kick
 * itself (its speed and tumble) and every copy flies the same way; once the
 * shooter's copy is still, where it lies goes out as a COOPEV_DROPREST.
 *
 * Each such item has an id (the player who made it, a serial). Whichever
 * copy goes off or is picked up first sends COOPEV_ITEMGONE; the others go
 * off with it, or leave the world. A dropped gun picked up by one player is
 * picked up by everyone (the pickup rule for weapons); a thrown item picked
 * up goes to that player only.
 *
 * Each world's copy of a blast hurts that world's own Bond, as any blast
 * does (standing on a partner's mine hurts); our watch only sets off our
 * own remote mines.
 *
 * Online versus uses the throws too (a copy is owned by the thrower's body,
 * so a kill is theirs), and the drops for the guns a dead player leaves; a
 * dropped gun goes to whoever picks it up, not to everyone.
 */
#include <ultra64.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include <boss.h>
#include "player.h"
#include "bondview.h"
#include "bondinv.h"
#include "chr.h"
#include "propobj.h"
#include "quaternion.h"
#include "math_atan2f.h"
#include "lv.h"
#include "coop.h"

extern void portConLogf(const char *fmt, ...);
extern ObjectRecord *gunCoopSpawnThrown(s32 item, coord3d *pos, Mtxf *rot, coord3d *velocity, Mtxf *spin, s32 timer, PropRecord *owner);
extern ObjectRecord *gunCoopSpawnRound(s32 item, coord3d *pos, Mtxf *rot, coord3d *velocity, coord3d *thrust, s32 timer, s32 guard, PropRecord *owner);
extern void objFreePermanently(ObjectRecord *obj, bool freeprop);
extern void chrpropReparent(PropRecord *newChild, PropRecord *host);
extern void chrpropDeregisterRooms(PropRecord *prop);
extern void chrpropDelist(PropRecord *prop);
extern void chrpropDisable(PropRecord *prop);
extern void propExecuteTickOperation(PropRecord *prop, TICKOP op);
extern TICKOP propPickupByPlayer(PropRecord *prop, bool showstring);
extern ChrRecord *chrFindByLiteralId(s32 chrnum);
extern void propobjSetDropped(PropRecord *prop, DROPTYPE droptype);
extern s32 objDrop(PropRecord *prop);
extern void gunInitProjectileObject(ObjectRecord *obj, coord3d *pos, StandTile *stan, Mtxf *matrix, coord3d *velocity, Mtxf *arg5, PropRecord *owner);
extern StandTile *stanFindTileBelowPos(coord3d *pos, u8 *rooms, f32 *yRtn);
extern void setupUpdateObjectRoomPosition(ObjectRecord *obj);
extern void matrix_4x4_set_identity(Mtxf *mtx);
extern void projectileFree(Projectile *projectile);
extern bool objCoopEmbed(PropRecord *prop, PropRecord *parent);
extern void objBounce(ObjectRecord *obj, coord3d *arg1);
extern PropRecord *hatCreateForChr(ChrRecord *chr, s32 modelnum, u32 flags);

enum { COOPITEM_THROWN = 1, COOPITEM_DROP = 2, COOPITEM_ROUND = 3 /* a launcher round: the thrower's goes off, the others' only show it in flight */ };
enum { COOPTHROW_ROUND = 1, COOPTHROW_GUARD = 2, COOPTHROW_POWERED = 4 };   /* CoopThrowMsg flags */
enum { COOPGONE_BOOM = 1, COOPGONE_PICKED = 2 };

typedef struct
{
    u16 id;
    u8 item;            /* ITEM_IDS */
    u8 flags;           /* COOPTHROW_*: a launcher round, a guard's, a rocket under power (an older build sends 0) */
    f32 pos[3];         /* where it leaves the hand */
    f32 vel[3];
} CoopThrowMsg;         /* COOPEV_THROW */

typedef struct
{
    u16 id;
    s16 timer;          /* the fuse, as the thrower's game set it */
    s16 rot[4];         /* its orientation, a quaternion x 32767 */
    s16 spin[4];        /* its tumble per tick; a powered rocket's thrust x 1000 */
    s32 stage;
    s16 chrnum;         /* COOPTHROW_GUARD: the host's guard that fired it */
    s16 pad;
} CoopThrowMoreMsg;     /* COOPEV_THROWMORE (an older build sent the first 24 bytes) */

typedef struct
{
    u16 id;
    u8 how;             /* COOPGONE_* */
    u8 pad;
    s32 stage;
} CoopGoneMsg;          /* COOPEV_ITEMGONE */

typedef struct
{
    u16 id;
    s16 chrnum;         /* the guard that dropped it */
    s8 item;            /* ITEM_IDS */
    u8 pad;
    s16 model;          /* PROP_ */
    s32 stage;
    f32 pos[3];         /* where it was, for a guard we do not have */
    s16 fuse;           /* 1 + the fuse a live grenade has, 0 none (an older host sends 0) */
    s8 droptype;        /* DROPTYPE_*: a thrown grenade flies, a gun falls; 0 default */
    u8 pad2;
} CoopDropMsg;          /* COOPEV_DROP (28 bytes) */

typedef struct
{
    u16 id;
    s16 rot[4];         /* the orientation as a quaternion, x 32767 */
    u16 pad;
    f32 pos[3];
} CoopDropRestMsg;      /* COOPEV_DROPREST: the host's copy came to rest here (28 bytes) */

typedef struct
{
    u16 id;
    s16 rot[3];         /* how it left the hand: x y z of a quaternion with w >= 0, x 32767 */
    s16 spin[3];        /* its tumble per tick, the same way */
    s16 speed[3];       /* x 1000 */
    s16 ofs[3];         /* where it starts, from the guard's position in the DROP before it */
    u8 stage;           /* low byte */
    u8 pad;
} CoopDropStartMsg;     /* COOPEV_DROPSTART, right after its COOPEV_DROP (28 bytes) */

typedef struct
{
    u16 id;
    s16 rot[4];         /* the orientation as a quaternion, x 32767 */
    u16 pad;
    s32 objofs;         /* the setup object it stuck to, -1 it landed loose */
    f32 pos[3];
} CoopStuckMsg;         /* COOPEV_ITEMSTUCK: where the thrower's own copy ended up (28 bytes) */

typedef struct
{
    u16 id;
    s16 rot[4];         /* its tumble per tick, a quaternion x 32767 */
    s16 speed[3];       /* the kick, x 1000 */
    s32 stage;
} CoopKnockMsg;         /* COOPEV_ITEMKNOCK: our shot sent it flying (20 bytes) */

#define COOP_ITEMS_MAX 64

typedef struct
{
    u16 id;
    u8 used;
    u8 kind;            /* COOPITEM_* */
    u8 gone;            /* a partner's copy went off: ours is going off with it */
    u8 rested;          /* host, a guard's drop: where it came to rest has been sent */
    u8 still;           /* host: ticks it has not moved */
    u8 settled;         /* a throw: ours, where it ended up has been sent; a partner's, their word on it came */
    u8 embedfix;        /* stuck to an object off our screen: its matrix is worked out when that is drawn */
    u8 knocked;         /* our shot sent it flying: where it comes to rest is to be sent */
    u16 age;            /* ticks since the throw */
    f32 last[3];        /* host: where it was last tick */
    s32 item;
    ObjectRecord *obj;
} CoopItem;

static CoopItem sItems[COOP_ITEMS_MAX];
static u16 sSerial;
static CoopThrowMsg sPendingThrow;  /* the first half of a throw, until its second arrives */
static s32 sPendingFrom = -1;
static s32 sApplying;               /* a partner's throw, drop or pickup is being played here */
static ObjectRecord *sExploding;    /* propExplode is setting this item off */
static s32 sExplodingOwner;         /* 1 + the player whose item it is, 0 ours */
static s32 sExplodingOurRound;      /* propExplode is setting off a launcher round our Bond fired */


static s32 coopItemsOn(void)
{
    return coopEnabled() && coopSessionActive();
}

/* the throws worth seeing: everything that is thrown as an object. A knife hurts
 * by touch, in the thrower's game only: the others' copies are made without the
 * flag that lets a knife in flight cut (gunCoopSpawnThrown) */
static s32 coopItemsThrowable(s32 item)
{
    switch (item)
    {
        case ITEM_THROWKNIFE:
        case ITEM_GRENADE:
        case ITEM_REMOTEMINE:
        case ITEM_PROXIMITYMINE:
        case ITEM_TIMEDMINE:
        case ITEM_BOMBCASE:
        case ITEM_BUG:
        case ITEM_MICROCAMERA:
        case ITEM_GOLDENEYEKEY:
        case ITEM_PLASTIQUE:
            return 1;
    }
    return 0;
}

/* the record still holds the object it was given (objects are reused) */
static s32 coopItemLive(CoopItem *it)
{
    return it->used && it->obj != NULL && it->obj->prop != NULL && it->obj->prop->obj == it->obj
        && ((WeaponObjRecord *) it->obj)->weaponnum == it->item;
}

static CoopItem *coopItemById(u16 id)
{
    s32 i;

    for (i = 0; i < COOP_ITEMS_MAX; i++)
    {
        if (sItems[i].used && sItems[i].id == id)
        {
            if (!coopItemLive(&sItems[i]))
            {
                sItems[i].used = 0;
                return NULL;
            }
            return &sItems[i];
        }
    }
    return NULL;
}

static CoopItem *coopItemByObj(ObjectRecord *obj)
{
    s32 i;

    for (i = 0; obj != NULL && i < COOP_ITEMS_MAX; i++)
    {
        if (sItems[i].used && sItems[i].obj == obj && coopItemLive(&sItems[i]))
        {
            return &sItems[i];
        }
    }
    return NULL;
}

static void coopItemAdd(u16 id, s32 kind, ObjectRecord *obj)
{
    s32 i, free = -1;

    for (i = 0; i < COOP_ITEMS_MAX; i++)
    {
        if (!sItems[i].used || !coopItemLive(&sItems[i]))
        {
            sItems[i].used = 0;
            if (free < 0)
            {
                free = i;
            }
        }
    }
    if (free < 0)
    {
        free = sSerial % COOP_ITEMS_MAX;   /* full: the oldest is likely long gone */
    }
    sItems[free].used = 1;
    sItems[free].gone = 0;
    sItems[free].rested = 0;
    sItems[free].still = 0;
    sItems[free].settled = 0;
    sItems[free].embedfix = 0;
    sItems[free].knocked = 0;
    sItems[free].age = 0;
    sItems[free].id = id;
    sItems[free].kind = (u8) kind;
    sItems[free].item = ((WeaponObjRecord *) obj)->weaponnum;
    sItems[free].obj = obj;
}

static u16 coopItemNewId(void)
{
    sSerial = (u16) ((sSerial + 1) & 0x3FFF);
    if (sSerial == 0)
    {
        sSerial = 1;
    }
    return (u16) ((coopExtSelf() & 3) << 14) | sSerial;
}

static void coopQuatPack(Mtxf *m, s16 *out)
{
    f32 q[4];
    s32 i;

    quaternion_from_matrix(m->m, q);
    for (i = 0; i < 4; i++)
    {
        f32 v = q[i];

        v = (v > 1.0f) ? 1.0f : (v < -1.0f) ? -1.0f : v;
        out[i] = (s16) (v * 32767.0f);
    }
}

static void coopQuatUnpack(const s16 *in, Mtxf *m)
{
    f32 q[4];
    s32 i;

    for (i = 0; i < 4; i++)
    {
        q[i] = (f32) in[i] / 32767.0f;
    }
    if (q[0] == 0.0f && q[1] == 0.0f && q[2] == 0.0f && q[3] == 0.0f)
    {
        q[0] = 1.0f;
    }
    quaternion_to_matrix(q, m->m);
}

void coopItemsStageStart(void)
{
    memset(sItems, 0, sizeof(sItems));
    sPendingFrom = -1;
    sExploding = NULL;
}

/* ---- our throws ------------------------------------------------------------ */

/* gun.c gunInitProjectileFromPlayer: we are throwing obj from pos */
void coopItemsThrown(ObjectRecord *obj, coord3d *pos, Mtxf *rot, coord3d *vel, Mtxf *spin)
{
    WeaponObjRecord *w = (WeaponObjRecord *) obj;
    CoopThrowMsg a;
    CoopThrowMoreMsg b;

    if (sApplying || !coopItemsOn() || obj == NULL || !coopItemsThrowable(w->weaponnum))
    {
        return;
    }
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.id = b.id = coopItemNewId();
    a.item = (u8) w->weaponnum;
    a.pos[0] = pos->f[0];
    a.pos[1] = pos->f[1];
    a.pos[2] = pos->f[2];
    a.vel[0] = vel->f[0];
    a.vel[1] = vel->f[1];
    a.vel[2] = vel->f[2];
    b.timer = w->timer;
    coopQuatPack(rot, b.rot);
    coopQuatPack(spin, b.spin);
    b.stage = bossGetStageNum();
    coopItemAdd(a.id, COOPITEM_THROWN, obj);
    coopExtSend(COOPEV_THROW, 0xFF, &a, sizeof(a));
    coopExtSend(COOPEV_THROWMORE, 0xFF, &b, sizeof(b));
    portConLogf("ge: coop: threw item %d (id %04x) at %.0f %.0f %.0f\n", a.item, a.id, a.pos[0], a.pos[1], a.pos[2]);
}

/* gun.c (our rockets, grenade-launcher rounds and tank shells) and chraction.c
 * (the host's guards' rounds): obj was launched from pos. Everyone else's game
 * shows it in flight; only this one's goes off (its blast is sent as any is,
 * in versus as VSBOOM), so an opponent sees a rocket coming and can dodge it */
void coopItemsLaunched(ObjectRecord *obj, coord3d *pos, Mtxf *rot, coord3d *vel, coord3d *thrust, ChrRecord *guard)
{
    WeaponObjRecord *w = (WeaponObjRecord *) obj;
    CoopThrowMsg a;
    CoopThrowMoreMsg b;
    Mtxf r;
    s32 k;

    if (sApplying || !coopItemsOn() || obj == NULL
        || (w->weaponnum != ITEM_ROCKETROUND && w->weaponnum != ITEM_GRENADEROUND)
        || (guard != NULL && (coopVersus() || !coopExtIsHost() || coopPropIsPartner(guard->prop))))
    {
        return;
    }
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.id = b.id = coopItemNewId();
    a.item = (u8) w->weaponnum;
    a.flags = COOPTHROW_ROUND | (guard != NULL ? COOPTHROW_GUARD : 0) | (thrust != NULL ? COOPTHROW_POWERED : 0);
    for (k = 0; k < 3; k++)
    {
        a.pos[k] = pos->f[k];
        a.vel[k] = vel->f[k];
        if (thrust != NULL)
        {
            f32 t = thrust->f[k] * 1000.0f;
            b.spin[k] = (s16) ((t > 32767.0f) ? 32767.0f : (t < -32767.0f) ? -32767.0f : t);
        }
    }
    b.timer = (s16) w->timer;
    r = *rot;
    r.m[3][0] = r.m[3][1] = r.m[3][2] = 0.0f;
    coopQuatPack(&r, b.rot);
    b.stage = bossGetStageNum();
    b.chrnum = (guard != NULL) ? guard->chrnum : -1;
    coopItemAdd(a.id, COOPITEM_ROUND, obj);
    coopExtSend(COOPEV_THROW, 0xFF, &a, sizeof(a));
    coopExtSend(COOPEV_THROWMORE, 0xFF, &b, sizeof(b));
    portConLogf("ge: coop: %s %d launched round %d (id %04x) at %.0f %.0f %.0f\n", guard != NULL ? "guard" : "we, player",
                guard != NULL ? guard->chrnum : coopExtSelf() + 1, a.item, a.id, a.pos[0], a.pos[1], a.pos[2]);
}

/* propobj.c: a partner's (or the host's guard's) round we only show. 1: it
 * does no harm, and goes without a blast */
s32 coopItemsShownOnly(ObjectRecord *obj)
{
    CoopItem *it;

    if (!coopItemsOn() || obj == NULL)
    {
        return 0;
    }
    it = coopItemByObj(obj);
    return it != NULL && it->kind == COOPITEM_ROUND && (it->id >> 14) != (u16) coopExtSelf();
}

/* propobj.c chrobjWeaponTick: one of those rounds. It goes when it hits
 * something or when the real one went off (ITEMGONE), and after 10 s at most */
s32 coopItemsShownOnlyTick(PropRecord *prop)
{
    WeaponObjRecord *w;
    CoopItem *it;

    if (prop == NULL || !coopItemsShownOnly(prop->obj))
    {
        return 0;
    }
    it = coopItemByObj(prop->obj);
    w = (WeaponObjRecord *) prop->obj;
    if (w->weaponnum == ITEM_GRENADEROUND && w->timer > 0)
    {
        w->timer -= g_ClockTimer;
        if (w->timer < 0)
        {
            w->timer = 0;
        }
    }
    if (it->age < 0xFFFF)
    {
        it->age += (u16) ((g_ClockTimer > 0) ? g_ClockTimer : 1);
    }
    if (w->timer == 0 || it->gone || it->age > 600)
    {
        w->timer = -1;
        prop->obj->runtime_bitflags |= RUNTIMEBITFLAG_REMOVE;
        it->used = 0;
        portConLogf("ge: coop: round %04x shown here is gone after %d ticks%s\n", it->id, it->age, it->gone ? ", the real one went off" : "");
    }
    return 1;
}

static void coopItemsApplyThrow(const CoopThrowMoreMsg *b, s32 from)
{
    CoopExtView view;
    PropRecord *owner = NULL;
    ObjectRecord *obj;
    coord3d pos, vel;
    Mtxf rot, spin;

    if (sPendingFrom != from || sPendingThrow.id != b->id)
    {
        return; /* the first half went missing (another stage) */
    }
    sPendingFrom = -1;
    if (b->stage != bossGetStageNum() || coopItemById(b->id) != NULL)
    {
        return;
    }
    if (coopVersus())
    {   /* their body, and their player number here for whose kill it is */
        s32 li = coopVsLocalOfNet(from);

        owner = (li > 0 && g_playerPointers[li] != NULL) ? g_playerPointers[li]->prop : NULL;
    }
    else if (coopExtRemoteView(from, &view))
    {
        owner = view.prop;
    }
    pos.f[0] = sPendingThrow.pos[0];
    pos.f[1] = sPendingThrow.pos[1];
    pos.f[2] = sPendingThrow.pos[2];
    vel.f[0] = sPendingThrow.vel[0];
    vel.f[1] = sPendingThrow.vel[1];
    vel.f[2] = sPendingThrow.vel[2];
    coopQuatUnpack(b->rot, &rot);
    if (sPendingThrow.flags & COOPTHROW_ROUND)
    {   /* a launcher round: ours flies and smokes, and goes when theirs goes off
         * (their blast comes as its own event) or when it hits something */
        coord3d thrust;
        ChrRecord *chr;

        if (coopVersus() && (owner == NULL || (sPendingThrow.flags & COOPTHROW_GUARD)))
        {
            return;     /* (versus: a player's, flying from their body) */
        }
        if ((sPendingThrow.flags & COOPTHROW_GUARD) && (chr = chrFindByLiteralId(b->chrnum)) != NULL)
        {
            owner = chr->prop;
        }
        thrust.f[0] = (sPendingThrow.flags & COOPTHROW_POWERED) ? b->spin[0] / 1000.0f : 0.0f;
        thrust.f[1] = (sPendingThrow.flags & COOPTHROW_POWERED) ? b->spin[1] / 1000.0f : 0.0f;
        thrust.f[2] = (sPendingThrow.flags & COOPTHROW_POWERED) ? b->spin[2] / 1000.0f : 0.0f;
        sApplying = 1;
        obj = gunCoopSpawnRound(sPendingThrow.item, &pos, &rot, &vel, (sPendingThrow.flags & COOPTHROW_POWERED) ? &thrust : NULL,
                                b->timer, (sPendingThrow.flags & COOPTHROW_GUARD) != 0, owner);
        sApplying = 0;
        if (obj == NULL)
        {
            return;
        }
        coopItemAdd(b->id, COOPITEM_ROUND, obj);
        portConLogf("ge: coop: %s %d's round %d (id %04x) flies here from %.0f %.0f %.0f\n", (sPendingThrow.flags & COOPTHROW_GUARD) ? "guard" : "player",
                    (sPendingThrow.flags & COOPTHROW_GUARD) ? b->chrnum : from + 1, sPendingThrow.item, b->id, pos.f[0], pos.f[1], pos.f[2]);
        return;
    }
    coopQuatUnpack(b->spin, &spin);
    sApplying = 1;
    obj = gunCoopSpawnThrown(sPendingThrow.item, &pos, &rot, &vel, &spin, b->timer, owner);
    sApplying = 0;
    if (obj == NULL)
    {
        portConLogf("ge: coop: could not replay player %d's throw (item %d)\n", from + 1, sPendingThrow.item);
        return;
    }
    if (coopVersus() && coopVsLocalOfNet(from) > 0)
    {   /* propExplode credits its owner; our detonator leaves it alone */
        obj->runtime_bitflags &= ~RUNTIMEBITFLAG_OWNER;
        obj->runtime_bitflags |= (u32) coopVsLocalOfNet(from) << RUNTIMEBITSHIFT_OWNER;
    }
    coopItemAdd(b->id, COOPITEM_THROWN, obj);
    portConLogf("ge: coop: player %d threw item %d (id %04x) at %.0f %.0f %.0f\n", from + 1, sPendingThrow.item, b->id,
                pos.f[0], pos.f[1], pos.f[2]);
}

/* ---- guns the host's guards drop -------------------------------------------- */

static void coopItemsMtxToQuat(Mtxf *mtx, f32 scale, f32 q[4]);
static void coopItemsQuatToMtx(const f32 q[4], f32 scale, Mtxf *mtx);
static f32 coopItemsMtxScale(Mtxf *mtx);
static s16 coopItemsPackSpeed(f32 v);
static s32 coopItemsAirborne(ObjectRecord *obj);

static f32 sDropAt[3];      /* joiner: the guard's position in the last DROP (COOPEV_DROPSTART's origin) */
static u16 sDropAtId;

/* a rotation as three shorts: the quaternion's x y z with w >= 0 */
static void coopItemsPackRot(Mtxf *mtx, f32 scale, s16 out[3])
{
    f32 q[4];
    s32 k;

    coopItemsMtxToQuat(mtx, scale, q);
    for (k = 0; k < 3; k++)
    {
        out[k] = (s16) (((q[3] < 0.0f) ? -q[k] : q[k]) * 32767.0f);
    }
}

static void coopItemsUnpackRot(const s16 in[3], f32 scale, Mtxf *mtx)
{
    f32 q[4];
    f32 w;
    s32 k;

    for (k = 0; k < 3; k++)
    {
        q[k] = (f32) in[k] / 32767.0f;
    }
    w = 1.0f - q[0] * q[0] - q[1] * q[1] - q[2] * q[2];
    q[3] = (w > 0.0f) ? sqrtf(w) : 0.0f;
    coopItemsQuatToMtx(q, scale, mtx);
}

/* host (versus: the dropper): how the drop just left the hand (objDrop's own rolls, and the hand
 * where the host's screen drew it), so every copy falls the same way rather than
 * each game's own until the host's resting place comes (COOPEV_DROPREST) */
static void coopItemsSendDropStart(u16 id, ObjectRecord *obj, const f32 *at)
{
    CoopDropStartMsg d;
    Projectile *pr = obj->projectile;
    s32 k;

    if (!(obj->runtime_bitflags & RUNTIMEBITFLAG_HASPROJECTILE) || pr == NULL || obj->prop == NULL)
    {
        return;
    }
    memset(&d, 0, sizeof(d));
    d.id = id;
    coopItemsPackRot(&obj->mtx, coopItemsMtxScale(&obj->mtx), d.rot);
    coopItemsPackRot(&pr->mtx, coopItemsMtxScale(&pr->mtx), d.spin);
    for (k = 0; k < 3; k++)
    {
        f32 o = obj->prop->pos.f[k] - at[k];

        d.speed[k] = coopItemsPackSpeed(pr->speed.f[k]);
        d.ofs[k] = (s16) ((o > 32767.0f) ? 32767.0f : (o < -32767.0f) ? -32767.0f : o);
    }
    d.stage = (u8) bossGetStageNum();
    coopDesignSend(COOPEV_DROPSTART, COOP_ALL_PLAYERS, &d, sizeof(d));
}

/* joiner (versus: everyone else): start our copy where and how theirs did */
void coopItemsApplyDropStart(const u8 *raw)
{
    CoopDropStartMsg d;
    CoopItem *it;
    ObjectRecord *obj;
    PropRecord *prop;
    Projectile *pr;
    coord3d pos;
    StandTile *stan;
    s32 k;

    memcpy(&d, raw, sizeof(d));
    if ((coopVersus() ? (d.id >> 14) == (u16) (coopExtSelf() & 3) : coopExtIsHost()) || d.stage != (u8) bossGetStageNum() || d.id != sDropAtId || (it = coopItemById(d.id)) == NULL || !coopItemLive(it))
    {
        return;
    }
    obj = it->obj;
    prop = obj->prop;
    if (prop->parent != NULL || !coopItemsAirborne(obj) || (pr = obj->projectile) == NULL)
    {
        return;     /* in someone's hands already, or it never left ours */
    }
    for (k = 0; k < 3; k++)
    {
        pos.f[k] = sDropAt[k] + (f32) d.ofs[k];
    }
    stan = stanFindTileBelowPos(&pos, NULL, NULL);
    if (stan == NULL)
    {
        return;     /* no floor under the host's spot here: ours falls from the feet */
    }
    coopItemsUnpackRot(d.rot, coopItemsMtxScale(&obj->mtx), &obj->mtx);
    coopItemsUnpackRot(d.spin, 1.0f, &pr->mtx);
    for (k = 0; k < 3; k++)
    {
        pr->speed.f[k] = (f32) d.speed[k] / 1000.0f;
    }
    prop->pos = pos;
    prop->stan = stan;
    obj->runtime_pos = pos;
    setupUpdateObjectRoomPosition(obj);
    portConLogf("ge: coop: drop %04x leaves the hand as the %s did\n", d.id, coopVersus() ? "dropper's" : "host's");
}

/* chr.c: a guard is about to drop what it holds (CHRHIDDEN_DROP_HELD_ITEMS).
 * 1: keep it in hand, the host says what drops */
s32 coopItemsChrDropping(ChrRecord *chr)
{
    if (!coopItemsOn() || coopVersus() || sApplying || coopExtIsHost() || chr == NULL || chr->prop == NULL || coopPropIsPartner(chr->prop))
    {
        return 0;
    }
    return 1;
}

/* chr.c: the host's guard dropped prop */
void coopItemsChrDropped(ChrRecord *chr, PropRecord *prop)
{
    ObjectRecord *obj;
    CoopDropMsg m;

    if (!coopItemsOn() || coopVersus() || !coopExtIsHost() || chr == NULL || prop == NULL || (prop->type != PROP_TYPE_WEAPON && prop->type != PROP_TYPE_OBJ)
        || prop->parent != NULL || chr->prop == NULL || coopPropIsPartner(chr->prop))
    {
        return;
    }
    obj = prop->obj;
    if (obj == NULL || (prop->type == PROP_TYPE_WEAPON && ((WeaponObjRecord *) obj)->weaponnum <= ITEM_UNARMED))
    {
        return;
    }
    memset(&m, 0, sizeof(m));
    m.id = coopItemNewId();
    m.chrnum = (s16) chr->chrnum;
    m.item = (prop->type == PROP_TYPE_WEAPON) ? ((WeaponObjRecord *) obj)->weaponnum : -1;  /* -1: a key or other object it held */
    m.model = (s16) obj->obj;
    m.stage = bossGetStageNum();
    m.pos[0] = chr->prop->pos.f[0];
    m.pos[1] = chr->prop->pos.f[1];
    m.pos[2] = chr->prop->pos.f[2];
    if (prop->type == PROP_TYPE_WEAPON && ((WeaponObjRecord *) obj)->timer >= 0)
    {   /* a grenade it threw: live, not a pickup */
        s32 t = ((WeaponObjRecord *) obj)->timer;

        m.fuse = (s16) (1 + ((t > 32766) ? 32766 : t));
    }
    if ((obj->runtime_bitflags & RUNTIMEBITFLAG_HASPROJECTILE) && obj->projectile != NULL)
    {
        m.droptype = (s8) obj->projectile->droptype;
    }
    coopItemAdd(m.id, COOPITEM_DROP, obj);
    coopExtSend(COOPEV_DROP, 0xFF, &m, sizeof(m));
    coopItemsSendDropStart(m.id, obj, m.pos);
    portConLogf("ge: coop: guard %d dropped item %d (id %04x)%s\n", m.chrnum, m.item, m.id, m.fuse ? ", a live grenade" : "");
}

/* propobj.c drop_inventory, versus: our player died and dropped prop; the
 * same gun falls from our body in the other arenas (COOPEV_DROP, from the
 * chrnum our body goes by there) and is picked up once */
void coopItemsPlayerDropped(PropRecord *prop)
{
    ObjectRecord *obj;
    CoopDropMsg m;

    if (!coopItemsOn() || !coopVersus() || sApplying || prop == NULL || prop->type != PROP_TYPE_WEAPON
        || (obj = prop->obj) == NULL || g_CurrentPlayer->prop == NULL)
    {
        return;
    }
    memset(&m, 0, sizeof(m));
    m.id = coopItemNewId();
    m.chrnum = (s16) (COOP_PUPPET_CHRNUM + coopExtSelf());
    m.item = (s8) ((WeaponObjRecord *) obj)->weaponnum;
    m.model = (s16) obj->obj;
    m.stage = bossGetStageNum();
    m.pos[0] = g_CurrentPlayer->prop->pos.f[0];
    m.pos[1] = g_CurrentPlayer->prop->pos.f[1];
    m.pos[2] = g_CurrentPlayer->prop->pos.f[2];
    coopItemAdd(m.id, COOPITEM_DROP, obj);
    coopExtSend(COOPEV_DROP, 0xFF, &m, sizeof(m));
    coopItemsSendDropStart(m.id, obj, m.pos);
    portConLogf("ge: coop: we dropped item %d (id %04x)\n", m.item, m.id);
}

static void coopItemsApplyDrop(const CoopDropMsg *m)
{
    ChrRecord *chr;
    PropRecord *child;
    PropRecord *found = NULL;
    ObjectRecord *obj;

    if (m->stage != bossGetStageNum() || coopItemById(m->id) != NULL)
    {
        return;
    }
    sDropAtId = m->id;
    sDropAt[0] = m->pos[0];
    sDropAt[1] = m->pos[1];
    sDropAt[2] = m->pos[2];
    chr = chrFindByLiteralId(m->chrnum);
    if (chr != NULL && chr->prop != NULL)
    {
        for (child = chr->prop->child; child != NULL; child = child->prev)
        {
            if (m->item > 0 ? (child->type == PROP_TYPE_WEAPON && child->weapon != NULL && child->weapon->weaponnum == m->item)
                            : (child->type == PROP_TYPE_OBJ && child->obj != NULL && child->obj->obj == m->model))
            {
                found = child;
                break;
            }
        }
    }
    if (found == NULL && m->item <= 0 && chr != NULL && chr->prop != NULL && chr->model != NULL
        && m->model >= PROP_HATFURRY && m->model <= PROP_HATPEAKED && chr->handle_positiondata_hat == NULL)
    {   /* a hat the host's guard got by script, shot off before our copy had it: put it on to knock it off */
        found = hatCreateForChr(chr, m->model, 0);
        if (found != NULL)
        {
            coopHatSeen(chr);
            portConLogf("ge: coop: host guard %d's hat %d made to fall\n", m->chrnum, m->model);
        }
    }
    if (found == NULL && m->item <= 0)
    {
        return;     /* a key or object our stand-in does not have: nothing to make it from */
    }
    sApplying = 1;
    if (found == NULL)
    {   /* our stand-in did not hold that one: make it */
        obj = create_new_item_instance_of_model(m->model, m->item);
        if (obj == NULL || obj->prop == NULL)
        {
            sApplying = 0;
            return;
        }
        if (m->fuse > 0)
        {   /* armed before it leaves the hand, as chrlvTickThrowGrenade does */
            ((WeaponObjRecord *) obj)->timer = m->fuse - 1;
        }
        if (chr != NULL && chr->prop != NULL)
        {
            chrpropReparent(obj->prop, chr->prop);
            found = obj->prop;
        }
        else
        {
            coord3d pos, vel = { 0.0f, 0.0f, 0.0f };
            Mtxf rot, spin;
            StandTile *stan;

            pos.f[0] = m->pos[0];
            pos.f[1] = m->pos[1] + 60.0f;
            pos.f[2] = m->pos[2];
            stan = stanFindTileBelowPos(&pos, NULL, NULL);
            if (stan == NULL)
            {
                objFreePermanently(obj, TRUE);
                sApplying = 0;
                return;
            }
            matrix_4x4_set_identity(&rot);
            matrix_4x4_set_identity(&spin);
            gunInitProjectileObject(obj, &pos, stan, &rot, &vel, &spin, NULL);
            coopItemAdd(m->id, COOPITEM_DROP, obj);
            sApplying = 0;
            portConLogf("ge: coop: host guard %d's drop (item %d) made at %.0f %.0f %.0f\n", m->chrnum, m->item, pos.f[0], pos.f[1], pos.f[2]);
            return;
        }
    }
    if (found->type == PROP_TYPE_WEAPON && m->fuse > 0)
    {   /* the guard's grenade: live, thrown the way the host's guard faces, and no pickup (objTickPlayer) */
        found->weapon->timer = m->fuse - 1;
    }
    propobjSetDropped(found, (m->droptype == DROPTYPE_THROWGRENADE) ? DROPTYPE_THROWGRENADE : DROPTYPE_DEFAULT);
    /* drop it at the guard's feet, not its hand: our stand-in's hand is where our copy of the
     * model was last drawn, and a gun dropped from there could miss the floor and fall for ever */
    found->flags &= ~PROPFLAG_ONSCREEN;
    objDrop(found);
    coopItemAdd(m->id, COOPITEM_DROP, found->obj);
    sApplying = 0;
    portConLogf("ge: coop: host guard %d dropped item %d (id %04x)\n", m->chrnum, m->item, m->id);
}

/* ---- going off, being picked up ------------------------------------------------ */

static void coopItemsSendGone(CoopItem *it, s32 how)
{
    CoopGoneMsg g;

    memset(&g, 0, sizeof(g));
    g.id = it->id;
    g.how = (u8) how;
    g.stage = bossGetStageNum();
    coopExtSend(COOPEV_ITEMGONE, 0xFF, &g, sizeof(g));
}

/* propobj.c propExplode, before and after */
void coopItemsExplodeBegin(PropRecord *prop)
{
    CoopItem *it;

    sExploding = NULL;
    sExplodingOwner = 0;
    sExplodingOurRound = 0;
    if (prop == NULL || prop->obj == NULL || !coopItemsOn())
    {
        return;
    }
    if ((prop->obj->runtime_bitflags & RUNTIMEBITFLAG_HASPROJECTILE) && prop->obj->projectile != NULL
        && g_CurrentPlayer != NULL && prop->obj->projectile->ownerprop == g_CurrentPlayer->prop)
    {
        sExplodingOurRound = 1;     /* a rocket or grenade round: not a thrown item, but ours for friendly fire */
    }
    it = coopItemByObj(prop->obj);
    if (it == NULL)
    {
        return;
    }
    if (it->kind == COOPITEM_ROUND)
    {   /* our launcher round: the copies only showed it, and go now; its blast is sent as any is */
        coopItemsSendGone(it, COOPGONE_BOOM);
        it->used = 0;
        return;
    }
    sExploding = prop->obj;
    sExplodingOwner = ((it->id >> 14) != (u16) coopExtSelf()) ? 1 + (it->id >> 14) : 0;
    if (!it->gone)
    {
        coopItemsSendGone(it, COOPGONE_BOOM);   /* the other copies go off with it */
    }
    it->used = 0;
    portConLogf("ge: coop: item %04x went off\n", it->id);
}

void coopItemsExplodeEnd(void)
{
    sExploding = NULL;
    sExplodingOwner = 0;
    sExplodingOurRound = 0;
}

/* coop.c: the blast being made is a launcher round our own Bond fired */
s32 coopItemsExplodingOurRound(void)
{
    return sExplodingOurRound;
}

/* coop.c coopExplosionCreated: the blast is one of these items' (each world sets off its own copy) */
s32 coopItemsExploding(void)
{
    return sExploding != NULL;
}

/* 1 + whose item is going off, 0 when it is ours or none (coop.c: stats blame on the host) */
s32 coopItemsExplodingOwner(void)
{
    return sExplodingOwner;
}

/* propobj.c chrobjWeaponTick: our watch's detonator sets off only our own remote mines */
s32 coopItemsIsPartners(ObjectRecord *obj)
{
    CoopItem *it;

    if (!coopItemsOn())
    {
        return 0;
    }
    it = coopItemByObj(obj);
    return it != NULL && it->kind == COOPITEM_THROWN && (it->id >> 14) != (u16) coopExtSelf();
}

/* coop.c coopPickedUp: we picked up something made at run time */
void coopItemsPickedUp(ObjectRecord *obj)
{
    CoopItem *it;

    if (sApplying || !coopItemsOn())
    {
        return;
    }
    it = coopItemByObj(obj);
    if (it == NULL)
    {
        return;
    }
    coopItemsSendGone(it, COOPGONE_PICKED);
    it->used = 0;
    portConLogf("ge: coop: picked up item %04x\n", it->id);
}

static void coopItemsApplyGone(const CoopGoneMsg *g, s32 from)
{
    CoopItem *it;
    ObjectRecord *obj;
    PropRecord *prop;

    if (g->stage != bossGetStageNum())
    {
        return;
    }
    it = coopItemById(g->id);
    if (it == NULL)
    {
        return; /* gone here too already */
    }
    obj = it->obj;
    prop = obj->prop;
    if (it->kind == COOPITEM_ROUND)
    {
        it->gone = 1;   /* coopItemsShownOnlyTick takes it away */
        return;
    }
    if (g->how == COOPGONE_BOOM)
    {
        it->gone = 1;   /* coopItemsExplodeBegin knows it, and sends nothing back */
        ((WeaponObjRecord *) obj)->timer = 0;  /* goes off on its next tick, as a mine the detonator reached */
        portConLogf("ge: coop: player %d's copy of item %04x went off: ours goes too\n", from + 1, g->id);
        return;
    }
    it->used = 0;
    if (it->kind == COOPITEM_DROP && coopInMissionNow() && !coopVersus())
    {   /* a gun: the pickup rule gives everyone what it holds (in versus it is only theirs) */
        TICKOP op;

        sApplying = 1;
        op = propPickupByPlayer(prop, TRUE);
        if (op != TICKOP_NONE)
        {
            propExecuteTickOperation(prop, op);
            sApplying = 0;
            portConLogf("ge: coop: player %d picked up dropped item %04x: ours too\n", from + 1, g->id);
            return;
        }
        sApplying = 0;
    }
    objFreePermanently(obj, TRUE);
    portConLogf("ge: coop: player %d picked up item %04x: removed here\n", from + 1, g->id);
}

/* ---- where a guard's drop came to rest -------------------------------------------
 * Each game lets its copy of a dropped gun tumble on its own, from its own copy
 * of the guard, so the copies came to rest apart. Once the host's has landed
 * (no longer airborne), its place and orientation go to everyone, and theirs
 * is put there exactly. */

static s32 coopItemsAirborne(ObjectRecord *obj)
{
    return (obj->runtime_bitflags & RUNTIMEBITFLAG_HASPROJECTILE) && obj->projectile != NULL
        && (obj->projectile->flags & PROJECTILEFLAG_AIRBORNE);
}

/* the rotation part of obj->mtx (scaled by the model) to a unit quaternion */
static void coopItemsMtxToQuat(Mtxf *mtx, f32 scale, f32 q[4])
{
    f32 m[3][3];
    f32 t, n;
    s32 a, b;

    for (a = 0; a < 3; a++)
    {
        for (b = 0; b < 3; b++)
        {
            m[a][b] = mtx->m[a][b] / scale;
        }
    }
    t = m[0][0] + m[1][1] + m[2][2];
    if (t > 0.0f)
    {
        f32 r = sqrtf(t + 1.0f) * 2.0f;
        q[3] = 0.25f * r;
        q[0] = (m[1][2] - m[2][1]) / r;
        q[1] = (m[2][0] - m[0][2]) / r;
        q[2] = (m[0][1] - m[1][0]) / r;
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
    {
        f32 r = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q[3] = (m[1][2] - m[2][1]) / r;
        q[0] = 0.25f * r;
        q[1] = (m[1][0] + m[0][1]) / r;
        q[2] = (m[2][0] + m[0][2]) / r;
    }
    else if (m[1][1] > m[2][2])
    {
        f32 r = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q[3] = (m[2][0] - m[0][2]) / r;
        q[0] = (m[1][0] + m[0][1]) / r;
        q[1] = 0.25f * r;
        q[2] = (m[2][1] + m[1][2]) / r;
    }
    else
    {
        f32 r = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q[3] = (m[0][1] - m[1][0]) / r;
        q[0] = (m[2][0] + m[0][2]) / r;
        q[1] = (m[2][1] + m[1][2]) / r;
        q[2] = 0.25f * r;
    }
    n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (a = 0; a < 4; a++)
    {
        q[a] = (n > 0.0f) ? q[a] / n : (a == 3);
    }
}

static void coopItemsQuatToMtx(const f32 q[4], f32 scale, Mtxf *mtx)
{
    f32 x = q[0], y = q[1], z = q[2], w = q[3];

    mtx->m[0][0] = (1.0f - 2.0f * (y * y + z * z)) * scale;
    mtx->m[0][1] = (2.0f * (x * y + z * w)) * scale;
    mtx->m[0][2] = (2.0f * (x * z - y * w)) * scale;
    mtx->m[1][0] = (2.0f * (x * y - z * w)) * scale;
    mtx->m[1][1] = (1.0f - 2.0f * (x * x + z * z)) * scale;
    mtx->m[1][2] = (2.0f * (y * z + x * w)) * scale;
    mtx->m[2][0] = (2.0f * (x * z + y * w)) * scale;
    mtx->m[2][1] = (2.0f * (y * z - x * w)) * scale;
    mtx->m[2][2] = (1.0f - 2.0f * (x * x + y * y)) * scale;
}

static f32 coopItemsMtxScale(Mtxf *mtx)
{
    f32 s = sqrtf(mtx->m[0][0] * mtx->m[0][0] + mtx->m[0][1] * mtx->m[0][1] + mtx->m[0][2] * mtx->m[0][2]);

    return (s > 0.0001f) ? s : 1.0f;
}

/* host, each tick: send the resting place of each guard's drop once it has landed */
static void coopItemsRestTick(void)
{
    s32 i, k;

    if (!coopItemsOn() || (!coopExtIsHost() && !coopVersus()) || !coopInMissionNow())
    {
        return;
    }
    for (i = 0; i < COOP_ITEMS_MAX; i++)
    {
        CoopItem *it = &sItems[i];
        ObjectRecord *obj = it->obj;
        CoopDropRestMsg r;
        f32 q[4];

        if (!it->used || it->rested || it->kind != COOPITEM_DROP || !coopItemLive(it) || obj->prop->parent != NULL
            || coopItemsAirborne(obj) || (coopVersus() && (it->id >> 14) != (u16) (coopExtSelf() & 3)))   /* versus: the dropper's word */
        {
            continue;
        }
        if (it->last[0] != obj->prop->pos.f[0] || it->last[1] != obj->prop->pos.f[1] || it->last[2] != obj->prop->pos.f[2])
        {   /* still settling */
            it->last[0] = obj->prop->pos.f[0];
            it->last[1] = obj->prop->pos.f[1];
            it->last[2] = obj->prop->pos.f[2];
            it->still = 0;
            continue;
        }
        if (++it->still < 30)
        {
            continue;
        }
        it->rested = 1;
        memset(&r, 0, sizeof(r));
        r.id = it->id;
        coopItemsMtxToQuat(&obj->mtx, coopItemsMtxScale(&obj->mtx), q);
        for (k = 0; k < 4; k++)
        {
            r.rot[k] = (s16) (q[k] * 32767.0f);
        }
        r.pos[0] = obj->prop->pos.f[0];
        r.pos[1] = obj->prop->pos.f[1];
        r.pos[2] = obj->prop->pos.f[2];
        coopExtSend(COOPEV_DROPREST, 0xFF, &r, sizeof(r));
        portConLogf("ge: coop: drop %04x came to rest at %.0f %.0f %.0f\n", r.id, r.pos[0], r.pos[1], r.pos[2]);
    }
}

static void coopItemsApplyRest(const CoopDropRestMsg *r)
{
    CoopItem *it = coopItemById(r->id);
    ObjectRecord *obj;
    PropRecord *prop;
    coord3d pos;
    StandTile *stan;
    f32 q[4];
    f32 off;
    s32 k;

    if (it == NULL || !coopItemLive(it) || (prop = it->obj->prop)->parent != NULL)
    {
        return;     /* picked up, or gone */
    }
    obj = it->obj;
    {
        f32 dx = prop->pos.f[0] - r->pos[0], dy = prop->pos.f[1] - r->pos[1], dz = prop->pos.f[2] - r->pos[2];

        off = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    pos.f[0] = r->pos[0];
    pos.f[1] = r->pos[1] + 30.0f;   /* the floor under it, from a little above */
    pos.f[2] = r->pos[2];
    stan = stanFindTileBelowPos(&pos, NULL, NULL);
    if (stan == NULL)
    {
        return;
    }
    for (k = 0; k < 4; k++)
    {
        q[k] = (f32) r->rot[k] / 32767.0f;
    }
    if (obj->runtime_bitflags & RUNTIMEBITFLAG_HASPROJECTILE)
    {
        obj->projectile->flags &= ~PROJECTILEFLAG_AIRBORNE;     /* ours stops where theirs did */
        obj->projectile->speed.f[0] = obj->projectile->speed.f[1] = obj->projectile->speed.f[2] = 0.0f;
    }
    coopItemsQuatToMtx(q, coopItemsMtxScale(&obj->mtx), &obj->mtx);
    pos.f[1] = r->pos[1];
    prop->pos = pos;
    prop->stan = stan;
    obj->runtime_pos = pos;
    setupUpdateObjectRoomPosition(obj);
    portConLogf("ge: coop: drop %04x put where the %s came to rest (ours was %.0f away)\n", r->id, coopVersus() ? "dropper's" : "host's", off);
}

/* ---- where a throw ended up ---------------------------------------------------
 * The game only sticks a thrown item to something drawn on that screen
 * (objEmbed), so a partner's mine thrown at a helicopter off our screen
 * bounced off our copy of it, and Surface 2's script, seeing it lie loose,
 * failed the objective. The thrower's game decides: once its own copy sticks
 * to a setup object or comes to rest, COOPEV_ITEMSTUCK says where, and the
 * other copies are put there, stuck to the same object. Until that word
 * comes, a partner's copy is not an item lying in the level to the level
 * scripts (coopItemsUnsettled, propobj.c weaponFindThrown). */

#define COOPITEM_SETTLE_MAX 600     /* 10 s: their word got lost, or they left */

static void coopItemsSendStuck(CoopItem *it, s32 objofs, coord3d *pos)
{
    CoopStuckMsg m;
    f32 q[4];
    s32 k;

    it->settled = 1;
    memset(&m, 0, sizeof(m));
    m.id = it->id;
    m.objofs = objofs;
    coopItemsMtxToQuat(&it->obj->mtx, coopItemsMtxScale(&it->obj->mtx), q);
    for (k = 0; k < 4; k++)
    {
        m.rot[k] = (s16) (q[k] * 32767.0f);
    }
    m.pos[0] = pos->f[0];
    m.pos[1] = pos->f[1];
    m.pos[2] = pos->f[2];
    coopExtSend(COOPEV_ITEMSTUCK, 0xFF, &m, sizeof(m));
    portConLogf("ge: coop: our item %04x %s %d at %.0f %.0f %.0f\n", it->id, objofs >= 0 ? "stuck to obj" : "landed",
                objofs, pos->f[0], pos->f[1], pos->f[2]);
}

static void coopItemsSettleTick(void)
{
    s32 i;

    for (i = 0; i < COOP_ITEMS_MAX; i++)
    {
        CoopItem *it = &sItems[i];
        ObjectRecord *obj = it->obj;
        PropRecord *parent;

        if (!it->used || it->kind != COOPITEM_THROWN || it->settled || !coopItemLive(it))
        {
            continue;
        }
        if (it->age < 0xFFFF)
        {
            it->age++;
        }
        if ((it->id >> 14) != (u16) coopExtSelf())
        {
            if (it->age > COOPITEM_SETTLE_MAX)
            {
                it->settled = 1;
            }
            continue;
        }
        parent = obj->prop->parent;
        if (obj->runtime_bitflags & RUNTIMEBITFLAG_EMBEDDED)
        {
            s32 ofs = (parent != NULL && parent->type == PROP_TYPE_OBJ) ? coopExtSetupOfs(parent->obj) : -1;

            if (ofs < 0)
            {
                it->settled = 1;    /* stuck to a guard or to something thrown: theirs can stay as it is */
                continue;
            }
            coopItemsSendStuck(it, ofs, &obj->runtime_pos);
            continue;
        }
        if (parent != NULL || coopItemsAirborne(obj))
        {
            it->still = 0;
            continue;
        }
        if (it->last[0] != obj->prop->pos.f[0] || it->last[1] != obj->prop->pos.f[1] || it->last[2] != obj->prop->pos.f[2])
        {
            it->last[0] = obj->prop->pos.f[0];
            it->last[1] = obj->prop->pos.f[1];
            it->last[2] = obj->prop->pos.f[2];
            it->still = 0;
            continue;
        }
        if (++it->still >= 10)
        {
            coopItemsSendStuck(it, -1, &obj->prop->pos);
        }
    }
}

static void coopItemsApplyStuck(const CoopStuckMsg *m, s32 from)
{
    CoopItem *it = coopItemById(m->id);
    ObjectRecord *obj, *host;
    PropRecord *prop;
    coord3d pos;
    f32 q[4];
    s32 k;

    if (it == NULL || it->kind != COOPITEM_THROWN)
    {
        return;
    }
    it->settled = 1;
    obj = it->obj;
    prop = obj->prop;
    if (obj->runtime_bitflags & RUNTIMEBITFLAG_EMBEDDED)
    {
        return;     /* ours stuck by itself */
    }
    if (prop->parent != NULL)
    {
        return;     /* in someone's hands */
    }
    if (m->objofs < 0)
    {
        CoopDropRestMsg r;

        memset(&r, 0, sizeof(r));
        r.id = m->id;
        memcpy(r.rot, m->rot, sizeof(r.rot));
        memcpy(r.pos, m->pos, sizeof(r.pos));
        coopItemsApplyRest(&r);     /* lies where theirs does */
        return;
    }
    host = (ObjectRecord *) ((u8 *) g_CurrentSetup.propDefs + m->objofs);
    if (g_CurrentSetup.propDefs == NULL || coopExtSetupOfs(host) != m->objofs || host->prop == NULL || host->model == NULL)
    {
        return;
    }
    if (obj->runtime_bitflags & RUNTIMEBITFLAG_HASPROJECTILE)
    {
        projectileFree(obj->projectile);
        obj->projectile = NULL;
        obj->runtime_bitflags &= ~RUNTIMEBITFLAG_HASPROJECTILE;
    }
    for (k = 0; k < 4; k++)
    {
        q[k] = (f32) m->rot[k] / 32767.0f;
    }
    coopItemsQuatToMtx(q, coopItemsMtxScale(&obj->mtx), &obj->mtx);
    pos.f[0] = m->pos[0];
    pos.f[1] = m->pos[1];
    pos.f[2] = m->pos[2];
    prop->pos = pos;
    obj->runtime_pos = pos;
    if (objCoopEmbed(prop, host->prop))
    {
        it->embedfix = 1;
        coopObjNoteUse(from);   /* an objective it completes is theirs (coop_objective.c) */
        portConLogf("ge: coop: player %d's item %04x stuck to obj %d here too\n", from + 1, m->id, m->objofs);
    }
}

/* propobj.c: obj was stuck by a partner's word; 1 once, when its matrix is to be worked out */
s32 coopItemsEmbedPending(ObjectRecord *obj)
{
    CoopItem *it = coopItemsOn() ? coopItemByObj(obj) : NULL;

    if (it == NULL || !it->embedfix)
    {
        return 0;
    }
    it->embedfix = 0;
    return 1;
}

/* propobj.c weaponFindThrown: a partner's throw not yet settled by their game */
s32 coopItemsUnsettled(PropRecord *prop)
{
    CoopItem *it;

    if (!coopItemsOn() || prop == NULL || (prop->type != PROP_TYPE_OBJ && prop->type != PROP_TYPE_WEAPON))
    {
        return 0;
    }
    it = coopItemByObj(prop->obj);
    return it != NULL && it->kind == COOPITEM_THROWN && !it->settled && (it->id >> 14) != (u16) coopExtSelf();
}

/* ---- knocked about by a shot -------------------------------------------------
 * objBounce gives a shot item a random kick, in the shooter's game only: the
 * host shot the covert modem and it hopped about on the host's screen and lay
 * still on the joiner's, and the other way round. The shooter's kick goes to
 * everyone, so their copies fly the same way, and where the shooter's copy
 * comes to rest follows (COOPEV_DROPREST, as a guard's drop). */

static Projectile *coopItemsProjectile(ObjectRecord *obj)
{
    if (obj->runtime_bitflags & RUNTIMEBITFLAG_EMBEDDED)
    {
        return obj->embedment->projectile;
    }
    if (obj->runtime_bitflags & RUNTIMEBITFLAG_HASPROJECTILE)
    {
        return obj->projectile;
    }
    return NULL;
}

static s16 coopItemsPackSpeed(f32 v)
{
    v *= 1000.0f;
    return (s16) ((v > 32767.0f) ? 32767.0f : (v < -32767.0f) ? -32767.0f : v);
}

/* propobj.c objHit: our shot just bounced obj */
void coopItemsBounced(ObjectRecord *obj)
{
    CoopItem *it;
    Projectile *pr;
    CoopKnockMsg m;
    f32 q[4];
    s32 k;

    if (sApplying || !coopItemsOn() || (it = coopItemByObj(obj)) == NULL || (pr = coopItemsProjectile(obj)) == NULL)
    {
        return;
    }
    memset(&m, 0, sizeof(m));
    m.id = it->id;
    coopItemsMtxToQuat(&pr->mtx, coopItemsMtxScale(&pr->mtx), q);
    for (k = 0; k < 4; k++)
    {
        m.rot[k] = (s16) (q[k] * 32767.0f);
    }
    m.speed[0] = coopItemsPackSpeed(pr->speed.f[0]);
    m.speed[1] = coopItemsPackSpeed(pr->speed.f[1]);
    m.speed[2] = coopItemsPackSpeed(pr->speed.f[2]);
    m.stage = bossGetStageNum();
    it->knocked = 1;
    it->still = 0;
    it->settled = 1;    /* ours: the rest word below replaces the settle word */
    coopExtSend(COOPEV_ITEMKNOCK, 0xFF, &m, sizeof(m));
    portConLogf("ge: coop: we knocked item %04x about\n", it->id);
}

/* propobj.c objHit: our shot is about to knock loose what sits on parent (a
 * modem stuck to a crate): each game drops its own copy its own way, so where
 * ours lands goes to everyone as for a shot item */
void coopItemsParentKnocked(PropRecord *parent)
{
    PropRecord *child;
    CoopItem *it;

    if (sApplying || !coopItemsOn() || parent == NULL)
    {
        return;
    }
    for (child = parent->child; child != NULL; child = child->prev)
    {
        if ((child->type == PROP_TYPE_OBJ || child->type == PROP_TYPE_WEAPON) && (it = coopItemByObj(child->obj)) != NULL)
        {
            it->knocked = 1;
            it->still = 0;
            it->settled = 1;
            portConLogf("ge: coop: we knocked item %04x loose\n", it->id);
        }
    }
}

static void coopItemsApplyKnock(const CoopKnockMsg *m, s32 from)
{
    CoopItem *it = coopItemById(m->id);
    CoopExtView view;
    ObjectRecord *obj;
    Projectile *pr;
    coord3d dir = { 0.0f, 0.0f, 0.0f };
    f32 q[4];
    s32 k;

    if (m->stage != bossGetStageNum() || it == NULL)
    {
        return;     /* gone here already */
    }
    obj = it->obj;
    if (obj->prop->parent != NULL && !(obj->runtime_bitflags & RUNTIMEBITFLAG_EMBEDDED))
    {
        return;     /* in someone's hands here */
    }
    sApplying = 1;
    objBounce(obj, &dir);
    sApplying = 0;
    if ((pr = coopItemsProjectile(obj)) == NULL)
    {
        return;
    }
    for (k = 0; k < 4; k++)
    {
        q[k] = (f32) m->rot[k] / 32767.0f;
    }
    coopItemsQuatToMtx(q, 1.0f, &pr->mtx);
    pr->speed.f[0] = (f32) m->speed[0] / 1000.0f;
    pr->speed.f[1] = (f32) m->speed[1] / 1000.0f;
    pr->speed.f[2] = (f32) m->speed[2] / 1000.0f;
    if (!coopVersus() && coopExtRemoteView(from, &view) && view.prop != NULL)
    {
        pr->ownerprop = view.prop;
    }
    it->knocked = 0;    /* theirs to say where it ends up */
    it->settled = 1;
    portConLogf("ge: coop: player %d knocked item %04x about: ours flies too\n", from + 1, m->id);
}

/* each tick: once a copy our shot sent flying is still, where it lies goes to everyone */
static void coopItemsKnockTick(void)
{
    s32 i, k;

    for (i = 0; i < COOP_ITEMS_MAX; i++)
    {
        CoopItem *it = &sItems[i];
        ObjectRecord *obj = it->obj;
        CoopDropRestMsg r;
        f32 q[4];

        if (!it->used || !it->knocked || !coopItemLive(it))
        {
            continue;
        }
        if (obj->prop->parent != NULL || (obj->runtime_bitflags & RUNTIMEBITFLAG_EMBEDDED))
        {
            it->knocked = 0;    /* picked up, still stuck where it was, or stuck to something on the way */
            continue;
        }
        if (coopItemsAirborne(obj) || it->last[0] != obj->prop->pos.f[0] || it->last[1] != obj->prop->pos.f[1]
            || it->last[2] != obj->prop->pos.f[2])
        {
            it->last[0] = obj->prop->pos.f[0];
            it->last[1] = obj->prop->pos.f[1];
            it->last[2] = obj->prop->pos.f[2];
            it->still = 0;
            continue;
        }
        if (++it->still < 10)
        {
            continue;
        }
        it->knocked = 0;
        it->rested = 1;
        memset(&r, 0, sizeof(r));
        r.id = it->id;
        coopItemsMtxToQuat(&obj->mtx, coopItemsMtxScale(&obj->mtx), q);
        for (k = 0; k < 4; k++)
        {
            r.rot[k] = (s16) (q[k] * 32767.0f);
        }
        r.pos[0] = obj->prop->pos.f[0];
        r.pos[1] = obj->prop->pos.f[1];
        r.pos[2] = obj->prop->pos.f[2];
        coopExtSend(COOPEV_DROPREST, 0xFF, &r, sizeof(r));
        portConLogf("ge: coop: knocked item %04x came to rest at %.0f %.0f %.0f\n", r.id, r.pos[0], r.pos[1], r.pos[2]);
    }
}

/* ---- events --------------------------------------------------------------- */

void coopItemsApply(s32 kind, const u8 *raw, s32 from)
{
    if (!coopItemsOn())
    {
        return;
    }
    switch (kind)
    {
        case COOPEV_THROW:
            memcpy(&sPendingThrow, raw, sizeof(sPendingThrow));
            sPendingFrom = from;
            break;
        case COOPEV_THROWMORE:
        {
            CoopThrowMoreMsg b;

            memcpy(&b, raw, sizeof(b));
            if (coopInMissionNow())
            {
                coopItemsApplyThrow(&b, from);
            }
            break;
        }
        case COOPEV_ITEMGONE:
        {
            CoopGoneMsg g;

            memcpy(&g, raw, sizeof(g));
            coopItemsApplyGone(&g, from);
            break;
        }
        case COOPEV_ITEMSTUCK:
        {
            CoopStuckMsg m;

            memcpy(&m, raw, sizeof(m));
            if (coopInMissionNow())
            {
                coopItemsApplyStuck(&m, from);
            }
            break;
        }
        case COOPEV_ITEMKNOCK:
        {
            CoopKnockMsg m;

            memcpy(&m, raw, sizeof(m));
            if (coopInMissionNow())
            {
                coopItemsApplyKnock(&m, from);
            }
            break;
        }
        case COOPEV_DROPREST:
        {
            CoopDropRestMsg r;

            memcpy(&r, raw, sizeof(r));
            if (coopInMissionNow())
            {
                coopItemsApplyRest(&r);
            }
            break;
        }
        case COOPEV_DROP:
        {
            CoopDropMsg m;

            memcpy(&m, raw, sizeof(m));
            if (coopInMissionNow())
            {
                coopItemsApplyDrop(&m);
            }
            break;
        }
    }
}

/* ---- harness ----------------------------------------------------------------
 * GE_COOP_TESTAIM=<s>:<yaw>:<pitch>: from <s> seconds into the mission, for
 * 8 s, turn <yaw> degrees from where we stood at the start and look <pitch>
 * degrees up (negative down); with GE_COOP_TESTFIRE it aims a throw.
 * GE_COOP_TESTTHROWAT=<tag>:<s>[:<dist>]: at <s> seconds stand <dist> units
 * (default 250) from the object with that tag and look at it for 8 s, so a
 * GE_COOP_TESTFIRE throw lands on it.
 * GE_COOP_TESTKNOCK=<s>[:<every>[:<times>]]: from <s> seconds, every <every>
 * s (5), <times> times (3), knock the nearest thrown or dropped item about
 * as a shot from where we look would (objHit's bounce).
 * GE_COOP_TESTITEMLOG=1: every 2 s, log where each thrown or dropped item is.
 * GE_COOP_TESTGRENADE=<s>[:<every>]: host, from <s> seconds, every <every> s
 * (6), the nearest guard throws a grenade.
 * GE_COOP_TESTSTEPON=<s>: from <s> seconds, each second, stand on the nearest
 * thrown or dropped item lying still, as walking onto it would. */
static s32 sTestAimAt = -2;
static f32 sTestAimYaw, sTestAimPitch, sTestAimBase = -1.0f;
static s32 sTestAtTag = -1, sTestAtAt = -1;
static f32 sTestAtDist = 250.0f;

extern void coopTeleportPlayerTo(coord3d *pos, StandTile *stan, f32 yaw);

static void coopItemsTestThrowAt(s32 missionticks)
{
    ObjectRecord *obj;
    coord3d to;
    StandTile *stan = NULL;
    f32 dx, dy, dz, d;
    s32 k;

    if (sTestAtAt < 0 || missionticks < sTestAtAt || missionticks >= sTestAtAt + 480
        || (obj = objFindByTagId(sTestAtTag)) == NULL || obj->prop == NULL)
    {
        return;
    }
    if (missionticks == sTestAtAt)
    {
        for (k = 0; k < 8 && stan == NULL; k++)
        {   /* the first side of it with floor */
            to.f[0] = obj->prop->pos.f[0] + sTestAtDist * cosf(k * 0.7853982f);
            to.f[1] = obj->prop->pos.f[1] + 100.0f;
            to.f[2] = obj->prop->pos.f[2] + sTestAtDist * sinf(k * 0.7853982f);
            stan = stanFindTileBelowPos(&to, NULL, NULL);
        }
        if (stan == NULL)
        {
            portConLogf("ge: coop: testthrowat: no floor by tag %d\n", sTestAtTag);
            sTestAtAt = -1;
            return;
        }
        dx = obj->prop->pos.f[0] - to.f[0];
        dz = obj->prop->pos.f[2] - to.f[2];
        coopTeleportPlayerTo(&to, stan, atan2f(-dx, dz));
        portConLogf("ge: coop: testthrowat: by tag %d at %.0f %.0f %.0f\n", sTestAtTag, to.f[0], to.f[1], to.f[2]);
    }
    dx = obj->prop->pos.f[0] - g_CurrentPlayer->prop->pos.f[0];
    dy = obj->prop->pos.f[1] - g_CurrentPlayer->prop->pos.f[1];
    dz = obj->prop->pos.f[2] - g_CurrentPlayer->prop->pos.f[2];
    d = sqrtf(dx * dx + dz * dz);
    g_CurrentPlayer->vv_theta = atan2f(-dx, dz) * 57.29578f;
    if (g_CurrentPlayer->vv_theta < 0.0f)
    {
        g_CurrentPlayer->vv_theta += 360.0f;
    }
    g_CurrentPlayer->vv_verta = atan2f(dy, (d > 1.0f) ? d : 1.0f) * 57.29578f;
}

static void coopItemsTestKnock(s32 missionticks)
{
    static s32 at = -2, every = 5, times = 3, logon;
    CoopItem *best = NULL;
    f32 bestd = 1e30f;
    s32 i;

    if (at == -2)
    {
        const char *e = getenv("GE_COOP_TESTKNOCK");

        at = -1;
        if (e != NULL && sscanf(e, "%d:%d:%d", &at, &every, &times) >= 1)
        {
            at *= 60;
        }
        logon = getenv("GE_COOP_TESTITEMLOG") != NULL;
    }
    if (logon && missionticks > 0 && (missionticks % 120) == 0)
    {
        for (i = 0; i < COOP_ITEMS_MAX; i++)
        {
            if (coopItemLive(&sItems[i]))
            {
                PropRecord *p = sItems[i].obj->prop;

                portConLogf("ge: coop: itemlog t=%d id %04x item %d at %.0f %.0f %.0f fuse %d%s%s\n", missionticks / 60, sItems[i].id,
                            sItems[i].item, p->pos.f[0], p->pos.f[1], p->pos.f[2], ((WeaponObjRecord *) sItems[i].obj)->timer,
                            coopItemsAirborne(sItems[i].obj) ? " airborne" : "", p->parent != NULL ? " parented" : "");
            }
        }
    }
    if (at < 0 || times <= 0 || missionticks < at || ((missionticks - at) % (every * 60)) != 0 || g_CurrentPlayer->prop == NULL)
    {
        return;
    }
    times--;
    for (i = 0; i < COOP_ITEMS_MAX; i++)
    {
        PropRecord *p;
        f32 dx, dz;

        if (!coopItemLive(&sItems[i]) || (p = sItems[i].obj->prop)->parent != NULL)
        {
            continue;
        }
        dx = p->pos.f[0] - g_CurrentPlayer->prop->pos.f[0];
        dz = p->pos.f[2] - g_CurrentPlayer->prop->pos.f[2];
        if (dx * dx + dz * dz < bestd)
        {
            bestd = dx * dx + dz * dz;
            best = &sItems[i];
        }
    }
    if (best == NULL)
    {
        portConLogf("ge: coop: testknock: nothing to knock\n");
        return;
    }
    {
        coord3d dir = { 0.0f, 0.0f, -1.0f };    /* straight ahead, in the view */

        portConLogf("ge: coop: testknock: item %04x (dist %.0f)\n", best->id, sqrtf(bestd));
        objBounce(best->obj, &dir);
        coopItemsBounced(best->obj);
    }
}

extern bool actor_draws_throws_grenade_at_player_if_possible(ChrRecord *self);
extern s32 chrIsNotDeadOrShot(ChrRecord *chr);

static void coopItemsTestGrenade(s32 missionticks)
{
    static s32 at = -2, every = 6, stepat = -2;
    PropRecord *p, *best = NULL;
    f32 bestd = 1e30f;

    if (at == -2)
    {
        const char *e = getenv("GE_COOP_TESTGRENADE");

        at = -1;
        if (e != NULL && sscanf(e, "%d:%d", &at, &every) >= 1)
        {
            at *= 60;
        }
        e = getenv("GE_COOP_TESTSTEPON");
        stepat = (e != NULL) ? atoi(e) * 60 : -1;
    }
    if (g_CurrentPlayer->prop == NULL)
    {
        return;
    }
    if (at >= 0 && missionticks >= at && ((missionticks - at) % (every * 60)) == 0 && coopExtIsHost())
    {
        for (p = g_ActivePropsHead; p != NULL; p = p->next)
        {
            f32 dx, dz;

            if (p->type != PROP_TYPE_CHR || p->chr == NULL || coopPropIsPartner(p) || !chrIsNotDeadOrShot(p->chr))
            {
                continue;
            }
            dx = p->pos.f[0] - g_CurrentPlayer->prop->pos.f[0];
            dz = p->pos.f[2] - g_CurrentPlayer->prop->pos.f[2];
            if (dx * dx + dz * dz < bestd)
            {
                bestd = dx * dx + dz * dz;
                best = p;
            }
        }
        if (best != NULL)
        {
            u8 prob = best->chr->grenadeprob;
            s32 thrown;

            best->chr->grenadeprob = 0xFF;
            thrown = actor_draws_throws_grenade_at_player_if_possible(best->chr);
            best->chr->grenadeprob = prob;
            portConLogf("ge: coop: testgrenade: guard %d %s (dist %.0f)\n", best->chr->chrnum, thrown ? "throws" : "could not throw", sqrtf(bestd));
        }
    }
    if (stepat >= 0 && missionticks >= stepat && (missionticks % 60) == 0)
    {
        CoopItem *it = NULL;
        s32 i;

        for (i = 0; i < COOP_ITEMS_MAX; i++)
        {
            f32 dx, dz;

            if (!coopItemLive(&sItems[i]) || (p = sItems[i].obj->prop)->parent != NULL || coopItemsAirborne(sItems[i].obj) || p->stan == NULL)
            {
                continue;
            }
            dx = p->pos.f[0] - g_CurrentPlayer->prop->pos.f[0];
            dz = p->pos.f[2] - g_CurrentPlayer->prop->pos.f[2];
            if (dx * dx + dz * dz < 4.0f)
            {
                it = NULL;  /* already on one */
                break;
            }
            if (it == NULL || dx * dx + dz * dz < bestd)
            {
                bestd = dx * dx + dz * dz;
                it = &sItems[i];
            }
        }
        if (it != NULL)
        {
            p = it->obj->prop;
            portConLogf("ge: coop: teststepon: item %04x (item %d, fuse %d)\n", it->id, it->item, ((WeaponObjRecord *) it->obj)->timer);
            coopTeleportPlayerTo(&p->pos, p->stan, g_CurrentPlayer->vv_theta * 0.01745329f);
        }
    }
}

/* GE_COOP_TESTROUND=<s>:<item>[:<chrnum>]: <s> seconds in, a rocket (86) or a
 * grenade-launcher round (87) is launched from our Bond, or on the host from
 * that guard, level and the way it faces (harness only) */
static void coopItemsTestRound(s32 missionticks)
{
    static s32 at = -2, item, chrnum = -1;
    ChrRecord *guard = NULL;
    PropRecord *from;
    WeaponObjRecord *w;
    coord3d pos, vel, dir;
    Mtxf rot, ident;
    f32 yaw;

    if (at == -2)
    {
        const char *e = getenv("GE_COOP_TESTROUND");

        at = -1;
        if (e == NULL || sscanf(e, "%d:%d:%d", &at, &item, &chrnum) < 2)
        {
            at = -1;
        }
        at = (at >= 0) ? at * 60 : -1;
    }
    if (at < 0 || missionticks != at || g_CurrentPlayer->prop == NULL)
    {
        return;
    }
    from = g_CurrentPlayer->prop;
    yaw = g_CurrentPlayer->vv_theta * (M_PI_F / 180.0f);
    if (chrnum >= 0 && coopExtIsHost() && (guard = chrFindByLiteralId(chrnum)) != NULL && guard->prop != NULL && guard->model != NULL)
    {
        from = guard->prop;
        yaw = getsubroty(guard->model);
        dir.f[0] = sinf(yaw);
        dir.f[2] = cosf(yaw);
    }
    else
    {
        guard = NULL;
        dir.f[0] = -sinf(yaw);
        dir.f[2] = cosf(yaw);
    }
    dir.f[1] = 0.05f;
    pos.f[0] = from->pos.f[0] + dir.f[0] * 60.0f;
    pos.f[1] = from->pos.f[1] + (guard != NULL ? 40.0f : 0.0f);
    pos.f[2] = from->pos.f[2] + dir.f[2] * 60.0f;
    w = (WeaponObjRecord *) create_new_item_instance_of_model(item == ITEM_ROCKETROUND ? PROP_CHRROCKET : PROP_CHRGRENADEROUND, item);
    if (w == NULL)
    {
        return;
    }
    matrix_4x4_set_identity(&ident);
    matrix_4x4_set_identity(&rot);
    vel.f[0] = dir.f[0] * (item == ITEM_ROCKETROUND ? 3.0f : 33.333332f);
    vel.f[1] = dir.f[1] * (item == ITEM_ROCKETROUND ? 3.0f : 33.333332f) + (item == ITEM_ROCKETROUND ? 0.0f : 6.0f);
    vel.f[2] = dir.f[2] * (item == ITEM_ROCKETROUND ? 3.0f : 33.333332f);
    w->timer = (item == ITEM_ROCKETROUND) ? -1 : 180;
    gunInitProjectileObject((ObjectRecord *) w, &pos, from->stan, &rot, &vel, &ident, from);
    if (!(w->runtime_bitflags & RUNTIMEBITFLAG_HASPROJECTILE))
    {
        return;
    }
    if (item == ITEM_ROCKETROUND)
    {
        dir.f[0] *= 1.111111f;
        dir.f[1] *= 1.111111f;
        dir.f[2] *= 1.111111f;
        w->projectile->flags |= PROJECTILEFLAG_LAUNCHING | PROJECTILEFLAG_00000020;
        w->projectile->unkB0 = w->runtime_pos.f[1];
        w->projectile->unkB4 = w->projectile->speed.f[1];
        w->projectile->unk10 = dir;
    }
    else
    {
        w->projectile->unk8C = 0.3f;
        w->projectile->unk94 = 0.13333333f;
        w->projectile->refreshrate = 60;
    }
    portConLogf("ge: coop: testround %d from %s\n", item, guard != NULL ? "a guard" : "us");
    coopItemsLaunched((ObjectRecord *) w, &pos, &rot, &vel, item == ITEM_ROCKETROUND ? &dir : NULL, guard);
}

void coopItemsTick(s32 missionticks)
{
    coopItemsRestTick();
    if (sTestAimAt == -2)
    {
        const char *e = getenv("GE_COOP_TESTAIM");

        sTestAimAt = -1;
        if (e != NULL && sscanf(e, "%d:%f:%f", &sTestAimAt, &sTestAimYaw, &sTestAimPitch) >= 1)
        {
            sTestAimAt *= 60;
        }
        e = getenv("GE_COOP_TESTTHROWAT");
        if (e != NULL && sscanf(e, "%d:%d:%f", &sTestAtTag, &sTestAtAt, &sTestAtDist) >= 2)
        {
            sTestAtAt *= 60;
        }
    }
    coopItemsTestThrowAt(missionticks);
    coopItemsTestKnock(missionticks);
    coopItemsTestGrenade(missionticks);
    coopItemsTestRound(missionticks);
    if (coopItemsOn())
    {
        coopItemsSettleTick();
        coopItemsKnockTick();
    }
    if (missionticks == 1)
    {
        sTestAimBase = g_CurrentPlayer->vv_theta;
    }
    if (sTestAimAt >= 0 && missionticks >= sTestAimAt && missionticks < sTestAimAt + 480 && sTestAimBase >= 0.0f)
    {
        f32 yaw = sTestAimBase + sTestAimYaw;

        while (yaw >= 360.0f)
        {
            yaw -= 360.0f;
        }
        while (yaw < 0.0f)
        {
            yaw += 360.0f;
        }
        g_CurrentPlayer->vv_theta = yaw;
        g_CurrentPlayer->vv_verta = sTestAimPitch;
    }
}
