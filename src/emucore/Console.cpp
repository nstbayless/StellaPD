// Console.cpp — MVP rewrite for the Playdate port.
//
// The original Console.cpp wired up the full StellaDS controller matrix
// (paddles, driving controller, keypad, Genesis pad, QuadTari, SaveKey,
// BoosterGrip, two joysticks for special games, etc). For an MVP that only
// targets common joystick games we collapse the lot to two Joysticks and
// nothing else.

#include "pd_compat.h"
#include <stdio.h>
#include <assert.h>
#include <new>

#include "Cart.hxx"
#include "Console.hxx"
#include "Control.hxx"
#include "Event.hxx"
#include "EventHandler.hxx"
#include "Joystick.hxx"
#include "Paddles.hxx"
#include "M6502Low.hxx"
#include "M6532.hxx"
#include "Switches.hxx"
#include "System.hxx"
#include "TIA.hxx"
#include "TIASound.hxx"

// Tried anchoring theTIA / theM6532 into .bss.stellapd_hot too, but it
// shifted the M6502 register file's D-cache set residue and cost ~0.1 fps
// instead of gaining. Leave them in the default BSS section; only the
// items read on every smol-CPU step (M6502 instance + register file +
// gSystemCycles + opdecode_table) benefit from the dedicated hot region.
M6532 theM6532;
TIA   theTIA;

// Pin the M6502Low instance into a fixed, cache-line-aligned slot at the
// start of BSS. The hot interpreter loop in execute_smol does a handful of
// loads through the M6502 `this` pointer on every instruction (mySystem,
// myEnvelope* fields, etc.), and when those fields are heap-allocated the
// instance's address shifts whenever unrelated allocations move the heap
// arena -- different D-cache set residues, different evictions, different
// fps. Anchoring it next to opdecode_table / lumaLUT keeps the residue
// stable regardless of the heap layout.
//
// Console constructs M6502Low exactly once at startup, never destructs.
// Placement-new into this storage; skip delete.
alignas(32) static unsigned char g_m6502_storage[256]
    __attribute__((section(".bss.stellapd_hot.m6502")));

extern uInt16 mySoundFreq;

// Internal audio sample rate. Must be <= sample_freq (31.4 kHz, the TIA's
// own audio clock) or Tia_process loops 16M times per call: with
// playback_freq > sample_freq, Samp_n_max = (sample_freq<<8)/playback_freq
// drops below 256, after which Samp_n_cnt -= 256 wraps a uInt32 into the
// negative range and the high-byte `if (cnt & 0xFF00) continue;` only
// breaks when the counter wraps all the way back around. We pick 15.7 kHz
// (= 1/2 of the TIA clock) -- this also matches the DS port's natural
// producer rate of every-76-cycles (1.19 MHz / 76 ~= 15700 calls/sec),
// so the existing TIA::poke threshold needs no change. The callback in
// stella_glue.cpp resamples 15.7 -> 44.1 kHz with simple phase tracking.
#define STELLA_AUDIO_RATE 15700

Console::Console(const uInt8* image, uInt32 size, const char* /*filename*/)
{
  myControllers[0] = 0;
  myControllers[1] = 0;
  mySwitches = 0;
  mySystem = 0;
  myEvent = 0;

  myEventHandler = new EventHandler(this);
  myEvent = myEventHandler->event();

  mySwitches = new Switches(*myEvent);
  mySystem   = new System(MY_ADDR_SHIFT, MY_PAGE_SHIFT);

  // Placement-new into the fixed BSS slot (declared above). sizeof(M6502Low)
  // must fit; the static_assert inside the .cpp guards that.
  static_assert(sizeof(M6502Low) <= sizeof(g_m6502_storage),
                "g_m6502_storage too small for M6502Low");
  M6502* m6502 = new (g_m6502_storage) M6502Low(1);
  theM6532.setConsole(this);

  myCartridge = Cartridge::create(image, size);
  #ifdef HOST_TEST
  fprintf(stderr, "[Console] after Cartridge::create: start=%u num=%u\n",
          myCartInfo.displayStartScanline, myCartInfo.displayNumScalines);
  #endif

  // Force SOUND_WAVE on the Playdate: that path pushes TIA samples through
  // the tia_buf[] ring (Tia_process), which our audio source callback in
  // stella_glue.cpp drains. The other paths write samples to *aptr/*bptr
  // (the libnds audio FIFO), which on Playdate point at scratch RAM and go
  // nowhere. Override regardless of what the cart-detect code chose.
  myCartInfo.soundQuality = SOUND_WAVE;
  mySoundFreq = STELLA_AUDIO_RATE;

  theTIA.setConsole(this);
  #ifdef HOST_TEST
  fprintf(stderr, "[Console] before Tia_sound_init: start=%u num=%u\n",
          myCartInfo.displayStartScanline, myCartInfo.displayNumScalines);
  #endif
  // Resample the TIA's ~31.4 kHz internal audio clock to the Playdate's
  // fixed 44.1 kHz output rate -- the ring buffer holds samples already at
  // playback rate, so the audio callback can copy them straight out.
  Tia_sound_init(31400, mySoundFreq);
  #ifdef HOST_TEST
  fprintf(stderr, "[Console] after Tia_sound_init: start=%u num=%u\n",
          myCartInfo.displayStartScanline, myCartInfo.displayNumScalines);
  #endif

  // g_controller_is_paddle (set from the Playdate menu) selects paddles for
  // the left jack so the crank can drive paddle resistance (Breakout/Kaboom).
  extern int g_controller_is_paddle;
  if (g_controller_is_paddle) {
    myControllers[0] = new Paddles(Controller::Left, *myEvent);
    myControllers[1] = new Paddles(Controller::Right, *myEvent);
  } else {
    myControllers[0] = new Joystick(Controller::Left, *myEvent);
    myControllers[1] = new Joystick(Controller::Right, *myEvent);
  }

  mySystem->attach(m6502);
  theM6532.install(*mySystem);
  theTIA.install(*mySystem);
  mySystem->attach(myCartridge);

  #ifdef HOST_TEST
  fprintf(stderr, "[Console] before mySystem->reset: start=%u num=%u\n",
          myCartInfo.displayStartScanline, myCartInfo.displayNumScalines);
  #endif
  mySystem->reset();
  #ifdef HOST_TEST
  fprintf(stderr, "[Console] after mySystem->reset: start=%u num=%u\n",
          myCartInfo.displayStartScanline, myCartInfo.displayNumScalines);
  #endif
  fakePaddleResistance = 500000;
}

Console::Console(const Console&) { assert(false); }

Console::~Console()
{
  delete mySystem;
  delete mySwitches;
  delete myControllers[0];
  delete myControllers[1];
  delete myEventHandler;
}

void Console::update() { theTIA.update(); }

Console& Console::operator=(const Console&) { assert(false); return *this; }
