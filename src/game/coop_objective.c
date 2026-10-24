/**
 * Story co-op: who completed an objective, and a toast for it.
 *
 * Every game works out the objectives itself from the shared world (stage
 * flags, rooms, deposits, photos, pickups, the host's guards and objects),
 * so the moment one completes says nothing about who did it. The host
 * decides: coop.c notes the player behind every remote change it applies
 * (coopObjNoteActor: their WORLD bits, pickups, shots on guards and objects,
 * explosions), and our own shots are noted as ours. When an objective turns
 * complete on the host, the credit goes to
 *   - whoever changed the world in the last COOPOBJ_NOW ticks (a room, a
 *     pickup, a photo: the objective turns in the same tick), else
 *   - whoever last did damage in the last COOPOBJ_DAMAGE ticks (a guard's
 *     death or a blast finishes a little after the shot), else
 *   - the host.
 * The host sends COOPEV_OBJDONE with the objective and the player to
 * everyone; each game shows "<NAME> COMPLETED OBJECTIVE A" in that player's
 * colour over the objective's text, unless the credit is its own (the game's own "Objective A:
 * Completed" line covers that).
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include "objective_status.h"
#include "player.h"
#include <bondgame.h>
#include "coop.h"

extern void portConLogf(const char *fmt, ...);
extern u8 *get_text_for_objective(int objectiveIndex);

#define COOPOBJ_NOW     3       /* ticks */
#define COOPOBJ_DAMAGE  300     /* 5 s */

typedef struct
{
    u8 index;       /* the objective's number in the setup */
    u8 letter;      /* A = 0, counting only this difficulty's */
    u8 player;      /* who gets the credit */
    u8 pad;
} CoopObjDone;

static u32 sNow;
static s32 sActor = -1;
static u32 sActorTick;
static s32 sDmgActor = -1;
static u32 sDmgTick;
static u8 sShown[OBJECTIVES_MAX];
static u8 sWasOpen[OBJECTIVES_MAX];    /* seen not complete in this mission: a change to complete is news */

void coopObjNoteActor(s32 player, s32 damage)
{
    if (player < 0 || player >= COOP_MAX_PLAYERS)
    {
        return;
    }
    if (damage)
    {
        sDmgActor = player;
        sDmgTick = sNow;
    }
    else
    {
        sActor = player;
        sActorTick = sNow;
    }
}

static void coopObjToast(s32 index, s32 letter, s32 player)
{
    char body[64];
    char text[100];
    const char *name = coopPlayerName(player);
    const u8 *raw = get_text_for_objective(index);
    s32 n;

    if (index < 0 || index >= OBJECTIVES_MAX || sShown[index])
    {
        return;
    }
    sShown[index] = 1;
    if (player == coopExtSelf())
    {
        return;     /* the game's own line says it */
    }
    snprintf(body, sizeof(body), "%s", (raw != NULL) ? (const char *) raw : "");
    for (n = 0; body[n] != '\0'; n++)
    {
        if (body[n] == '\n')
        {
            body[n] = '\0';
            break;
        }
    }
    snprintf(text, sizeof(text), "%s COMPLETED OBJECTIVE %c|%s", (name != NULL && name[0] != '\0') ? name : "PARTNER",
             'A' + letter, body);
    coopFeedToast(text, player);
}

void coopObjStatusChanged(s32 index, s32 letter, s32 status)
{
    CoopObjDone d;
    s32 credit;

    if (status != OBJECTIVESTATUS_COMPLETE || !coopExtIsHost() || !coopLobbyHasPartner()
        || index < 0 || index >= OBJECTIVES_MAX || !sWasOpen[index] || sShown[index])
    {
        return;
    }
    credit = 0;
    if (sActor >= 0 && sNow - sActorTick <= COOPOBJ_NOW)
    {
        credit = sActor;
    }
    else if (sDmgActor >= 0 && sNow - sDmgTick <= COOPOBJ_DAMAGE)
    {
        credit = sDmgActor;
    }
    portConLogf("ge: coop: objective %d (%c) complete, credit player %d\n", index, 'A' + letter, credit + 1);
    memset(&d, 0, sizeof(d));
    d.index = (u8) index;
    d.letter = (u8) letter;
    d.player = (u8) credit;
    coopExtSend(COOPEV_OBJDONE, 0xFF /* COOP_ALL */, &d, sizeof(d));
    coopObjToast(index, letter, credit);
}

void coopObjApply(s32 kind, const u8 *raw, s32 from)
{
    CoopObjDone d;

    memcpy(&d, raw, sizeof(d));
    if (kind != COOPEV_OBJDONE || from != 0 || d.player >= COOP_MAX_PLAYERS || !coopExtMissionTicks())
    {
        return;
    }
    portConLogf("ge: coop: host says objective %d (%c) was player %d's\n", d.index, 'A' + d.letter, d.player + 1);
    coopObjToast(d.index, d.letter, d.player);
}

/* GE_COOP_TESTOBJDONE=<s>: <s> seconds into the mission, this game counts
 * the first room-entered objective criterion as met, as if it walked in, or
 * if there is none, the GoldenEye key as copied
 * (harness only; the WORLD packet carries it to the host) */
static void coopObjTestTick(s32 inmission)
{
    static s32 at = -2;

    if (at == -2)
    {
        const char *e = getenv("GE_COOP_TESTOBJDONE");

        at = (e != NULL) ? atoi(e) * 60 : -1;
    }
    if (at >= 0 && inmission && (s32) coopExtMissionTicks() == at)
    {
        struct criteria_roomentered *r = ptr_last_enter_room_subobject_entry_type20;
        s32 n = 0;

        for (; r != NULL; r = r->next)
        {
            n++;
        }
        portConLogf("ge: coop: testobjdone: %d room criteria\n", n);
        if (ptr_last_enter_room_subobject_entry_type20 != NULL)
        {
            ptr_last_enter_room_subobject_entry_type20->status = 1;
        }
        else
        {
            g_CurrentPlayer->copiedgoldeneye = 1;   /* Bunker 1's key copy */
        }
        at = -1;
    }
}

void coopObjTick(s32 inmission)
{
    s32 i;

    sNow++;
    coopObjTestTick(inmission);
    if (inmission && coopExtIsHost() && coopExtMissionTicks() > 1)
    {   /* an objective complete from the start ("don't kill civilians") is no one's doing */
        for (i = 0; i < objectiveGetCount() && i < OBJECTIVES_MAX; i++)
        {
            if (objective_ptrs[i] != NULL && get_status_of_objective(i) != OBJECTIVESTATUS_COMPLETE)
            {
                sWasOpen[i] = 1;
            }
        }
    }
    if (!inmission || coopExtMissionTicks() <= 1)
    {   /* a new mission (or the menus): nothing shown yet */
        if (coopExtMissionTicks() <= 1)
        {
            memset(sShown, 0, sizeof(sShown));
            sActor = sDmgActor = -1;
            memset(sWasOpen, 0, sizeof(sWasOpen));
        }
    }
}
