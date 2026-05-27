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

M6532 theM6532;
TIA   theTIA;

extern uInt16 mySoundFreq;

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

  M6502* m6502 = new M6502Low(1);
  theM6532.setConsole(this);

  myCartridge = Cartridge::create(image, size);
  #ifdef HOST_TEST
  fprintf(stderr, "[Console] after Cartridge::create: start=%u num=%u\n",
          myCartInfo.displayStartScanline, myCartInfo.displayNumScalines);
  #endif

  mySoundFreq = 20933;
  if (myCartInfo.soundQuality == SOUND_MUTE)  mySoundFreq = 10466;
  if (myCartInfo.soundQuality == SOUND_10KHZ) mySoundFreq = 10466;
  if (myCartInfo.soundQuality == SOUND_15KHZ) mySoundFreq = 15700;
  if (myCartInfo.soundQuality == SOUND_20KHZ) mySoundFreq = 20933;
  if (myCartInfo.soundQuality == SOUND_30KHZ) mySoundFreq = 31400;

  theTIA.setConsole(this);
  #ifdef HOST_TEST
  fprintf(stderr, "[Console] before Tia_sound_init: start=%u num=%u\n",
          myCartInfo.displayStartScanline, myCartInfo.displayNumScalines);
  #endif
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
