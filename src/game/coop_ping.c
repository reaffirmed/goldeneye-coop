/**
 * Story co-op: pings.
 *
 * The ping key (default the middle mouse button) marks the point under the
 * crosshair for every player in the mission: a diamond in the pinger's colour
 * with their name and the distance, and a square on the radar (coop_hud.c).
 * A guard under the crosshair is pinged as an enemy, and the marker follows
 * that guard while it lives. A ping lasts COOP_PING_LIFE ticks; each player
 * has one at a time, so a new ping moves the old one.
 *
 * The aim is traced like a bullet (chrprop.c chraiDefaultWeaponFireHandler):
 * the stan tiles give how far the floor goes, then every room drawn this frame
 * is tested for the nearest wall, floor or ceiling, then the guards on screen
 * for one nearer still. Nothing is hit, damaged or made to make a sound.
 *
 * A ping rides a reliable COOPEV_PING to everyone; the pinger's own game
 * shows it the same way.
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
#include <music.h>
#include <snd.h>
#include "math_atan2f.h"
#include "player.h"
#include "bondview.h"
#include "bg.h"
#include "matrixmath.h"
#include "stan.h"
#include "chr.h"
#include "chrai.h"
#include "chraction.h"
#include "coop.h"

extern void portConLogf(const char *fmt, ...);
extern void hudmsgBottomShow(char *mess);
extern void bullet_path_from_screen_center(coord3d *arg0, coord3d *arg1, enum GUNHAND arg2);

#define COOPPING_RANGE     20000.0f /* 200 m */
#define COOPPING_COOLDOWN  20       /* ticks between our own pings */
#define COOPPING_CHR_RADIUS 35.0f   /* a guard is a standing cylinder this wide */
#define COOPPING_CHR_HEIGHT 190.0f  /* and this tall, from the floor it stands on */

typedef struct
{
    s32 live;
    s32 age;
    CoopPing ping;
} CoopPingSlot;

static CoopPingSlot sPings[COOP_MAX_PLAYERS];
static s32 sLastPress = -1000;
static s32 sTicks;

/* ---- tracing the aim --------------------------------------------------- */

/* the nearest wall, floor or ceiling along from + dir; 1 and *hitpos, *room when there is one */
static s32 coopPingTraceBg(PropRecord *bond, coord3d *from, coord3d *dir, coord3d *hitpos, s32 *room)
{
    StandTile *tile = bond->stan;
    coord3d dest, end;
    s32 rooms[MAXROOMCOUNT];
    s32 n, i, found = 0;
    f32 best = 1.0e30f;
    f32 wscale = get_room_data_float2();

    dest.x = from->x + dir->x * COOPPING_RANGE;
    dest.y = from->y + dir->y * COOPPING_RANGE;
    dest.z = from->z + dir->z * COOPPING_RANGE;
    end = dest;
    *room = getTileRoom(bond->stan);
    if (walkTilesBetweenPoints_NoCallback(&tile, bond->pos.x, bond->pos.z, from->x, from->z))
    {
        stanResetHits();
        if (!walkTilesBetweenPoints_NoCallback(&tile, from->x, from->z, dest.x, dest.z))
        {
            chrlvStanLineDirIntersection(from, dir, &end);   /* where the walkable floor runs out */
        }
    }

    /* the room we stand in first, then all the others on screen; the nearest wins */
    rooms[0] = getTileRoom(bond->stan);
    n = 1 + bgCopyDrawnRoomsToList(&rooms[1], MAXROOMCOUNT - 1);
    for (i = 0; i < n; i++)
    {
        HitThing hit;
        f32 dx, dy, dz, d;

        if (rooms[i] <= 0 || rooms[i] >= 256 || (i > 0 && rooms[i] == rooms[0]))
        {
            continue;
        }
        if (!bgTestBulletHitBackground(from, &end, rooms[i], &hit))
        {
            continue;
        }
        dx = hit.hitpos.x * wscale - from->x;
        dy = hit.hitpos.y * wscale - from->y;
        dz = hit.hitpos.z * wscale - from->z;
        d = dx * dx + dy * dy + dz * dz;
        if (d < best)
        {
            best = d;
            hitpos->x = hit.hitpos.x * wscale;
            hitpos->y = hit.hitpos.y * wscale;
            hitpos->z = hit.hitpos.z * wscale;
            *room = rooms[i];
            found = 1;
        }
    }
    return found;
}

/* how far along from + dir it passes through a guard's cylinder; -1 if it misses */
static f32 coopPingChrDist(ChrRecord *chr, coord3d *from, coord3d *dir)
{
    PropRecord *prop = chr->prop;
    f32 ox = from->x - prop->pos.x;
    f32 oz = from->z - prop->pos.z;
    f32 a = dir->x * dir->x + dir->z * dir->z;
    f32 b = 2.0f * (ox * dir->x + oz * dir->z);
    f32 c = ox * ox + oz * oz - COOPPING_CHR_RADIUS * COOPPING_CHR_RADIUS;
    f32 disc, t, y;

    if (a < 1.0e-6f)
    {   /* straight up or down: inside the circle or not */
        if (c > 0.0f)
        {
            return -1.0f;
        }
        t = (dir->y < 0.0f) ? (from->y - (chr->ground + COOPPING_CHR_HEIGHT)) : (chr->ground - from->y);
        return (t >= 0.0f) ? t : -1.0f;
    }
    disc = b * b - 4.0f * a * c;
    if (disc < 0.0f)
    {
        return -1.0f;
    }
    t = (-b - sqrtf(disc)) / (2.0f * a);
    if (t < 0.0f)
    {
        t = (-b + sqrtf(disc)) / (2.0f * a);   /* we stand inside it */
    }
    if (t < 0.0f)
    {
        return -1.0f;
    }
    y = from->y + dir->y * t;
    if (y < chr->ground || y > chr->ground + COOPPING_CHR_HEIGHT)
    {
        return -1.0f;
    }
    return t;
}

/* the nearest living guard on screen the aim passes through, nearer than maxdist */
static ChrRecord *coopPingTraceChr(coord3d *from, coord3d *dir, f32 maxdist, f32 *dist)
{
    PropRecord **pp;
    ChrRecord *best = NULL;

    *dist = maxdist;
    for (pp = g_LastOnScreenProp; (--pp) >= g_OnScreenPropList;)
    {
        PropRecord *prop = *pp;
        ChrRecord *chr;
        f32 t;

        if (prop == NULL || prop->type != PROP_TYPE_CHR || (chr = prop->chr) == NULL || coopPropIsPartner(prop))
        {
            continue;
        }
        if (chrIsDead(chr) || (chr->hidden & CHRHIDDEN_REMOVE) || chr->model == NULL)
        {
            continue;
        }
        t = coopPingChrDist(chr, from, dir);
        if (t >= 0.0f && t < *dist)
        {
            *dist = t;
            best = chr;
        }
    }
    return best;
}

/* the ping key */
void coopPingPress(void)
{
    PropRecord *bond = getCurrentPlayerProp();
    coord3d origin, dir, from, hitpos;
    CoopPing ping;
    ChrRecord *chr;
    f32 bgdist = COOPPING_RANGE;
    f32 chrdist;
    s32 room;
    s32 gotbg;

    if (!coopSessionActive() || bond == NULL || bond->stan == NULL)
    {
        return;   /* solo: nobody to ping for */
    }
    if (g_CurrentPlayer->bonddead)
    {
        portConLogf("ge: coop: ping: refused: we are down\n");
        return;
    }
    if (sTicks - sLastPress < COOPPING_COOLDOWN)
    {
        return;
    }
    sLastPress = sTicks;

    /* the crosshair's line, as the right hand's gun fires along it */
    bullet_path_from_screen_center(&origin, &dir, GUNRIGHT);
    from = origin;
    mtx4TransformVecInPlace(currentPlayerGetViewToWorldMtxf(), &from);
    mtx4RotateVecInPlace(currentPlayerGetViewToWorldMtxf(), &dir);
    {
        f32 len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);

        if (len < 1.0e-6f)
        {
            return;
        }
        dir.x /= len;
        dir.y /= len;
        dir.z /= len;
    }

    gotbg = coopPingTraceBg(bond, &from, &dir, &hitpos, &room);
    if (gotbg)
    {
        f32 dx = hitpos.x - from.x, dy = hitpos.y - from.y, dz = hitpos.z - from.z;

        bgdist = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    chr = coopPingTraceChr(&from, &dir, bgdist, &chrdist);

    memset(&ping, 0, sizeof(ping));
    ping.stage = bossGetStageNum();
    ping.chrnum = -1;
    ping.room = (s16) room;
    if (chr != NULL)
    {
        ping.kind = COOPPING_ENEMY;
        ping.chrnum = chr->chrnum;
        ping.pos[0] = chr->prop->pos.x;
        ping.pos[1] = chr->ground + COOPPING_CHR_HEIGHT * 0.5f;
        ping.pos[2] = chr->prop->pos.z;
        if (chr->prop->stan != NULL)
        {
            ping.room = (s16) getTileRoom(chr->prop->stan);
        }
    }
    else if (gotbg)
    {
        ping.kind = COOPPING_SPOT;
        /* a hand's width back toward us, so the marker sits on the surface */
        ping.pos[0] = hitpos.x - dir.x * 5.0f;
        ping.pos[1] = hitpos.y - dir.y * 5.0f;
        ping.pos[2] = hitpos.z - dir.z * 5.0f;
    }
    else
    {
        portConLogf("ge: coop: ping: nothing under the crosshair\n");
        hudmsgBottomShow("Nothing there to ping.\n");
        return;
    }
    portConLogf("ge: coop: ping: %s at %.0f %.0f %.0f (room %d, chr %d), we are at %.0f %.0f %.0f\n",
                ping.kind == COOPPING_ENEMY ? "enemy" : "spot", ping.pos[0], ping.pos[1], ping.pos[2], ping.room,
                ping.chrnum, bond->pos.x, bond->pos.y, bond->pos.z);
    coopPingSend(&ping);
    coopPingReceive(&ping, coopLobbySelfPlayer());
}

/* a ping from player `from` (or our own) */
void coopPingReceive(CoopPing *ping, s32 from)
{
    CoopPingSlot *s;
    char name[17];
    char msg[64];
    const char *n;
    s32 k = 0;

    if (from < 0 || from >= COOP_MAX_PLAYERS || ping->stage != bossGetStageNum() || getCurrentPlayerProp() == NULL)
    {
        portConLogf("ge: coop: ping from player %d dropped (stage %d, we are in %d)\n", from + 1, ping->stage,
                    bossGetStageNum());
        return;
    }
    s = &sPings[from];
    s->ping = *ping;
    s->live = 1;
    s->age = 0;
    {   /* the click from where the ping is, so one off screen is heard from its side (coop_audio.c) */
        coord3d at;

        at.f[0] = ping->pos[0];
        at.f[1] = ping->pos[1];
        at.f[2] = ping->pos[2];
        coopAudioPing(&at, from == coopLobbySelfPlayer());
    }
    portConLogf("ge: coop: ping from player %d: %s at %.0f %.0f %.0f\n", from + 1,
                ping->kind == COOPPING_ENEMY ? "enemy" : "spot", ping->pos[0], ping->pos[1], ping->pos[2]);
    if (from == coopLobbySelfPlayer())
    {
        return;
    }
    for (n = coopPlayerName(from); n != NULL && *n != '\0' && *n != '\n' && k < (s32) sizeof(name) - 1; n++)
    {
        name[k++] = ((u8) *n >= 0x80 || (u8) *n < 0x20) ? '?' : *n;
    }
    name[k] = '\0';
    snprintf(msg, sizeof(msg), (ping->kind == COOPPING_ENEMY) ? "%s spotted an enemy.\n" : "%s pinged a spot.\n", name);
    hudmsgBottomShow(msg);
}

void coopPingStageStart(void)
{
    memset(sPings, 0, sizeof(sPings));
    sLastPress = -1000;
}

/* once a mission tick: age the pings, keep enemy pings on their guard */
void coopPingTick(void)
{
    s32 p;

    sTicks++;
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        CoopPingSlot *s = &sPings[p];

        if (!s->live)
        {
            continue;
        }
        if (++s->age >= COOP_PING_LIFE)
        {
            s->live = 0;
            continue;
        }
        if (s->ping.kind == COOPPING_ENEMY && s->ping.chrnum >= 0)
        {
            ChrRecord *chr = chrFindByLiteralId(s->ping.chrnum);

            if (chr != NULL && chr->prop != NULL && !chrIsDead(chr))
            {
                s->ping.pos[0] = chr->prop->pos.x;
                s->ping.pos[1] = chr->ground + COOPPING_CHR_HEIGHT * 0.5f;
                s->ping.pos[2] = chr->prop->pos.z;
            }
            else if (s->age < COOP_PING_LIFE - 60)
            {
                s->age = COOP_PING_LIFE - 60;   /* down: the marker stays where it fell for a second more */
            }
        }
    }
}

s32 coopPingAt(s32 i, CoopPingMark *out)
{
    s32 p;

    if (getCurrentPlayerProp() == NULL)
    {
        return 0;
    }
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        if (sPings[p].live && i-- == 0)
        {
            break;
        }
    }
    if (p >= COOP_MAX_PLAYERS)
    {
        return 0;
    }
    out->pos.f[0] = sPings[p].ping.pos[0];
    out->pos.f[1] = sPings[p].ping.pos[1];
    out->pos.f[2] = sPings[p].ping.pos[2];
    out->kind = sPings[p].ping.kind;
    out->player = p;
    out->age = sPings[p].age;
    out->name = coopPlayerName(p);
    return 1;
}


/* ---- test harness (coop.c GE_COOP_TESTPING) ------------------------------ */

/* GE_COOP_TESTPINGCHR=<chrnum> turns to that guard (and pitches to its
 * middle) before the press, -1 the nearest living one; otherwise the pitch is
 * `pitch` degrees. GE_COOP_TESTPINGSTEP=<units> first moves us that far from
 * the guard, where there is floor. */
void coopPingTestPress(s32 pitch)
{
    static s32 logged;
    static s32 stepped;
    const char *st = getenv("GE_COOP_TESTPINGSTEP");
    const char *c = getenv("GE_COOP_TESTPINGCHR");
    PropRecord *bond = getCurrentPlayerProp();
    ChrRecord *chr = NULL;

    sLastPress = -1000;
    if (c != NULL && bond != NULL && atoi(c) >= 0)
    {
        chr = chrFindByLiteralId(atoi(c));
    }
    else if (c != NULL && bond != NULL)
    {
        f32 best = 1.0e30f;
        s32 i;

        for (i = 0; i < g_NumChrSlots; i++)
        {
            ChrRecord *k = &g_ChrSlots[i];
            f32 dx, dz;

            if (k->chrnum < 0 || k->prop == NULL || k->model == NULL || coopPropIsPartner(k->prop) || chrIsDead(k))
            {
                continue;
            }
            dx = k->prop->pos.x - bond->pos.x;
            dz = k->prop->pos.z - bond->pos.z;
            if (dx * dx + dz * dz < best)
            {
                best = dx * dx + dz * dz;
                chr = k;
            }
        }
    }
    if (chr != NULL && chr->prop != NULL && st != NULL && !stepped)
    {   /* back from the guard along the line to us, else to a side */
        f32 dx = bond->pos.x - chr->prop->pos.x;
        f32 dz = bond->pos.z - chr->prop->pos.z;
        f32 d = sqrtf(dx * dx + dz * dz);
        f32 far = (f32) atoi(st);
        StandTile *stan = NULL;
        coord3d to;
        s32 k;

        stepped = 1;
        if (d < 1.0f)
        {
            dx = 0.0f;
            dz = d = 1.0f;
        }
        for (k = 0; k < 4 && stan == NULL; k++)
        {
            f32 ax = (k == 0) ? dx / d : (k == 1) ? dz / d : (k == 2) ? -dz / d : -dx / d;
            f32 az = (k == 0) ? dz / d : (k == 1) ? -dx / d : (k == 2) ? dx / d : -dz / d;

            to.x = chr->prop->pos.x + ax * far;
            to.y = chr->prop->pos.y + 50.0f;
            to.z = chr->prop->pos.z + az * far;
            stan = stanFindTileBelowPos(&to, NULL, NULL);
        }
        if (stan != NULL)
        {
            coopRespawnPlayerAt(&to, stan, 0.0f, getCurrentPlayerWeaponId(GUNRIGHT));
            portConLogf("ge: coop: testping: stepped to %.0f %.0f %.0f\n", to.x, to.y, to.z);
            return;   /* face it from the next tick, where we now stand */
        }
    }
    if (chr != NULL && chr->prop != NULL)
    {
        f32 dx = chr->prop->pos.x - bond->pos.x;
        f32 dz = chr->prop->pos.z - bond->pos.z;
        f32 dy = (chr->ground + COOPPING_CHR_HEIGHT * 0.5f) - bond->pos.y;

        g_CurrentPlayer->vv_theta = -(atan2f(dx, dz) * 180.0f) / M_PI_F;
        if (g_CurrentPlayer->vv_theta < 0.0f)
        {
            g_CurrentPlayer->vv_theta += 360.0f;
        }
        g_CurrentPlayer->vv_verta = (atan2f(dy, sqrtf(dx * dx + dz * dz)) * 180.0f) / M_PI_F;
        if (!logged)
        {
            logged = 1;
            portConLogf("ge: coop: testping: facing chr %d at %.0f %.0f %.0f\n", chr->chrnum, chr->prop->pos.x,
                        chr->prop->pos.y, chr->prop->pos.z);
        }
        return;
    }
    g_CurrentPlayer->vv_verta = (f32) pitch;
}
