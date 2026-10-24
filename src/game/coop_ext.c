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
}

void coopExtApply(s32 kind, const u8 *raw, s32 from)
{
    switch (kind)
    {
        case COOPEV_CHAT:
            coopChatApply(raw, from);
            break;
    }
}
