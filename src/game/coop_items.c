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
 * Each such item has an id (the player who made it, a serial). Whichever
 * copy goes off or is picked up first sends COOPEV_ITEMGONE; the others go
 * off with it, or leave the world. A dropped gun picked up by one player is
 * picked up by everyone (the pickup rule for weapons); a thrown item picked
 * up goes to that player only.
 *
 * Each world's copy of a blast hurts that world's own Bond, as any blast
 * does (standing on a partner's mine hurts); our watch only sets off our
 * own remote mines.
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
#include "coop.h"

extern void portConLogf(const char *fmt, ...);
extern ObjectRecord *gunCoopSpawnThrown(s32 item, coord3d *pos, Mtxf *rot, coord3d *velocity, Mtxf *spin, s32 timer, PropRecord *owner);
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
extern void matrix_4x4_set_identity(Mtxf *mtx);

enum { COOPITEM_THROWN = 1, COOPITEM_DROP = 2 };
enum { COOPGONE_BOOM = 1, COOPGONE_PICKED = 2 };

typedef struct
{
    u16 id;
    u8 item;            /* ITEM_IDS */
    u8 flags;
    f32 pos[3];         /* where it leaves the hand */
    f32 vel[3];
} CoopThrowMsg;         /* COOPEV_THROW */

typedef struct
{
    u16 id;
    s16 timer;          /* the fuse, as the thrower's game set it */
    s16 rot[4];         /* its orientation, a quaternion x 32767 */
    s16 spin[4];        /* its tumble per tick */
    s32 stage;
} CoopThrowMoreMsg;     /* COOPEV_THROWMORE */

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
} CoopDropMsg;          /* COOPEV_DROP */

#define COOP_ITEMS_MAX 64

typedef struct
{
    u16 id;
    u8 used;
    u8 kind;            /* COOPITEM_* */
    u8 gone;            /* a partner's copy went off: ours is going off with it */
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


static s32 coopItemsOn(void)
{
    return coopEnabled() && !coopVersus() && coopSessionActive();
}

/* the throws worth seeing: everything that is thrown as an object (not knives, which hurt by touch) */
static s32 coopItemsThrowable(s32 item)
{
    switch (item)
    {
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
    if (coopExtRemoteView(from, &view))
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
    coopQuatUnpack(b->spin, &spin);
    sApplying = 1;
    obj = gunCoopSpawnThrown(sPendingThrow.item, &pos, &rot, &vel, &spin, b->timer, owner);
    sApplying = 0;
    if (obj == NULL)
    {
        portConLogf("ge: coop: could not replay player %d's throw (item %d)\n", from + 1, sPendingThrow.item);
        return;
    }
    coopItemAdd(b->id, COOPITEM_THROWN, obj);
    portConLogf("ge: coop: player %d threw item %d (id %04x) at %.0f %.0f %.0f\n", from + 1, sPendingThrow.item, b->id,
                pos.f[0], pos.f[1], pos.f[2]);
}

/* ---- guns the host's guards drop -------------------------------------------- */

/* chr.c: a guard is about to drop what it holds (CHRHIDDEN_DROP_HELD_ITEMS).
 * 1: keep it in hand, the host says what drops */
s32 coopItemsChrDropping(ChrRecord *chr)
{
    if (!coopItemsOn() || sApplying || coopExtIsHost() || chr == NULL || chr->prop == NULL || coopPropIsPartner(chr->prop))
    {
        return 0;
    }
    return 1;
}

/* chr.c: the host's guard dropped prop */
void coopItemsChrDropped(ChrRecord *chr, PropRecord *prop)
{
    WeaponObjRecord *w;
    CoopDropMsg m;

    if (!coopItemsOn() || !coopExtIsHost() || chr == NULL || prop == NULL || prop->type != PROP_TYPE_WEAPON
        || prop->parent != NULL || chr->prop == NULL || coopPropIsPartner(chr->prop))
    {
        return;
    }
    w = prop->weapon;
    if (w == NULL || w->weaponnum <= ITEM_UNARMED)
    {
        return;
    }
    memset(&m, 0, sizeof(m));
    m.id = coopItemNewId();
    m.chrnum = (s16) chr->chrnum;
    m.item = w->weaponnum;
    m.model = (s16) w->obj;
    m.stage = bossGetStageNum();
    m.pos[0] = chr->prop->pos.f[0];
    m.pos[1] = chr->prop->pos.f[1];
    m.pos[2] = chr->prop->pos.f[2];
    coopItemAdd(m.id, COOPITEM_DROP, (ObjectRecord *) w);
    coopExtSend(COOPEV_DROP, 0xFF, &m, sizeof(m));
    portConLogf("ge: coop: guard %d dropped item %d (id %04x)\n", m.chrnum, m.item, m.id);
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
    chr = chrFindByLiteralId(m->chrnum);
    if (chr != NULL && chr->prop != NULL)
    {
        for (child = chr->prop->child; child != NULL; child = child->prev)
        {
            if (child->type == PROP_TYPE_WEAPON && child->weapon != NULL && child->weapon->weaponnum == m->item)
            {
                found = child;
                break;
            }
        }
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
    propobjSetDropped(found, DROPTYPE_DEFAULT);
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
    if (prop == NULL || prop->obj == NULL || !coopItemsOn())
    {
        return;
    }
    it = coopItemByObj(prop->obj);
    if (it == NULL)
    {
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
    if (g->how == COOPGONE_BOOM)
    {
        it->gone = 1;   /* coopItemsExplodeBegin knows it, and sends nothing back */
        ((WeaponObjRecord *) obj)->timer = 0;  /* goes off on its next tick, as a mine the detonator reached */
        portConLogf("ge: coop: player %d's copy of item %04x went off: ours goes too\n", from + 1, g->id);
        return;
    }
    it->used = 0;
    if (it->kind == COOPITEM_DROP && coopInMissionNow())
    {   /* a gun: the pickup rule gives everyone what it holds */
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
 * degrees up (negative down); with GE_COOP_TESTFIRE it aims a throw. */
static s32 sTestAimAt = -2;
static f32 sTestAimYaw, sTestAimPitch, sTestAimBase = -1.0f;

void coopItemsTick(s32 missionticks)
{
    if (sTestAimAt == -2)
    {
        const char *e = getenv("GE_COOP_TESTAIM");

        sTestAimAt = -1;
        if (e != NULL && sscanf(e, "%d:%f:%f", &sTestAimAt, &sTestAimYaw, &sTestAimPitch) >= 1)
        {
            sTestAimAt *= 60;
        }
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
