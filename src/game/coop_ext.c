/**
 * Story co-op: the feature batch's dispatcher. coop.c hands every event of
 * kind COOPEV_EXT_FIRST..LAST here and calls coopExtTick once a tick (menus
 * too); each feature keeps its own file.
 */
#include <ultra64.h>
#include <bondtypes.h>
#include "coop.h"

void coopExtTick(s32 inmission)
{
    coopChatTick(inmission);
    coopObjTick(inmission);
    coopTeamTimeTick(inmission);
}

void coopExtPacket(const void *pkt, s32 len, s32 from)
{
    const u16 *h = (const u16 *) pkt;

    switch (h[3])
    {
        case COOPPKT_TEAMTIME:
            if (len >= 20)   /* the header and a CoopTeamTimeMsg */
            {
                coopTeamTimeApply((const u8 *) pkt + 8, from);
            }
            break;
    }
}

void coopExtApply(s32 kind, const u8 *raw, s32 from)
{
    switch (kind)
    {
        case COOPEV_CHAT:
            coopChatApply(raw, from);
            break;
        case COOPEV_OBJDONE:
            coopObjApply(kind, raw, from);
            break;
        case COOPEV_TEAMTIME:
            coopTeamTimeApply(raw, from);
            break;
    }
}
