/**
 * Story co-op: smooth movement for what other games move, and the round trip.
 *
 * A partner's STATE comes every tick of their game and a guard snapshot every
 * third tick of the host's, but datagrams arrive in clumps: drawing each one
 * the moment it lands makes partners and guards stutter and jump. Instead
 * each moving thing keeps a short track of timed samples (CoopTrack), and is
 * drawn where it was a little while ago in its sender's time, between the two
 * samples around that moment.
 *
 *  - Sample times are the sender's wall clock (coopNowMs, in each STATE and
 *    guard packet). CoopClock maps them onto ours: the offset is the smallest
 *    arrival minus send time seen (the least delayed packet), allowed to
 *    creep up slowly if the route gets longer for good.
 *  - The delay behind the newest sample (coopClockDelay) is one and a half
 *    send intervals plus twice the arrival jitter, 30..200 ms: enough that
 *    the next sample is nearly always in when it is needed.
 *  - Past the newest sample a track runs on at its last velocity for up to
 *    COOPINTERP_EXTRAP_MS, then holds. A jump of over COOPINTERP_SNAP units
 *    between samples (a respawn, a teleport) is not drawn as a slide.
 *
 * Round trip (COOPPKT_TIME): every half second each game sends each peer a
 * ping with its clock; the answer echoes it. coopNetRttMs(player) is the
 * smoothed round trip to that player (via the host for a joiner's partner).
 */
#include <ultra64.h>
#include <string.h>
#include <math.h>
#include <bondtypes.h>
#include "coop.h"
#include "coop_net.h"

extern void portConLogf(const char *fmt, ...);

#define COOPINTERP_EXTRAP_MS 100
#define COOPINTERP_SNAP      300.0f
#define COOPINTERP_MINDELAY  30
#define COOPINTERP_MAXDELAY  200

void coopClockReset(CoopClock *c)
{
    memset(c, 0, sizeof(*c));
}

void coopClockSample(CoopClock *c, u32 sendms, u32 arrivems)
{
    s32 o = (s32) (arrivems - sendms);

    if (!c->valid)
    {
        c->valid = 1;
        c->off = o;
        c->lastSend = sendms;
        c->interval = 16.0f;
        c->jitter = 0.0f;
        return;
    }
    if (o < c->off)
    {
        c->off = o;     /* a quicker packet: the true offset is at most this */
    }
    else
    {   /* every packet slower than the best: the route got longer, follow it slowly */
        c->creep += (f32) (o - c->off) * 0.002f;
        if (c->creep >= 1.0f)
        {
            c->off += (s32) c->creep;
            c->creep -= (f32) (s32) c->creep;
        }
    }
    c->jitter += ((f32) (o - c->off) - c->jitter) * 0.05f;
    if ((s32) (sendms - c->lastSend) > 0)
    {
        f32 gap = (f32) (sendms - c->lastSend);

        if (gap < 1000.0f)
        {
            c->interval += (gap - c->interval) * 0.05f;
        }
        c->lastSend = sendms;
    }
}

s32 coopClockDelay(const CoopClock *c)
{
    s32 d = (s32) (c->interval * 1.5f + c->jitter * 2.0f + 0.5f);

    if (d < COOPINTERP_MINDELAY)
    {
        d = COOPINTERP_MINDELAY;
    }
    if (d > COOPINTERP_MAXDELAY)
    {
        d = COOPINTERP_MAXDELAY;
    }
    return d;
}

/* the moment, in the sender's clock, to draw its things at now */
u32 coopClockRenderMs(const CoopClock *c, u32 nowms)
{
    return nowms - (u32) c->off - (u32) coopClockDelay(c);
}

void coopTrackReset(CoopTrack *t)
{
    memset(t, 0, sizeof(*t));
}

/* add a sample, keeping the track in time order (a late datagram goes in its place) */
void coopTrackPush(CoopTrack *t, const CoopSample *s)
{
    s32 i;

    if (t->n > 0 && (s32) (s->ms - t->s[t->n - 1].ms) <= 0)
    {
        for (i = 0; i < t->n; i++)
        {
            if (t->s[i].ms == s->ms)
            {
                return;     /* had it */
            }
        }
        if ((s32) (s->ms - t->s[0].ms) < 0)
        {
            return;         /* older than all we keep */
        }
    }
    if (t->n == COOPINTERP_SAMPLES)
    {
        memmove(&t->s[0], &t->s[1], sizeof(t->s[0]) * (COOPINTERP_SAMPLES - 1));
        t->n--;
    }
    i = t->n;
    while (i > 0 && (s32) (s->ms - t->s[i - 1].ms) < 0)
    {
        t->s[i] = t->s[i - 1];
        i--;
    }
    t->s[i] = *s;
    t->n++;
}

static f32 coopAngleLerp(f32 a, f32 b, f32 f)
{
    f32 d = b - a;

    while (d > 3.14159265f)
    {
        d -= 6.2831853f;
    }
    while (d < -3.14159265f)
    {
        d += 6.2831853f;
    }
    a += d * f;
    while (a < 0.0f)
    {
        a += 6.2831853f;
    }
    while (a >= 6.2831853f)
    {
        a -= 6.2831853f;
    }
    return a;
}

static f32 coopDist2(const CoopSample *a, const CoopSample *b)
{
    f32 dx = a->pos[0] - b->pos[0];
    f32 dy = a->pos[1] - b->pos[1];
    f32 dz = a->pos[2] - b->pos[2];

    return dx * dx + dy * dy + dz * dz;
}

/* where the track was at rt (sender's clock); the index of the sample at or
 * before it (whose other fields go with the drawing), -1 for an empty track */
s32 coopTrackAt(const CoopTrack *t, u32 rt, CoopSample *out)
{
    s32 i;

    if (t->n == 0)
    {
        return -1;
    }
    if ((s32) (rt - t->s[0].ms) <= 0)
    {
        *out = t->s[0];
        return 0;
    }
    for (i = 0; i + 1 < t->n; i++)
    {
        const CoopSample *a = &t->s[i];
        const CoopSample *b = &t->s[i + 1];

        if ((s32) (rt - b->ms) < 0)
        {
            f32 f = (f32) (rt - a->ms) / (f32) (b->ms - a->ms);
            s32 k;

            if (coopDist2(a, b) > COOPINTERP_SNAP * COOPINTERP_SNAP)
            {
                *out = *a;  /* a teleport: no slide */
                return i;
            }
            *out = *a;
            for (k = 0; k < 3; k++)
            {
                out->pos[k] = a->pos[k] + (b->pos[k] - a->pos[k]) * f;
            }
            out->yaw = coopAngleLerp(a->yaw, b->yaw, f);
            out->ms = rt;
            return i;
        }
    }
    /* past the newest: on at its speed for a moment, then wait */
    i = t->n - 1;
    *out = t->s[i];
    if (i > 0)
    {
        const CoopSample *a = &t->s[i - 1];
        const CoopSample *b = &t->s[i];
        s32 ahead = (s32) (rt - b->ms);
        s32 span = (s32) (b->ms - a->ms);
        s32 k;

        if (ahead > COOPINTERP_EXTRAP_MS)
        {
            ahead = COOPINTERP_EXTRAP_MS;
        }
        if (span > 0 && span < 250 && coopDist2(a, b) < COOPINTERP_SNAP * COOPINTERP_SNAP)
        {
            for (k = 0; k < 3; k++)
            {
                out->pos[k] = b->pos[k] + (b->pos[k] - a->pos[k]) * (f32) ahead / (f32) span;
            }
        }
    }
    return i;
}
