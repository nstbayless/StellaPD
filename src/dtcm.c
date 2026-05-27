// dtcm.c — see dtcm.h. Adapted from CrankBoy (src/dtcm.c).
#include "dtcm.h"
#include <stdlib.h>

// DTCM only exists on the physical device. On the simulator / host harness
// the "stack" is ordinary heap, so fall back to malloc everywhere.
#if !defined(TARGET_PLAYDATE) || defined(TARGET_SIMULATOR)
#undef DTCM_ALLOC
#endif

int is_dtcm_init = 0;

#ifdef DTCM_ALLOC
#define DTCM_CANARY 0xDE0DCA94u
static uint32_t* dtcm_low_canary_addr = NULL;
#endif

static void* dtcm_mempool       = NULL;  // current bump pointer (grows up)
static void* dtcm_mempool_start = NULL;  // base of the pool

void dtcm_set_mempool(void* addr)
{
    if (dtcm_mempool_start != NULL) return;  // set once
    dtcm_mempool_start = addr;
}

void* dtcm_alloc(size_t size)
{
#ifdef DTCM_ALLOC
    if (is_dtcm_init)
    {
        void* tmp = dtcm_mempool;
        *(uint32_t*)dtcm_mempool = 0;                 // clear old high canary
        dtcm_mempool = (void*)(size + (uintptr_t)dtcm_mempool);
        *(uint32_t*)dtcm_mempool = DTCM_CANARY;       // new high canary
        return tmp;
    }
#endif
    return malloc(size);
}

void dtcm_init(void)
{
    if (is_dtcm_init) return;
#ifdef DTCM_ALLOC
    if (dtcm_mempool_start == NULL) return;           // no pool -> stay on malloc
    is_dtcm_init = 1;
    dtcm_mempool = dtcm_mempool_start;
    *(uint32_t*)dtcm_mempool_start = DTCM_CANARY;
    dtcm_low_canary_addr = (uint32_t*)dtcm_alloc(sizeof(uint32_t));
    *dtcm_low_canary_addr = DTCM_CANARY;
#endif
}

int dtcm_verify(const char* context)
{
    (void)context;
#ifdef DTCM_ALLOC
    if (!is_dtcm_init) return 1;
    if (dtcm_low_canary_addr)
    {
        if (*dtcm_low_canary_addr != DTCM_CANARY) return 0;   // shrink PLAYDATE_STACK_SIZE
        if (*(uint32_t*)dtcm_mempool != DTCM_CANARY) return 0; // stack overflow into pool
    }
#endif
    return 1;
}
