/* Harness for docs/coop-test-checklist.md (harness only, env switches):
 *
 *   GE_COOP_TESTDUMP=<s>   <s> s into the mission, log every chr and every
 *                          tagged object (number, model, position, room)
 *   GE_COOP_STEPS=<step>@<s>[,<step>@<s>...]  one step each at <s> mission
 *                          seconds, in order:
 *       p<pad>   stand on that pad          c<chr>  stand where that chr is
 *       o<tag>   stand at that object       g<tag>  pick that object up
 *       u<tag>   press Use on that object   b<tag>  break that object
 *       s<chr>   one chest shot at that chr k<chr>  ten chest shots
 *       h<item>  switch to that item        d       die
 *       w<m>     stand on the waypoint pad nearest <m> metres from here
 *       a        raise the alarm             n<chr>  stand 3 to 10 m from that chr, facing it
 *       f<n>     press fire <n> times (throws or plants what's in hand)
 *       v<tag>   stand 1.5 m from that object, facing it
 *       x<tag>   use the gadget in hand on that object (what gunfire.c does when it is aimed at)
 *       l<deg>   look <deg> degrees down (negative up)
 *       e        press Use on the door nearest us (keys and locks apply as in play)
 *       j<chr>:<list>  host: that chr runs ai list <list> (hex) from its start, e.g.
 *                Bunker 2's Natalya following Bond (j<her chr>:40d); a chr
 *                number of 1024 and up (decimal) is the chr running that list now
 *   GE_COOP_WATCH=<chr>[,<chr>...]  every second, log those chrs when they
 *                          appear, vanish, die or move more than 7 m in a
 *                          second (and whether their room is on our screen, or for
 *                          one that vanished, was: "gone (was on our screen)")
 *   GE_COOP_WATCHWORLD=1   log the alarm, the countdown and the stage flags when they change
 *   GE_COOP_WATCHOBJS=1    every second, log each tagged object whose destroyed level goes up
 *                          ("watch: tag N destroyed L, at S s"; the first pass only notes them)
 *   GE_COOP_WATCHDOORS=1   log the open doors once, then each door that opens or closes
 *                          (door number = its byte offset in the setup, as testdoor/testopen)
 *   GE_COOP_REVIVE=1       log any chr that leaves ACT_DIE/ACT_DEAD for another action
 *   GE_COOP_SOLO=1         run the steps with no partner (a host on its own)
 *   GE_COOP_TESTALLOBJ=1   every objective counts as complete (the debug menu's switch),
 *                          so an exit plays the mission's success ending
 */
#include <ultra64.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <bondaicommands.h>
#include <bondgame.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include "bondview.h"
#include "chr.h"
#include "chrai.h"
#include "chraction.h"
#include "player.h"
#include "coop.h"
#include "propobj.h"
#include "objective_status.h"
#include "bondinv.h"
#include "gun.h"
#include "bg.h"
#include "stan.h"
#include "math_atan2f.h"
#include "loadobjectmodel.h"

bool doorIsClosed(DoorRecord *door);
TICKOP propdoorInteract(PropRecord *doorprop);

extern void portConLogf(const char *fmt, ...);
void bondviewCallRecordDamageKills(f32 damage, f32 arg1, s32 arg2, s32 arg3);
void coopLookTestPress(s32 presses);

#define CL_MAXSTEPS 24
#define CL_MAXWATCH 8

typedef struct
{
    char kind;
    s32  arg;
    s32  arg2;  /* j: the ai list */
    s32  at;    /* mission ticks */
} ClStep;

static s32    sInit;
static s32    sRevive, sSolo;
static u8     sLastAct[256];
static s32    sDumpAt = -1;
static ClStep sSteps[CL_MAXSTEPS];
static s32    sNumSteps, sNextStep;
static s32    sWatch[CL_MAXWATCH], sNumWatch;
static s32    sWatchSeen[CL_MAXWATCH];
static coord3d sWatchPos[CL_MAXWATCH];
static s32    sWatchRoom[CL_MAXWATCH];   /* the room it was last in, for a "gone" line */
static s32    sWatchWorld;
static s32    sWatchDoors;
static s32    sWatchObjs, sObjsSeen;
static u8     sObjLevel[256];
#define CL_MAXDOORS 512
static u8     sDoorOpen[CL_MAXDOORS];
static s32    sDoorsSeen;
static u32    sSnapSettle;    /* joiner: the applied snapshot whose doors are not logged settled yet */
static s32    sSnapSettleLeft;
static s32    sLastAlarm = -1, sLastClockRun = -1;
static s32    sLastFlags;
static u32    sLastTicks, sLastSec, sDoneAt;
static s32    sLinger = 20;  /* GE_COOP_STEPSLINGER=<s>: seconds to watch after the last step */

static void clInit(void)
{
    const char *e;

    sInit = 1;
    e = getenv("GE_COOP_TESTDUMP");
    sDumpAt = (e != NULL) ? atoi(e) * 60 : -1;
    e = getenv("GE_COOP_STEPS");
    while (e != NULL && *e != '\0' && sNumSteps < CL_MAXSTEPS)
    {
        ClStep *s = &sSteps[sNumSteps];
        const char *at = strchr(e, '@');

        if (at == NULL)
        {
            break;
        }
        s->kind = e[0];
        s->arg = atoi(e + 1);
        s->arg2 = (strchr(e, ':') != NULL && strchr(e, ':') < at) ? (s32) strtol(strchr(e, ':') + 1, NULL, 16) : 0;
        s->at = atoi(at + 1) * 60;
        sNumSteps++;
        e = strchr(at, ',');
        e = (e != NULL) ? e + 1 : NULL;
    }
    sRevive = getenv("GE_COOP_REVIVE") != NULL;
    sSolo = getenv("GE_COOP_SOLO") != NULL;
    if (getenv("GE_COOP_TESTALLOBJ") != NULL)
    {
        extern s32 debug_all_obj_complete_flag;

        debug_all_obj_complete_flag = 1;
    }
    e = getenv("GE_COOP_WATCH");
    while (e != NULL && *e != '\0' && sNumWatch < CL_MAXWATCH)
    {
        sWatch[sNumWatch++] = atoi(e);
        e = strchr(e, ',');
        e = (e != NULL) ? e + 1 : NULL;
    }
    sWatchWorld = getenv("GE_COOP_WATCHWORLD") != NULL;
    sWatchDoors = getenv("GE_COOP_WATCHDOORS") != NULL;
    sWatchObjs = getenv("GE_COOP_WATCHOBJS") != NULL;
    e = getenv("GE_COOP_STEPSLINGER");
    sLinger = (e != NULL) ? atoi(e) : 20;
}

static s32 clRoom(PropRecord *p)
{
    return (p != NULL && p->stan != NULL) ? (s32) p->stan->room : -1;
}

static void clDump(void)
{
    s32 i;

    for (i = 0; i < g_NumChrSlots; i++)
    {
        ChrRecord *c = &g_ChrSlots[i];

        if (c->model != NULL && c->prop != NULL)
        {
            portConLogf("ge: coop: dump: chr %d pos %.0f %.0f %.0f room %d health %.1f hidden %x\n", c->chrnum,
                        c->prop->pos.f[0], c->prop->pos.f[1], c->prop->pos.f[2], clRoom(c->prop),
                        c->maxdamage - c->damage, c->hidden);
        }
    }
    for (i = 0; i < 256; i++)
    {
        ObjectRecord *o = objFindByTagId(i);

        if (o != NULL && o->prop != NULL)
        {
            portConLogf("ge: coop: dump: tag %d model %d type %d pos %.0f %.0f %.0f room %d destroyed %d\n", i, o->obj, o->type,
                        o->prop->pos.f[0], o->prop->pos.f[1], o->prop->pos.f[2], clRoom(o->prop), objGetDestroyedLevel(o));
        }
    }
}

static void clShoot(ChrRecord *c, s32 times)
{
    coord3d v = { 0.0f, 0.0f, 1.0f };
    s32 i;

    for (i = 0; i < times; i++)
    {
        if (coopExtIsHost())
        {
            handles_shot_actors(c, HIT_CHEST, &v, ITEM_WPPK, TRUE);
        }
        else
        {
            coopClientForwardShot(c, HIT_CHEST, &v, ITEM_WPPK);
        }
    }
}

/* stand at pos: on its own tile when it has one, else on the nearest pad that has one */
static void clGoTo(coord3d *pos, StandTile *stan, s32 weapon)
{
    s32 i, best = -1;
    f32 bestd2 = 1.0e30f;

    if (stan != NULL)
    {
        coopRespawnPlayerAt(pos, stan, 0.0f, weapon);
        return;
    }
    for (i = 0; g_CurrentSetup.pathwaypoints != NULL && g_CurrentSetup.pathwaypoints[i].padID >= 0; i++)
    {
        PadRecord *pad = &g_CurrentSetup.pads[g_CurrentSetup.pathwaypoints[i].padID];
        f32 dx = pad->pos.f[0] - pos->f[0], dy = pad->pos.f[1] - pos->f[1], dz = pad->pos.f[2] - pos->f[2];

        if (pad->stan != NULL && dx * dx + dy * dy * 4.0f + dz * dz < bestd2)
        {
            bestd2 = dx * dx + dy * dy * 4.0f + dz * dz;
            best = g_CurrentSetup.pathwaypoints[i].padID;
        }
    }
    if (best >= 0)
    {
        coord3d p = g_CurrentSetup.pads[best].pos;

        portConLogf("ge: coop: step: no tile there, pad %d instead (%.0f away)\n", best, sqrtf(bestd2));
        coopRespawnPlayerAt(&p, g_CurrentSetup.pads[best].stan, 0.0f, weapon);
    }
}

/* stand on the waypoint pad nearest 6 m from chr (same floor), facing it */
static void clNear(ChrRecord *c, s32 weapon)
{
    s32 i, best = -1;
    f32 bestd = 1.0e30f;

    for (i = 0; g_CurrentSetup.pathwaypoints != NULL && g_CurrentSetup.pathwaypoints[i].padID >= 0; i++)
    {
        PadRecord *pad = &g_CurrentSetup.pads[g_CurrentSetup.pathwaypoints[i].padID];
        f32 dx = pad->pos.f[0] - c->prop->pos.f[0], dy = pad->pos.f[1] - c->prop->pos.f[1], dz = pad->pos.f[2] - c->prop->pos.f[2];
        f32 d = sqrtf(dx * dx + dz * dz);

        if (pad->stan != NULL && dy > -150.0f && dy < 150.0f && d > 200.0f && d < 2500.0f && fabsf(d - 600.0f) < bestd)
        {
            bestd = fabsf(d - 600.0f);
            best = g_CurrentSetup.pathwaypoints[i].padID;
        }
    }
    if (best >= 0)
    {
        coord3d p = g_CurrentSetup.pads[best].pos;
        f32 yaw = atan2f(c->prop->pos.f[0] - p.f[0], c->prop->pos.f[2] - p.f[2]);

        if (yaw < 0.0f)
        {
            yaw += 6.2831855f;
        }
        coopRespawnPlayerAt(&p, g_CurrentSetup.pads[best].stan, yaw, weapon);
    }
    else
    {
        portConLogf("ge: coop: step n%d: no pad 3 to 10 m away\n", c->chrnum);
    }
}

static void clStep(ClStep *s)
{
    s32 weapon = getCurrentPlayerWeaponId(GUNRIGHT);
    ChrRecord *c = NULL;
    ObjectRecord *o = NULL;

    if (s->kind == 'c' || s->kind == 's' || s->kind == 'k' || s->kind == 'n')
    {
        c = chrFindByLiteralId(s->arg);
        if (c == NULL || c->prop == NULL)
        {
            portConLogf("ge: coop: step %c%d: no chr\n", s->kind, s->arg);
            return;
        }
    }
    if (s->kind == 'o' || s->kind == 'g' || s->kind == 'u' || s->kind == 'b' || s->kind == 'v' || s->kind == 'x')
    {
        o = objFindByTagId(s->arg);
        if (o == NULL || o->prop == NULL)
        {
            portConLogf("ge: coop: step %c%d: no object\n", s->kind, s->arg);
            return;
        }
    }
    switch (s->kind)
    {
        case 'p':
        {
            PadRecord *pad = &g_CurrentSetup.pads[s->arg];
            coord3d pos = pad->pos;

            coopRespawnPlayerAt(&pos, pad->stan, 0.0f, weapon);
            break;
        }
        case 'c':
            clGoTo(&c->prop->pos, c->prop->stan, weapon);
            break;
        case 'o':
            clGoTo(&o->prop->pos, o->prop->stan, weapon);
            break;
        case 'g':
        {
            TICKOP op = propPickupByPlayer(o->prop, TRUE);

            propExecuteTickOperation(o->prop, op);
            break;
        }
        case 'u':
        {
            TICKOP op = propobjInteract(o->prop);

            propExecuteTickOperation(o->prop, op);
            break;
        }
        case 'b':
            objApplyDamage(o, 50.0f, &o->prop->pos, ITEM_AK47, 0);
            break;
        case 'n':
            clNear(c, weapon);
            break;
        case 's':
            clShoot(c, 1);
            break;
        case 'k':
            clShoot(c, 10);
            break;
        case 'e':
        {
            PropRecord *me = getCurrentPlayerProp();
            PropRecord *it, *best = NULL;
            f32 bestd = 1.0e30f;

            for (it = g_ActivePropsHead; me != NULL && it != NULL; it = it->next)
            {
                if (it->type == PROP_TYPE_DOOR && it->door != NULL)
                {
                    f32 dx = it->pos.f[0] - me->pos.f[0];
                    f32 dz = it->pos.f[2] - me->pos.f[2];

                    if (dx * dx + dz * dz < bestd)
                    {
                        bestd = dx * dx + dz * dz;
                        best = it;
                    }
                }
            }
            if (best == NULL)
            {
                portConLogf("ge: coop: step e: no door\n");
                break;
            }
            portConLogf("ge: coop: step e: door %d at %.0f, %s before\n",
                        (s32) ((u8 *) best->door - (u8 *) g_CurrentSetup.propDefs), sqrtf(bestd),
                        doorIsClosed(best->door) ? "closed" : "open");
            propExecuteTickOperation(best, propdoorInteract(best));
            break;
        }
        case 'h':
            bondinvAddInvItem(s->arg);
            gunRequestHandWeaponChange(GUNRIGHT, s->arg, 0);
            break;
        case 'd':
            bondviewCallRecordDamageKills(10.0f, 0.0f, -1, 1);
            break;
        case 'w':
        {
            PropRecord *me = getCurrentPlayerProp();
            s32 i, best = -1;
            f32 bestd = 1.0e30f;

            for (i = 0; me != NULL && g_CurrentSetup.pathwaypoints != NULL && g_CurrentSetup.pathwaypoints[i].padID >= 0; i++)
            {
                PadRecord *pad = &g_CurrentSetup.pads[g_CurrentSetup.pathwaypoints[i].padID];
                f32 dx = pad->pos.f[0] - me->pos.f[0], dz = pad->pos.f[2] - me->pos.f[2];
                f32 d = fabsf(sqrtf(dx * dx + dz * dz) - s->arg * 100.0f);

                if (pad->stan != NULL && d < bestd)
                {
                    bestd = d;
                    best = g_CurrentSetup.pathwaypoints[i].padID;
                }
            }
            if (best >= 0)
            {
                coord3d p = g_CurrentSetup.pads[best].pos;

                coopRespawnPlayerAt(&p, g_CurrentSetup.pads[best].stan, 0.0f, weapon);
            }
            break;
        }
        case 'a':
            alarmActivate();
            break;
        case 'v':
        {
            coord3d to;
            StandTile *stan = NULL;
            s32 k;

            for (k = 0; k < 8 && stan == NULL; k++)
            {   /* the first side of it with floor */
                to.f[0] = o->prop->pos.f[0] + 150.0f * cosf(k * 0.7853982f);
                to.f[1] = o->prop->pos.f[1] + 100.0f;
                to.f[2] = o->prop->pos.f[2] + 150.0f * sinf(k * 0.7853982f);
                stan = stanFindTileBelowPos(&to, NULL, NULL);
            }
            if (stan != NULL)
            {
                f32 dx = o->prop->pos.f[0] - to.f[0], dz = o->prop->pos.f[2] - to.f[2];

                coopRespawnPlayerAt(&to, stan, 0.0f, weapon);
                g_CurrentPlayer->vv_theta = atan2f(-dx, dz) * 57.29578f;   /* as GE_COOP_TESTTHROWAT faces it */
                if (g_CurrentPlayer->vv_theta < 0.0f)
                {
                    g_CurrentPlayer->vv_theta += 360.0f;
                }
            }
            else
            {
                portConLogf("ge: coop: step v%d: no floor by it\n", s->arg);
            }
            break;
        }
        case 'x':
            o->state |= PROPSTATE_ACTIVATED;
            coopObjGadgetUsed(o, weapon);
            break;
        case 'f':
            coopLookTestPress(s->arg > 0 ? s->arg : 1);
            break;
        case 'l':
            g_CurrentPlayer->vv_verta = (f32) -s->arg;
            break;
        case 'j':
        {
            s32 i;

            c = (s->arg < 0x400) ? chrFindByLiteralId(s->arg) : NULL;
            for (i = 0; s->arg >= 0x400 && c == NULL && i < g_NumChrSlots; i++)
            {
                if (g_ChrSlots[i].model != NULL && g_ChrSlots[i].prop != NULL && g_ChrSlots[i].ailist == ailistFindById(s->arg))
                {
                    c = &g_ChrSlots[i];
                }
            }
            if (!coopExtIsHost() || c == NULL || ailistFindById(s->arg2) == NULL)
            {
                portConLogf("ge: coop: step j%d: %s\n", s->arg, !coopExtIsHost() ? "host only" : (c == NULL) ? "no chr" : "no such list");
                return;
            }
            c->ailist = ailistFindById(s->arg2);
            c->aioffset = 0;
            c->sleep = 0;
            portConLogf("ge: coop: step j: chr %d runs list %x\n", c->chrnum, s->arg2);
            break;
        }
    }
    portConLogf("ge: coop: step %c%d at %u s done\n", s->kind, s->arg, coopExtMissionTicks() / 60);
}

extern unsigned long long portProfNow(void);   /* pcsched.c: monotonic microseconds */

static double clWallClock(void)
{   /* seconds on the machine's monotonic clock: copies on one machine share it, so
     * two logs' countdowns can be compared at the same moment (not mission time,
     * which each copy starts when it loads the mission) */
    return portProfNow() / 1e6;
}

/* the open and the moving doors, as "a,b" or "none"; returns the number moving */
static s32 clDoorLists(char *open, char *moving, s32 size)
{
    PropDefHeaderRecord *it = (PropDefHeaderRecord *) g_CurrentSetup.propDefs;
    s32 no = 0, nm = 0, lo = 0, lm = 0, k = 0;

    open[0] = moving[0] = '\0';
    for (; it != NULL && it->type != PROPDEF_END && k < CL_MAXDOORS; it = (PropDefHeaderRecord *) ((u32 *) it + sizepropdef(it)))
    {
        DoorRecord *d = (DoorRecord *) it;
        s32 id;

        if (it->type != PROPDEF_DOOR || d->prop == NULL)
        {
            continue;
        }
        k++;
        id = (s32) ((u8 *) it - (u8 *) g_CurrentSetup.propDefs);
        if (d->openstate != DOORSTATE_STATIONARY)
        {
            if (lm < size - 12)
            {
                lm += sprintf(moving + lm, "%s%d", nm ? "," : "", id);
            }
            nm++;
        }
        else if (!doorIsClosed(d) && lo < size - 12)
        {
            lo += sprintf(open + lo, "%s%d", no ? "," : "", id);
            no++;
        }
    }
    if (!no)
    {
        sprintf(open, "none");
    }
    if (!nm)
    {
        sprintf(moving, "none");
    }
    return nm;
}

/* coop_repl.c, GE_COOP_WATCHDOORS: the host starts a join snapshot (sent = 1), or a
 * joiner has applied one. Logs the doors standing open and the doors moving at that
 * moment, tagged with the snapshot, so a check compares the host's doors with the
 * joiner's for the same snapshot; the joiner logs them again once no door moves */
void coopClSnapDoors(s32 sent, u32 snapid)
{
    char open[512], moving[512];

    if (!sWatchDoors || g_CurrentSetup.propDefs == NULL)
    {
        return;
    }
    clDoorLists(open, moving, sizeof(open));
    portConLogf("ge: coop: watch: snapshot %u %s: open %s, moving %s, clock %.2f\n", snapid, sent ? "sent" : "applied",
                open, moving, clWallClock());
    if (!sent)
    {
        sSnapSettle = snapid;
        sSnapSettleLeft = 15;
    }
}

static void clSnapSettle(void)
{   /* once a second after a joiner applied a snapshot: its doors once none moves */
    char open[512], moving[512];
    s32 nm;

    if (sSnapSettleLeft <= 0)
    {
        return;
    }
    nm = clDoorLists(open, moving, sizeof(open));

    if (nm == 0 || --sSnapSettleLeft == 0)
    {
        portConLogf("ge: coop: watch: snapshot %u %s: open %s, moving %s, clock %.2f\n", sSnapSettle,
                    nm == 0 ? "settled" : "never settled", open, moving, clWallClock());
        sSnapSettleLeft = 0;
    }
}

static void clWatchDoors(void)
{   /* GE_COOP_WATCHDOORS: first pass lists the open doors, later passes log changes */
    PropDefHeaderRecord *it = (PropDefHeaderRecord *) g_CurrentSetup.propDefs;
    char list[512];
    s32 n = 0, len = 0, k = 0;

    if (it == NULL || (!coopExtIsHost() && !coopReplSnapshotDone()))
    {   /* a joiner's doors are the host's only once the join snapshot is in */
        return;
    }
    list[0] = '\0';
    for (; it->type != PROPDEF_END && k < CL_MAXDOORS; it = (PropDefHeaderRecord *) ((u32 *) it + sizepropdef(it)))
    {
        s32 id, open;

        if (it->type != PROPDEF_DOOR || ((DoorRecord *) it)->prop == NULL)
        {
            continue;
        }
        id = (s32) ((u8 *) it - (u8 *) g_CurrentSetup.propDefs);
        open = !doorIsClosed((DoorRecord *) it);
        if (!sDoorsSeen)
        {
            if (open && len < (s32) sizeof(list) - 12)
            {
                len += sprintf(list + len, "%s%d", n ? "," : "", id);
            }
        }
        else if (open != sDoorOpen[k])
        {
            portConLogf("ge: coop: watch: door %d %s, clock %.2f\n", id, open ? "open" : "closed", clWallClock());
        }
        n += open;
        sDoorOpen[k++] = (u8) open;
    }
    if (!sDoorsSeen)
    {
        portConLogf("ge: coop: watch: %d doors, open: %s\n", k, n ? list : "none");
        sDoorsSeen = 1;
    }
}

static void clWatchTick(void)
{
    s32 i;

    for (i = 0; i < sNumWatch; i++)
    {
        ChrRecord *c = chrFindByLiteralId(sWatch[i]);
        s32 here = (c != NULL && c->prop != NULL && c->model != NULL);

        if (here != sWatchSeen[i])
        {
            portConLogf("ge: coop: watch: chr %d %s%s, at %u s\n", sWatch[i], here ? "appears" : "gone",
                        here ? (getROOMID_isRendered(clRoom(c->prop)) ? " (on our screen)" : "")
                             : (sWatchSeen[i] && getROOMID_isRendered(sWatchRoom[i])) ? " (was on our screen)" : "",
                        coopExtMissionTicks() / 60);
        }
        if (here)
        {
            f32 dx = c->prop->pos.f[0] - sWatchPos[i].f[0], dz = c->prop->pos.f[2] - sWatchPos[i].f[2];

            if (sWatchSeen[i] && dx * dx + dz * dz > 700.0f * 700.0f)
            {
                portConLogf("ge: coop: watch: chr %d moved %.0f to %.0f %.0f %.0f room %d%s\n", sWatch[i], sqrtf(dx * dx + dz * dz),
                            c->prop->pos.f[0], c->prop->pos.f[1], c->prop->pos.f[2], clRoom(c->prop),
                            getROOMID_isRendered(clRoom(c->prop)) ? " (on our screen)" : "");
            }
            if ((coopExtMissionTicks() / 60) % 10 == 0)
            {
                portConLogf("ge: coop: watch: chr %d at %.0f %.0f %.0f room %d health %.1f\n", sWatch[i], c->prop->pos.f[0],
                            c->prop->pos.f[1], c->prop->pos.f[2], clRoom(c->prop), c->maxdamage - c->damage);
            }
            sWatchPos[i] = c->prop->pos;
            sWatchRoom[i] = clRoom(c->prop);
        }
        sWatchSeen[i] = here;
    }
    if (sWatchWorld)
    {
        s32 alarm = alarmIsActive() ? 1 : 0;
        s32 run = countdownTimerIsRunning() ? 1 : 0;

        if (objectiveregisters1 != sLastFlags)
        {
            portConLogf("ge: coop: watch: stage flags %x (was %x)\n", objectiveregisters1, sLastFlags);
            sLastFlags = objectiveregisters1;
        }
        if (alarm != sLastAlarm)
        {
            portConLogf("ge: coop: watch: alarm %s\n", alarm ? "on" : "off");
            sLastAlarm = alarm;
        }
        if (run != sLastClockRun || run)   /* every second while it runs: a check can place a line against it */
        {
            portConLogf("ge: coop: watch: countdown %s %.1f s at mission %u s, clock %.2f\n", run ? "running" : "stopped",
                        countdownTimerGetValue() / 60.0f, coopExtMissionTicks() / 60, clWallClock());
            sLastClockRun = run;
        }
    }
}

static void clWatchObjs(void)
{   /* GE_COOP_WATCHOBJS: a tagged object blown up or broken further (T4's train) */
    s32 i;

    for (i = 0; i < 256; i++)
    {
        ObjectRecord *o = objFindByTagId(i);
        s32 lv = (o != NULL && o->prop != NULL) ? objGetDestroyedLevel(o) : 0;

        if (sObjsSeen && lv > sObjLevel[i])
        {
            portConLogf("ge: coop: watch: tag %d destroyed %d, at %u s\n", i, lv, coopExtMissionTicks() / 60);
        }
        if (o != NULL && o->prop != NULL)
        {
            sObjLevel[i] = (u8) lv;
        }
    }
    sObjsSeen = 1;
}

static void clReviveTick(void)
{
    s32 i;

    for (i = 0; i < g_NumChrSlots && i < 256; i++)
    {
        ChrRecord *c = &g_ChrSlots[i];
        u8 was = sLastAct[i];

        if (c->model == NULL || c->prop == NULL)
        {
            if (sLastAct[i] != 0)
            {
                portConLogf("ge: coop: revive: slot %d freed\n", i);
            }
            sLastAct[i] = 0;
            continue;
        }
        if (sLastAct[i] == 0)
        {
            portConLogf("ge: coop: revive: slot %d holds chr %d (body %d head %d) health %.1f/%.1f at %.0f %.0f %.0f action %d\n", i,
                        c->chrnum, c->bodynum, c->headnum, c->maxdamage - c->damage, c->maxdamage, c->prop->pos.f[0],
                        c->prop->pos.f[1], c->prop->pos.f[2], c->actiontype);
        }
        if ((was == ACT_DIE || was == ACT_DEAD) && c->actiontype != ACT_DIE && c->actiontype != ACT_DEAD)
        {
            portConLogf("ge: coop: revive: chr %d from action %d to %d, health %.1f\n", c->chrnum, was, c->actiontype,
                        c->maxdamage - c->damage);
        }
        sLastAct[i] = c->actiontype ? c->actiontype : 0xff;
    }
}

void coopChecklistTick(void)
{
    u32 t = coopExtMissionTicks();

    if (!sInit)
    {
        clInit();
    }
    if (t < sLastTicks)
    {   /* a new mission */
        sNextStep = 0;
        memset(sWatchSeen, 0, sizeof(sWatchSeen));
        memset(sLastAct, 0, sizeof(sLastAct));
        sLastAlarm = sLastClockRun = -1;
        sLastFlags = 0;
        sDoorsSeen = 0;
        sSnapSettleLeft = 0;
        sObjsSeen = 0;
        memset(sObjLevel, 0, sizeof(sObjLevel));
    }
    sLastTicks = t;
    if ((!sSolo && !coopLobbyHasPartner()) || t < 2)
    {
        return;
    }
    if (sRevive)
    {
        clReviveTick();
    }
    if (sDumpAt >= 0 && t >= (u32) sDumpAt)
    {
        sDumpAt = -1;
        clDump();
    }
    while (sNextStep < sNumSteps && t >= (u32) sSteps[sNextStep].at)
    {
        clStep(&sSteps[sNextStep++]);
        if (sNextStep == sNumSteps)
        {
            sDoneAt = t + 60 * sLinger;
        }
    }
    if (sDoneAt > 0 && t >= sDoneAt)
    {   /* the rig stops the pair on this line */
        portConLogf("ge: coop: steps: finished\n");
        sDoneAt = 0;
    }
    if (t / 60 != sLastSec)
    {
        sLastSec = t / 60;
        clWatchTick();
        if (sWatchObjs)
        {
            clWatchObjs();
        }
        if (sWatchDoors)
        {
            clWatchDoors();
            clSnapSettle();
        }
    }
}
