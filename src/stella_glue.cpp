// stella_glue.cpp — Playdate-side glue between the C main loop in main.c
// and the C++ Stella emucore. Provides:
//   - C entry points to load a ROM, run a frame, fetch the framebuffer.
//   - Definitions for the global symbols the DS port relied on (frame
//     counters, sound rate, dummy DS hardware pointers, etc).
//   - A pair of TIA frame buffers (160 x 256).

#include "pd_compat.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "emucore/bspf.hxx"
#include "emucore/Cart.hxx"
#include "emucore/Console.hxx"
#include "emucore/Event.hxx"
#include "emucore/EventHandler.hxx"
#include "emucore/TIA.hxx"

// --- TIA framebuffers (8-bit palette indices) ------------------------------
// TIA::reset() zero-fills exactly 160*300 bytes of *each* buffer (see TIA.cpp),
// so the buffer must be at least that big or we'll smash adjacent globals.
#define STELLA_FB_WIDTH   160
#define STELLA_FB_HEIGHT  320        // >= 300, with margin

static uInt8 sFrameBufferA[STELLA_FB_WIDTH * STELLA_FB_HEIGHT];
static uInt8 sFrameBufferB[STELLA_FB_WIDTH * STELLA_FB_HEIGHT];

// TIA.cpp declares these as extern: we provide storage.
extern "C" uInt8* myCurrentFrameBuffer_storage[2];
uInt8* myCurrentFrameBuffer_storage[2] = { sFrameBufferA, sFrameBufferB };
// And TIA.cpp uses `uInt8* myCurrentFrameBuffer[2]` directly -- assigned at
// runtime in stella_init below.

// --- DS-shaped globals that emucore wants ---------------------------------
uInt32 gAtariFrames        = 0;
uInt32 gTotalAtariFrames   = 0;
// gSystemCycles / gTotalSystemCycles are defined by System.cpp.
// bWaveDirectSound / bNoCollisionDetection / bFrameSkipCDFJ are defined by TIA.cpp.

uInt16 mySoundFreq         = 20933;
uInt8  bElevatorAgent      = 0;
uInt8  isCDFJPlus          = 0;
// myRAM[256] is defined by M6532.cpp; PageAccess myPageAccessTable[64] is
// defined by System.cpp. We intentionally don't duplicate them here.

// --- Stub DS hardware pointers (TIA / TIASound write into these blindly) ---
// On the DS these mapped to uncached video memory + audio FIFO buffers. On
// the Playdate we point them at scratch RAM so the writes are harmless. We
// don't read from these — rendering goes through myCurrentFrameBuffer.
static uInt16 sDsScratch[64 * 1024];   // 128 KB

extern "C" {
  uInt16* g_bg_gfx_ptr = sDsScratch;   // referenced via BG_GFX macro in pd_compat
  uInt16* aptr = sDsScratch;           // TIASound DS audio FIFO A
  uInt16* bptr = sDsScratch + 1;       // TIASound DS audio FIFO B
}

// Controller selection (0 = joystick, 1 = paddle). Set from the Playdate
// menu via stella_set_paddle_mode(); consumed by the Console constructor.
int g_controller_is_paddle = 0;

// CPU core selection for STELLA_DUAL_CPU comparison builds (0=Stella, 1=smol).
int g_cpu_core = 0;
extern "C" void stella_set_cpu_core(int core) { g_cpu_core = core ? 1 : 0; }
extern "C" int  stella_get_cpu_core(void)     { return g_cpu_core; }

// CPU register globals (defined in M6502Low.cpp) for the state signature.
extern uInt16 gPC; extern uInt8 A, X, Y, SP;
extern uInt8 N, V, B, D, I, notZ, C;
extern Int32 gSystemCycles;

static uInt32 crc32_update(uInt32 crc, const uInt8* p, uInt32 n)
{
  crc = ~crc;
  for (uInt32 i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1)));
  }
  return ~crc;
}

// Per-frame signature for A/B comparison: framebuffer CRC + CPU-visible state
// (regs, flags, RAM, cycle count). Two cycle-accurate equivalents produce
// identical signatures every frame.
extern "C" void stella_frame_signature(uInt32* fb_crc, uInt32* cpu_crc, uInt32* cycles_out)
{
  if (fb_crc) {
    uInt32 startLine = myCartInfo.displayStartScanline;
    uInt32 numLines  = myCartInfo.displayNumScalines;
    *fb_crc = crc32_update(0, sFrameBufferA + startLine * 160, numLines * 160);
  }
  if (cpu_crc) {
    uInt8 regs[8] = { (uInt8)(gPC & 0xFF), (uInt8)(gPC >> 8), A, X, Y, SP,
                      (uInt8)((N&0x80)|(V?0x40:0)|(B?0x10:0)|(D?8:0)|(I?4:0)|(notZ?0:2)|(C?1:0)),
                      0 };
    uInt32 c = crc32_update(0, regs, 7);
    c = crc32_update(c, myRAM, 128);
    *cpu_crc = c;
  }
  if (cycles_out) *cycles_out = (uInt32)gSystemCycles;
}

// Detailed per-frame fields for pinpointing divergence. out[]: PClo,PChi,A,X,
// Y,SP,P. Returns the RAM CRC. (P excludes the always-set bit5.)
extern "C" uInt32 stella_regs(uInt8 out[7])
{
  out[0] = (uInt8)(gPC & 0xFF); out[1] = (uInt8)(gPC >> 8);
  out[2] = A; out[3] = X; out[4] = Y; out[5] = SP;
  out[6] = (uInt8)((N&0x80)|(V?0x40:0)|(B?0x10:0)|(D?8:0)|(I?4:0)|(notZ?0:2)|(C?1:0));
  return crc32_update(0, myRAM, 128);
}

// Atari paddle resistance range (from StellaDS).
#define PADDLE_MIN_RESISTANCE  70000
#define PADDLE_MAX_RESISTANCE  1030000

// --- Console instance + cart name -----------------------------------------
static Console* sConsole = 0;
static char sCartName[16] = "?";

extern "C" void stella_set_cart_name(const char* name)
{
  if (!name) { sCartName[0] = '?'; sCartName[1] = 0; return; }
  uInt32 i = 0;
  while (i < sizeof(sCartName)-1 && name[i]) { sCartName[i] = name[i]; ++i; }
  sCartName[i] = 0;
}

extern "C" const char* stella_cart_name(void) { return sCartName; }

// --- DTCM buffer relocation -----------------------------------------------
// fast_cart_buffer (cart ROM, read every instruction fetch) and myRAM (Atari
// zero-page, read/written constantly) are the hottest per-instruction memory
// accesses. Relocate them into DTCM (when available) before the console is
// built. 20 KB covers our largest supported cart (F6/F6SC = 16 KB) plus the
// SuperChip RAM some carts carve out of fast_cart_buffer.
#define FAST_CART_BYTES (20 * 1024)
static void* (*s_dtcm_alloc)(size_t) = 0;

// Static backing storage. The cart buffer (16-20 KB) is far too large for the
// stack-region DTCM pool (CrankBoy's trick only safely holds a few KB below
// the live frame), so it lives in regular (cached) RAM, 8-byte aligned.
static uInt8 sFastCartStorage[FAST_CART_BYTES] __attribute__((aligned(8)));
static uInt8 sMyRamStorage[256]                __attribute__((aligned(8)));

extern "C" void stella_set_dtcm_alloc(void* (*fn)(size_t)) { s_dtcm_alloc = fn; }

extern "C" void stella_alloc_buffers(void)
{
  if (fast_cart_buffer && myRAM) return;   // already set
  fast_cart_buffer = sFastCartStorage;
  myRAM            = sMyRamStorage;
  memset(fast_cart_buffer, 0, FAST_CART_BYTES);
  memset(myRAM, 0, 256);
}

// --- C entry points used by main.c ----------------------------------------
extern "C" int stella_init(const uInt8* image, uInt32 size)
{
  if (sConsole) { delete sConsole; sConsole = 0; }

  stella_alloc_buffers();   // ensure fast_cart_buffer / myRAM exist (no-op if done)

  // Make sure TIA sees our framebuffer storage.
  myCurrentFrameBuffer[0] = sFrameBufferA;
  myCurrentFrameBuffer[1] = sFrameBufferB;

  sConsole = new Console(image, size, "rom.a26");
  #ifdef HOST_TEST
  fprintf(stderr, "[stella_init] after Console: addr=%p start=%u num=%u sizeof=%zu\n",
          (void*)&myCartInfo, myCartInfo.displayStartScanline,
          myCartInfo.displayNumScalines, sizeof(CartInfo));
  #endif
  return sConsole != 0;
}

extern "C" void stella_run_frame(void)
{
  if (sConsole) sConsole->update();
}

extern "C" const uInt8* stella_framebuffer(void)
{
  // TIA toggles myCurrentFrameBuffer each update; the most recently rendered
  // frame is at myCurrentFrameBuffer[(myCurrentFrame)^1]. The TIA writes to
  // myFramePointer which is also one of the two. For MVP we just hand the
  // caller buffer A — both buffers tend to get filled in flicker-free mode
  // anyway. We can refine this later.
  return sFrameBufferA;
}

extern "C" uInt32 stella_fb_width(void)  { return STELLA_FB_WIDTH; }
extern "C" uInt32 stella_fb_height(void) { return STELLA_FB_HEIGHT; }

extern "C" uInt32 stella_display_start_scanline(void) { return myCartInfo.displayStartScanline; }
extern "C" uInt32 stella_display_num_scanlines(void)  { return myCartInfo.displayNumScalines; }

// --- Event injection: the C side wants to set joystick + switch state -----
extern "C" void stella_set_input(uInt8 up, uInt8 down, uInt8 left, uInt8 right,
                                  uInt8 fire, uInt8 select_, uInt8 reset_)
{
  if (!sConsole) return;
  Event& ev = *sConsole->eventHandler().event();
  ev.set(Event::JoystickZeroUp,    up    ? 1 : 0);
  ev.set(Event::JoystickZeroDown,  down  ? 1 : 0);
  ev.set(Event::JoystickZeroLeft,  left  ? 1 : 0);
  ev.set(Event::JoystickZeroRight, right ? 1 : 0);
  ev.set(Event::JoystickZeroFire,  fire  ? 1 : 0);
  ev.set(Event::ConsoleSelect,     select_ ? 1 : 0);
  ev.set(Event::ConsoleReset,      reset_ ? 1 : 0);
}

// Set paddle mode (0=joystick, 1=paddle). Takes effect on next stella_init.
extern "C" void stella_set_paddle_mode(int on) { g_controller_is_paddle = on ? 1 : 0; }
extern "C" int  stella_get_paddle_mode(void)   { return g_controller_is_paddle; }

// Feed an analog paddle position in [0..1023] (e.g. from the crank angle):
// 0 maps to maximum resistance (one extreme), 1023 to minimum. Also sets the
// paddle fire button. Drives both paddles 0 and 1 (left jack).
extern "C" void stella_set_paddle(int pos1023, uInt8 fire)
{
  if (!sConsole) return;
  if (pos1023 < 0) pos1023 = 0;
  if (pos1023 > 1023) pos1023 = 1023;
  int res = PADDLE_MAX_RESISTANCE -
            (int)((long long)pos1023 * (PADDLE_MAX_RESISTANCE - PADDLE_MIN_RESISTANCE) / 1023);
  Event& ev = *sConsole->eventHandler().event();
  ev.set(Event::PaddleZeroResistance, res);
  ev.set(Event::PaddleOneResistance,  res);
  ev.set(Event::PaddleZeroFire, fire ? 1 : 0);
  ev.set(Event::PaddleOneFire,  fire ? 1 : 0);
}

// --- C++ runtime placeholders ---------------------------------------------
// We compile with -fno-exceptions / -fno-rtti, and we never throw. Provide
// the bare-minimum operator new/delete + cxa stubs so the linker is happy
// without libsupc++.

void* operator new(__SIZE_TYPE__ sz)       { return malloc(sz); }
void* operator new[](__SIZE_TYPE__ sz)     { return malloc(sz); }
void  operator delete(void* p) noexcept   { free(p); }
void  operator delete[](void* p) noexcept { free(p); }
void  operator delete(void* p, __SIZE_TYPE__) noexcept   { free(p); }
void  operator delete[](void* p, __SIZE_TYPE__) noexcept { free(p); }

extern "C" void __cxa_pure_virtual() { while (1) {} }

#if defined(TARGET_PLAYDATE) && !defined(TARGET_SIMULATOR)
// On the simulator (hosted Linux/macOS) these symbols come from the host
// C/C++ runtime; only the device build needs us to define them.
extern "C" int  __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
extern "C" void* __dso_handle = 0;

// --- newlib syscall stubs --------------------------------------------------
// We don't actually use file I/O / signals / processes -- but the C++
// runtime pulls these in via abort() / iostreams. Provide minimal stubs.
extern "C" {
  void _exit(int)                       { while (1) {} }
  int  _kill(int, int)                  { return -1; }
  int  _getpid(void)                    { return 1; }
  int  _write(int, const char*, int n)  { return n; }
  int  _read(int, char*, int)           { return 0; }
  int  _close(int)                      { return -1; }
  int  _isatty(int)                     { return 0; }
  int  _fstat(int, void*)               { return -1; }
  int  _lseek(int, int, int)            { return -1; }
  // _sbrk is satisfied by the SDK's malloc redirection in setup.c.
}
#endif
