// dtcm.h — minimal DTCM bump allocator, adapted from CrankBoy's approach.
//
// On the Playdate the stack lives in fast DTCM. The firmware reserves
// STACK_SIZE (set in the Makefile) of it. We only let the real stack use
// PLAYDATE_STACK_SIZE; the remainder of that DTCM-backed region becomes a
// fast bump-allocation pool for our hottest data (cart ROM, zero-page RAM).
//
// dtcm_alloc() hands out DTCM when enabled+initialized, otherwise falls back
// to malloc so the same code path works on the simulator / host harness.

#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Real stack budget left for the C stack; the rest of STACK_SIZE is the pool.
#ifndef PLAYDATE_STACK_SIZE
#define PLAYDATE_STACK_SIZE 0x2700
#endif

extern int   is_dtcm_init;

void  dtcm_set_mempool(void* addr);   // pass __builtin_frame_address(0) - PLAYDATE_STACK_SIZE
void  dtcm_init(void);
void* dtcm_alloc(size_t size);        // DTCM if enabled, else malloc
int   dtcm_verify(const char* context);

#ifdef __cplusplus
}
#endif
