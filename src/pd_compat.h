// pd_compat.h — strips out libnds-specific decorators / macros / types so that
// Stella source can build for the Playdate (Cortex-M7) without the libnds
// runtime, FIFO sound subsystem, or two-screen video peripherals.
//
// Include this *before* any emucore header. Everywhere the original code did
// `#include <nds.h>`, point at this file instead.

#ifndef STELLA_PD_COMPAT_H
#define STELLA_PD_COMPAT_H

#ifdef __cplusplus
extern "C" {
#endif

// libnds integer typedefs ----------------------------------------------------
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed char    s8;
typedef signed short   s16;
typedef signed int     s32;
typedef u8  uint8;
typedef u16 uint16;
typedef u32 uint32;

// Section attributes are no-ops on Playdate -- we let the linker map all hot
// code into the standard text section. The Dirty C Secrets writeup notes that
// careful linker placement matters more than DTCM/ITCM tricks for Playdate.
#ifndef ITCM_CODE
#define ITCM_CODE
#endif
#ifndef DTCM_DATA
#define DTCM_DATA
#endif
#ifndef DTCM_BSS
#define DTCM_BSS
#endif

// On the DS, `isDSiMode()` selects faster code paths when running on a DSi.
// Playdate is plenty fast: pretend we are always on a DSi.
#define isDSiMode() (1)

// libnds video & IRQ helpers -- not used in MVP, define as no-ops.
#define swiWaitForVBlank() ((void)0)

// On the DS, BG_GFX maps to the VRAM background-graphics window at a fixed
// address. We don't use it: TIA writes a software 8-bit framebuffer that the
// Playdate side dithers, and the BG_GFX pointer is shoved into a scratch
// buffer in stella_glue.cpp.
extern u16* g_bg_gfx_ptr;
#define BG_GFX (g_bg_gfx_ptr)

// libnds DMA helper -- TIA uses this to push a scanline into VRAM. We just
// memcpy 32-bit words; the Playdate render pass reads from the same buffer.
static inline void dmaCopyWordsAsynch(int /*channel*/, const void* src, void* dst, unsigned bytes)
{
    unsigned int* s = (unsigned int*)src;
    unsigned int* d = (unsigned int*)dst;
    unsigned n = bytes >> 2;
    while (n--) *d++ = *s++;
}

#ifdef __cplusplus
}
#endif

#endif // STELLA_PD_COMPAT_H
