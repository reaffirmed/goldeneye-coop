/**
 * Story co-op: voice chat.
 *
 * Steam Voice records the microphone and compresses it (port/src/coop_steam.c
 * coopSteamVoice*). While we talk, each tick's compressed bytes go out in a
 * COOPPKT_VOICE packet: a joiner sends to the host, the host to every joiner,
 * and the host passes a joiner's packets on to the other joiners (coop.c
 * coopRecvVoice stamps who is talking). Steam only hands over data while
 * someone is actually speaking, so a quiet open mic sends nothing.
 *
 * Talking: [display] voice_mode 0 push to talk (the push_to_talk bind, V),
 * 1 open mic, 2 off. Never while typing a chat line or a password.
 *
 * Hearing: each partner's packets are decoded to 16-bit mono at the output
 * rate and queued in port/src/voice_pc.c, which audio_pc.c mixes into the
 * game's sound. In a mission a voice comes from where that partner stands:
 * panned with the game's own stereo pan (propobj.c portSndPanForPos, so the
 * stereo_pan option covers it) and quieter with distance on the curve guards'
 * sounds use (sub_GAME_7F0537B8, 5000 to 6000 units), but never below
 * COOP_VOICE_FAR_GAIN so a partner across the map can still be heard. In the
 * lobby, or for a partner in no mission with us, it plays in the middle.
 * [display] voice_volume (percent, up to 200) scales every voice.
 *
 * Each player can be muted on this game alone (the lobby's Voice row under
 * their name, the watch's Audio page); a muted partner's packets are dropped
 * before they are decoded. Mutes last for the session.
 *
 * Without Steam (plain UDP tests) or with a steam_api that lacks the voice
 * calls, voice chat is simply off: nothing is recorded or sent.
 *
 * Known gap: while the game is paused or a cutscene holds the controls,
 * coopTick does not read the network, so partners' voices wait until play
 * resumes (and the backlog is skipped). We still send while paused.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <bondgame.h>
#include <PR/libaudio.h>
#include "player.h"
#include "bondview.h"
#include "propobj.h"
#include "coop.h"
#include "coop_net.h"

extern void portConLogf(const char *fmt, ...);
extern s32 portVideoVoiceMode(void);
extern s32 portVideoVoiceVolume(void);
extern s32 portJoyPortPushToTalkHeld(void);
extern s32 portTextInputActive(void);
s32 sub_GAME_7F0537B8(f32 distance, f32 min, f32 max);

#define COOP_VOICE_DATA     1100    /* compressed bytes in one packet */
#define COOP_VOICE_TALK_MS  300     /* a packet keeps the speaking marker this long */
#define COOP_VOICE_FAR_GAIN 0.3f    /* the quietest a partner gets with distance */
#define COOP_VOICE_NEAR     5000.0f /* the guards' distance curve (chrobjSndCreatePostEventDefault) */
#define COOP_VOICE_FAR      6000.0f
#define COOP_VOICE_DECODE   (PORT_VOICE_RATE / 2)   /* half a second of samples per packet at most */

typedef struct
{
    u32 magic;      /* coopExtSendPacket fills these two */
    u16 version;
    u16 type;       /* COOPPKT_VOICE */
    u8 player;      /* who is talking; the host stamps a joiner's */
    u8 pad;
    u16 seq;        /* the talker's packet number */
    u16 len;        /* bytes of data */
    u16 pad2;
    u8 data[COOP_VOICE_DATA];
} CoopVoicePkt;

#define COOP_VOICE_HDR ((s32) (sizeof(CoopVoicePkt) - COOP_VOICE_DATA))

static s32 sMuted[COOP_MAX_PLAYERS];
static u32 sHeardMs[COOP_MAX_PLAYERS];  /* wall clock of their last packet, 0 never */
static u16 sLastSeq[COOP_MAX_PLAYERS];
static s32 sSeqValid[COOP_MAX_PLAYERS];
static u32 sSeqMs[COOP_MAX_PLAYERS];    /* when sLastSeq came: after a quiet spell any number goes (a new talker in that slot) */
static u32 sSentMs;                     /* of our last packet */
static u16 sSeq;
static s32 sRecording;
static s32 sWasSession;
static s32 sTestTalk = -1;              /* GE_COOP_TESTVOICE: open mic regardless of the options (harness) */
static u32 sLogMs[COOP_MAX_PLAYERS];   /* the last gain log line, per player */

/* the watch's Audio page labels its mute rows from these (port_options.c) */
char g_coopVoiceMuteLabel[COOP_MAX_PLAYERS - 1][40];

static u32 coopVoiceNowMs(void)
{
    return (u32) (osGetTime() / 46875ull);
}

s32 coopVoiceAvailable(void)
{
    return coopSessionActive() && coopNetIsSteam() && coopSteamVoiceAvailable() && portVideoVoiceMode() != 2;
}

/* we are sending right now (the HUD's own marker) */
s32 coopVoiceSelfTalking(void)
{
    return sRecording && sSentMs != 0 && coopVoiceNowMs() - sSentMs < COOP_VOICE_TALK_MS;
}

/* we are holding push to talk or have the mic open, whether or not Steam hears us */
s32 coopVoiceSelfOpen(void)
{
    return sRecording;
}

/* player p's voice is reaching us now */
s32 coopVoiceTalking(s32 p)
{
    if (p < 0 || p >= COOP_MAX_PLAYERS || p == coopExtSelf() || sHeardMs[p] == 0)
    {
        return 0;
    }
    return coopVoiceNowMs() - sHeardMs[p] < COOP_VOICE_TALK_MS;
}

s32 coopVoiceMuted(s32 p)
{
    return p >= 0 && p < COOP_MAX_PLAYERS && sMuted[p];
}

void coopVoiceSetMuted(s32 p, s32 muted)
{
    if (p < 0 || p >= COOP_MAX_PLAYERS || p == coopExtSelf())
    {
        return;
    }
    sMuted[p] = muted != 0;
    if (sMuted[p])
    {
        portVoiceDrop(p);
    }
    portConLogf("ge: voice: player %d %s\n", p + 1, sMuted[p] ? "muted" : "unmuted");
}

/* the watch's k-th mute row (from 0): which player, -1 when there is no k-th partner */
s32 coopVoiceMuteRowPlayer(s32 k)
{
    s32 p, n = 0;

    if (!coopSessionActive())
    {
        return -1;
    }
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        if (p == coopExtSelf() || !coopLobbyPlayerHere(p))
        {
            continue;
        }
        if (n == k)
        {
            return p;
        }
        n++;
    }
    return -1;
}

/* which mute rows the watch shows, a bit each (port_watchpages.c rebuilds the page when it changes) */
s32 coopVoiceMuteRowMask(void)
{
    s32 k, mask = 0;

    if (!coopNetIsSteam() || !coopSteamVoiceAvailable())
    {
        return 0;
    }
    for (k = 0; k < COOP_MAX_PLAYERS - 1; k++)
    {
        if (coopVoiceMuteRowPlayer(k) >= 0)
        {
            mask |= 1 << k;
        }
    }
    return mask;
}

/* the Port Options bridge for the watch's mute rows, "voice_mute_1".. (port_options.c) */
static s32 coopVoiceMuteKey(const char *key)
{
    if (strncmp(key, "voice_mute_", 11) != 0 || key[11] < '1' || key[11] >= '1' + COOP_MAX_PLAYERS - 1 || key[12] != '\0')
    {
        return -1;
    }
    return coopVoiceMuteRowPlayer(key[11] - '1');
}

s32 portVoiceOptionGet(const char *key, s32 *out)
{
    const s32 p = coopVoiceMuteKey(key);

    if (p < 0)
    {
        return 0;
    }
    *out = sMuted[p];
    return 1;
}

s32 portVoiceOptionSet(const char *key, s32 value)
{
    const s32 p = coopVoiceMuteKey(key);

    if (p < 0)
    {
        return 0;
    }
    coopVoiceSetMuted(p, value);
    return 1;
}

static void coopVoiceLabels(void)
{
    s32 k;

    for (k = 0; k < COOP_MAX_PLAYERS - 1; k++)
    {
        const s32 p = coopVoiceMuteRowPlayer(k);
        char name[28];

        if (p < 0)
        {
            snprintf(g_coopVoiceMuteLabel[k], sizeof(g_coopVoiceMuteLabel[k]), "Mute Player %d\n", (int) k + 2);
            continue;
        }
        coopAsciiName(name, sizeof(name), coopPlayerName(p), 0);
        snprintf(g_coopVoiceMuteLabel[k], sizeof(g_coopVoiceMuteLabel[k]), "Mute %s\n", name);
    }
}

/* where player p's voice comes from: left and right gains */
static void coopVoiceGains(s32 p, s32 inmission, f32 *left, f32 *right)
{
    const f32 volume = (f32) portVideoVoiceVolume() / 100.0f;
    CoopExtView v;
    f32 gain = 1.0f;
    f32 l = 1.0f, r = 1.0f;

    if (inmission && g_CurrentPlayer != NULL && coopExtRemoteView(p, &v))
    {
        coord3d *ear = bondviewGetCurrentPlayersPosition();
        f32 dx = v.pos.f[0] - ear->f[0];
        f32 dy = v.pos.f[1] - ear->f[1];
        f32 dz = v.pos.f[2] - ear->f[2];
        f32 dist = sqrtf(dx * dx + dy * dy + dz * dz);
        s32 pan = portSndPanForPos(&v.pos);

        gain = (f32) sub_GAME_7F0537B8(dist, COOP_VOICE_NEAR, COOP_VOICE_FAR) / 32767.0f;
        if (gain < COOP_VOICE_FAR_GAIN)
        {
            gain = COOP_VOICE_FAR_GAIN;
        }
        if (pan != AL_PAN_CENTER)
        {   /* equal power, as env.c pans a voice, with the middle at full on both sides */
            const f32 a = (f32) (pan - AL_PAN_LEFT) / (f32) (AL_PAN_RIGHT - AL_PAN_LEFT) * 1.5707963f;

            l = cosf(a) * 1.4142136f;
            r = sinf(a) * 1.4142136f;
            l = (l > 1.0f) ? 1.0f : l;
            r = (r > 1.0f) ? 1.0f : r;
        }
        if (coopVoiceNowMs() - sLogMs[p] >= 2000 && coopVoiceTalking(p))
        {
            sLogMs[p] = coopVoiceNowMs();
            portConLogf("ge: voice: player %d at %dm, pan %d, gain L %.2f R %.2f (volume %d%%)\n", p + 1,
                        (int) (dist / 100.0f + 0.5f), (int) pan, gain * l * volume, gain * r * volume, (int) portVideoVoiceVolume());
        }
    }
    *left = gain * l * volume;
    *right = gain * r * volume;
}

static s32 coopVoiceWantTalk(void)
{
    s32 mode = portVideoVoiceMode();

    if (sTestTalk < 0)
    {
        const char *e = getenv("GE_COOP_TESTVOICE");

        sTestTalk = (e != NULL && atoi(e) != 0);
        if (sTestTalk)
        {
            portConLogf("ge: voice: GE_COOP_TESTVOICE: talking all the time\n");
        }
    }
    if (!coopVoiceAvailable() || coopChatTyping() || coopBrowseTyping() || portTextInputActive())
    {
        return 0;
    }
    if (sTestTalk)
    {
        return 1;
    }
    return mode == 1 || (mode == 0 && portJoyPortPushToTalkHeld());
}

/* once a tick, missions (coopExtTick) and menus */
void coopVoiceTick(s32 inmission)
{
    static CoopVoicePkt pkt;
    static u8 got[8192];
    const s32 session = coopSessionActive();
    const s32 talk = coopVoiceWantTalk();
    s32 p, n;

    if (!session)
    {
        if (sWasSession)
        {   /* the session is over: forget everyone */
            sWasSession = 0;
            coopSteamVoiceRecord(0);
            sRecording = 0;
            for (p = 0; p < COOP_MAX_PLAYERS; p++)
            {
                portVoiceDrop(p);
                sMuted[p] = 0;
                sHeardMs[p] = 0;
                sSeqValid[p] = 0;
            }
        }
        return;
    }
    if (!sWasSession)
    {
        const char *m = getenv("GE_COOP_TESTMUTE");     /* harness: mute player <n> (1 the host) from the start */

        sWasSession = 1;
        if (m != NULL && atoi(m) >= 1 && atoi(m) <= COOP_MAX_PLAYERS)
        {
            coopVoiceSetMuted(atoi(m) - 1, 1);
        }
        portConLogf("ge: voice: %s\n", !coopNetIsSteam() ? "off (needs Steam)"
                                       : !coopSteamVoiceAvailable() ? "off (this steam_api has no voice)"
                                       : portVideoVoiceMode() == 2 ? "off (voice_mode)"
                                       : portVideoVoiceMode() == 1 ? "on, open mic" : "on, push to talk");
    }
    if (talk != sRecording)
    {
        sRecording = talk;
        coopSteamVoiceRecord(talk);
    }

    /* what Steam has recorded (it keeps the last words coming a moment after the key is let go) */
    n = coopSteamVoiceAvailable() ? coopSteamVoiceRead(got, sizeof(got)) : 0;
    if (n > COOP_VOICE_DATA)
    {   /* a backlog (a long stall): too old to be worth sending */
        portConLogf("ge: voice: dropped %d bytes of backlog\n", n);
        n = 0;
    }
    if (n > 0 && portVideoVoiceMode() != 2)
    {
        memcpy(pkt.data, got, n);
        pkt.type = COOPPKT_VOICE;
        pkt.player = (u8) coopExtSelf();
        pkt.seq = ++sSeq;
        pkt.len = (u16) n;
        coopExtSendPacket(&pkt, COOP_VOICE_HDR + n, COOP_ALL);
        if (sSentMs == 0)
        {
            portConLogf("ge: voice: first packet sent (%d bytes)\n", n);
        }
        sSentMs = coopVoiceNowMs();
    }

    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        f32 l, r;

        if (p == coopExtSelf())
        {
            continue;
        }
        coopVoiceGains(p, inmission, &l, &r);
        portVoiceSetGain(p, l, r);
    }
    coopVoiceLabels();
}

/* a COOPPKT_VOICE for us (coop.c has passed it on to the other joiners already) */
void coopVoiceReceive(const void *raw, s32 len)
{
    static s16 pcm[COOP_VOICE_DECODE];
    const CoopVoicePkt *pkt = (const CoopVoicePkt *) raw;
    s32 p, n;

    if (len < COOP_VOICE_HDR || pkt->len > COOP_VOICE_DATA || len < COOP_VOICE_HDR + pkt->len)
    {
        return;
    }
    p = pkt->player;
    if (p >= COOP_MAX_PLAYERS || p == coopExtSelf())
    {
        return;
    }
    if (sSeqValid[p] && coopVoiceNowMs() - sSeqMs[p] < 2000 && (s16) (pkt->seq - sLastSeq[p]) <= 0)
    {
        return;     /* late or doubled: newer words are already playing */
    }
    if (!sSeqValid[p])
    {
        portConLogf("ge: voice: first packet from player %d (%d bytes)%s\n", p + 1, (int) pkt->len, sMuted[p] ? ", muted" : "");
    }
    sSeqValid[p] = 1;
    sLastSeq[p] = pkt->seq;
    sSeqMs[p] = coopVoiceNowMs();
    if (sMuted[p] || portVideoVoiceMode() == 2)
    {
        return;
    }
    sHeardMs[p] = coopVoiceNowMs();
    n = coopSteamVoiceDecode(pkt->data, pkt->len, pcm, COOP_VOICE_DECODE, PORT_VOICE_RATE);
    if (n > 0)
    {
        portVoicePush(p, pcm, n);
    }
}

/* host migration: everyone is numbered again (newof[old] = new, -1 gone) */
void coopVoiceRenumber(const s32 *newof)
{
    s32 muted[COOP_MAX_PLAYERS];
    s32 p;

    memcpy(muted, sMuted, sizeof(muted));
    memset(sMuted, 0, sizeof(sMuted));
    for (p = 0; p < COOP_MAX_PLAYERS; p++)
    {
        portVoiceDrop(p);
        sHeardMs[p] = 0;
        sSeqValid[p] = 0;
        if (newof[p] >= 0 && newof[p] < COOP_MAX_PLAYERS)
        {
            sMuted[newof[p]] = muted[p];
        }
    }
}
