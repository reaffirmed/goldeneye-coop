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
 *   GE_COOP_WATCH=<chr>[,<chr>...]  every second, log those chrs when they
 *                          appear, vanish, die or move more than 7 m in a
 *                          second (and whether their room is on our screen)
 *   GE_COOP_WATCHWORLD=1   log the alarm, the countdown and the stage flags when they change
 *   GE_COOP_REVIVE=1       log any chr that leaves ACT_DIE/ACT_DEAD for another action
 *   GE_COOP_SOLO=1         run the steps with no partner (a host on its own)
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

extern void portConLogf(const char *fmt, ...);
void bondviewCallRecordDamageKills(f32 damage, f32 arg1, s32 arg2, s32 arg3);
void coopLookTestPress(s32 presses);

#define CL_MAXSTEPS 24
#define CL_MAXWATCH 8

typedef struct
{
    char kind;
    s32  arg;
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
static s32    sWatchWorld;
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
        s->at = atoi(at + 1) * 60;
        sNumSteps++;
        e = strchr(at, ',');
        e = (e != NULL) ? e + 1 : NULL;
    }
    sRevive = getenv("GE_COOP_REVIVE") != NULL;
    sSolo = getenv("GE_COOP_SOLO") != NULL;
    e = getenv("GE_COOP_WATCH");
    while (e != NULL && *e != '\0' && sNumWatch < CL_MAXWATCH)
    {
        sWatch[sNumWatch++] = atoi(e);
        e = strchr(e, ',');
        e = (e != NULL) ? e + 1 : NULL;
    }
    sWatchWorld = getenv("GE_COOP_WATCHWORLD") != NULL;
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
    }
    portConLogf("ge: coop: step %c%d at %u s done\n", s->kind, s->arg, coopExtMissionTicks() / 60);
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
            portConLogf("ge: coop: watch: chr %d %s%s\n", sWatch[i], here ? "appears" : "gone",
                        here ? (getROOMID_isRendered(clRoom(c->prop)) ? " (on our screen)" : "") : "");
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
        if (run != sLastClockRun || (run && (coopExtMissionTicks() / 60) % 5 == 0))
        {
            portConLogf("ge: coop: watch: countdown %s %.1f s\n", run ? "running" : "stopped", countdownTimerGetValue() / 60.0f);
            sLastClockRun = run;
        }
    }
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
    }
}
