/**
 * Voice chat playback: one stream of decoded voice per player, mixed into
 * the game's own output (audio_pc.c portAudioPump calls portVoiceMix on each
 * frame it renders, before the unfocused mute, so that mute covers voice too).
 *
 * src/game/coop_voice.c decodes what arrives (Steam Voice, 16-bit mono at
 * PORT_VOICE_RATE, the output rate, so nothing is resampled here) and sets
 * each player's left and right gain from where they stand. Packets arrive in
 * bursts, so a stream waits for VOICE_PREBUFFER before it starts and again
 * after it runs dry; a stream that has fallen far behind skips ahead so the
 * delay never builds up.
 *
 * The game tick and the audio pump run on one thread (pcsched.c), so there is
 * no locking.
 */
#include <string.h>

#include "coop_net.h"

extern void portConLogf(const char *fmt, ...);

#define VOICE_PLAYERS    4
#define VOICE_RING       (PORT_VOICE_RATE * 2)      /* 2 s */
#define VOICE_PREBUFFER  (PORT_VOICE_RATE / 10)     /* 100 ms before a stream starts */
#define VOICE_MAXLAG     (PORT_VOICE_RATE * 2 / 5)  /* 400 ms queued: skip to 150 ms */
#define VOICE_KEEP       (PORT_VOICE_RATE * 3 / 20)
#define VOICE_RAMP       (PORT_VOICE_RATE / 50)     /* gains glide over 20 ms */

typedef struct
{
    short ring[VOICE_RING];
    int head;           /* next sample to play */
    int count;          /* samples queued */
    int playing;        /* past the prebuffer */
    float gainL, gainR; /* where the gains are now */
    float wantL, wantR; /* and where they are going */
    int peak;           /* loudest sample of the last frame mixed */
    int dry;            /* times it ran out mid-stream, since the last report */
    int skipped;        /* samples dropped to catch up, since the last report */
} VoiceStream;

static unsigned int sMixed;     /* samples mixed since the last report */

static VoiceStream sVoice[VOICE_PLAYERS];

void portVoicePush(int player, const short *pcm, int n)
{
    VoiceStream *v;
    int i, tail;

    if (player < 0 || player >= VOICE_PLAYERS || pcm == NULL || n <= 0)
    {
        return;
    }
    v = &sVoice[player];
    if (n > VOICE_RING)
    {
        pcm += n - VOICE_RING;
        n = VOICE_RING;
    }
    if (v->count + n > VOICE_RING)
    {   /* full: the oldest goes */
        int drop = v->count + n - VOICE_RING;

        v->head = (v->head + drop) % VOICE_RING;
        v->count -= drop;
    }
    tail = (v->head + v->count) % VOICE_RING;
    for (i = 0; i < n; i++)
    {
        v->ring[tail] = pcm[i];
        tail = (tail + 1 == VOICE_RING) ? 0 : tail + 1;
    }
    v->count += n;
    if (v->playing && v->count > VOICE_MAXLAG)
    {   /* fallen behind (a stall, a burst after loss): catch up */
        int drop = v->count - VOICE_KEEP;

        v->head = (v->head + drop) % VOICE_RING;
        v->count -= drop;
        v->skipped += drop;
    }
}

void portVoiceSetGain(int player, float left, float right)
{
    if (player < 0 || player >= VOICE_PLAYERS)
    {
        return;
    }
    sVoice[player].wantL = left;
    sVoice[player].wantR = right;
}

void portVoiceDrop(int player)
{
    if (player < 0 || player >= VOICE_PLAYERS)
    {
        return;
    }
    sVoice[player].head = 0;
    sVoice[player].count = 0;
    sVoice[player].playing = 0;
    sVoice[player].peak = 0;
}

void portVoiceMix(short *stereo, int frames)
{
    const float step = 1.0f / VOICE_RAMP;
    int p, i;

    if (stereo == NULL || frames <= 0)
    {
        return;
    }
    for (p = 0; p < VOICE_PLAYERS; p++)
    {
        VoiceStream *v = &sVoice[p];
        int peak = 0;

        if (!v->playing)
        {
            if (v->count < VOICE_PREBUFFER)
            {
                v->peak = 0;
                continue;
            }
            v->playing = 1;
            v->gainL = v->wantL;    /* a new stream starts where it is, no glide in */
            v->gainR = v->wantR;
        }
        for (i = 0; i < frames && v->count > 0; i++)
        {
            const int s = v->ring[v->head];
            int l, r;

            v->head = (v->head + 1 == VOICE_RING) ? 0 : v->head + 1;
            v->count--;
            v->gainL += (v->wantL > v->gainL) ? ((v->wantL - v->gainL < step) ? v->wantL - v->gainL : step)
                                              : ((v->gainL - v->wantL < step) ? v->wantL - v->gainL : -step);
            v->gainR += (v->wantR > v->gainR) ? ((v->wantR - v->gainR < step) ? v->wantR - v->gainR : step)
                                              : ((v->gainR - v->wantR < step) ? v->wantR - v->gainR : -step);
            l = stereo[i * 2] + (int) ((float) s * v->gainL);
            r = stereo[i * 2 + 1] + (int) ((float) s * v->gainR);
            stereo[i * 2] = (short) ((l > 32767) ? 32767 : (l < -32768) ? -32768 : l);
            stereo[i * 2 + 1] = (short) ((r > 32767) ? 32767 : (r < -32768) ? -32768 : r);
            if (s > peak || -s > peak)
            {
                peak = (s < 0) ? -s : s;
            }
        }
        v->peak = peak;
        if (v->count == 0)
        {
            v->playing = 0;     /* ran dry: wait for a prebuffer again */
            v->dry++;
        }
    }
    sMixed += (unsigned int) frames;
    if (sMixed >= PORT_VOICE_RATE * 10)
    {   /* every 10 s of output: how the streams kept up (a choppy voice report) */
        for (p = 0; p < VOICE_PLAYERS; p++)
        {
            if (sVoice[p].dry != 0 || sVoice[p].skipped != 0)
            {
                portConLogf("ge: voice: player %d's stream ran dry %d times, %d ms skipped to catch up\n", p + 1,
                            sVoice[p].dry, sVoice[p].skipped * 1000 / PORT_VOICE_RATE);
            }
            sVoice[p].dry = 0;
            sVoice[p].skipped = 0;
        }
        sMixed = 0;
    }
}

int portVoiceLevel(int player)
{
    if (player < 0 || player >= VOICE_PLAYERS)
    {
        return 0;
    }
    return sVoice[player].peak * 100 / 32768;
}
