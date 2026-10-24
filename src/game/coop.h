#ifndef _COOP_H_
#define _COOP_H_
#include <ultra64.h>
#include <bondtypes.h>

void coopStageStart(void);
void coopTick(void);
s32  coopPropIsGhost(PropRecord *prop);

extern PropRecord *g_CoopPropOverride;
void coopChrTickBegin(ChrRecord *chr);
void coopChrTickEnd(void);
void coopChrStimulus(ChrRecord *chr);
s32  coopTargetIsRemote(void);
void coopGuardTankFlags(s32 flag);
f32  coopTargetDuckingHeight(void);
s32  coopTargetDamageShowTime(void);
s32  coopTargetIsDead(void);
s32  coopTargetRecordDamage(f32 damage, f32 angle);

#endif
