#include <ultra64.h>
#include "deb_loadallmodels.h"
#include <memp.h>
#include <random.h>
#include "chr.h"
#include "chrobjdata.h"
#include "initguards.h"
#ifndef TARGET_N64
#include "coop.h"
#endif

void init_guards(void) {
    animation_rate = 1.0f;
    D_8002C904 = 0;
    g_AnimationTablePointerCountRelated = 0;
    D_8002C90C = 0;
    D_8002C910 = 0;
    D_8002CC58 = 0;
    show_patrols_flag = FALSE;
    player1_guardID = 0x1388;
    g_ChrSlots = 0;
    g_NumChrSlots = 0;
    init_obj_register_difficulty_vals();
}

//consolidate with other align_addr_zero macro when cleaning up
#define align_addr_zero(X) ((((X) + 0xF) | 0xF) ^ 0xF)

void alloc_init_GUARDdata_entries(s32 count)
{
    s32 i;
    
    g_NumChrSlots = count + 10; //special guards?
#ifndef TARGET_N64
    g_NumChrSlots += coopDifficultyExtraSlots(); /* co-op difficulty: room for extra guards */
#endif

    g_ChrSlots = mempAllocBytesInBank(align_addr_zero(g_NumChrSlots * sizeof(ChrRecord)), MEMPOOL_STAGE);
    for(i = 0; g_NumChrSlots > i; i++)
    {
        g_ChrSlots[i].model = 0;
    }
}



void bodiesReset(u32 stagenum)
{
    s32 i;
  
    for (i=0; c_item_entries[i].header!=0; i++)
    {
        c_item_entries[i].header->RootNode = (ModelNode*)0;
    }
#if defined(ISGOLDFINGER) || defined(IS_NSNA)
    {
        s32 rnext = randomGetNext();

        for (i = 0; i < 8; i++)
        {
            if (i == 0)
            {
                current_random_male_head[i] = 0;
            }
            else
            {
                current_random_male_head[i] = rnext % i;
            }
            rnext >>= 1;
        };
    }
#elif !defined(TARGET_N64)
    coopLookRandomStart();  /* co-op: the same looks on every game */
    current_random_male_head = coopLookRandom() % num_male_heads;
    current_random_female_head = coopLookRandom() % num_female_heads;
#else
    current_random_male_head = randomGetNext() % num_male_heads;
    current_random_female_head = randomGetNext() % num_female_heads;
#endif
#if !defined(TARGET_N64) && !defined(ISGOLDFINGER) && !defined(IS_NSNA)
    current_random_body = coopLookRandom() % num_bodies;
#else
    current_random_body = randomGetNext() % num_bodies;
#endif
}
