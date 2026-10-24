#ifndef _COOP_H_
#define _COOP_H_
#include <ultra64.h>
#include <bondtypes.h>

void coopStageStart(void);
void coopTick(void);
s32  coopPropIsGhost(PropRecord *prop);

#endif
