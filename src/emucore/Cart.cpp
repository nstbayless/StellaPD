// Cart.cpp — minimal Playdate MVP cart factory.
//
// The original StellaDS Cart.cpp shipped a 3500-line MD5-keyed game database
// with per-title settings, plus probe heuristics for every bank-switching
// scheme Stella has ever supported. For the MVP we only need to cover:
//   - 2K, 4K
//   - F8, F8SC
//   - F6, F6SC
//   - FASC (CBS RAM Plus)
//
// We detect the scheme purely from ROM size + a "probably-SC" RAM probe;
// there's no per-game database and no MD5 lookup.

#include "pd_compat.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "bspf.hxx"
#include "Cart.hxx"
#include "Cart2K.hxx"
#include "Cart4K.hxx"
#include "CartF8.hxx"
#include "CartF8SC.hxx"
#include "CartF6.hxx"
#include "CartF6SC.hxx"
#include "CartFASC.hxx"
#include "M6502.hxx"

extern "C" void stella_set_cart_name(const char* name);

// --- globals referenced across the emucore ---------------------------------
uInt8       cart_buffer[MAX_CART_FILE_SIZE];
// fast_cart_buffer holds the active cart image and is read on every
// instruction fetch. It's relocated into DTCM by stella_alloc_buffers()
// before the console is built; until then it's null.
uInt8*      fast_cart_buffer = 0;
uInt8       xl_ram_buffer[32768];
PageAccess  page_access;
uInt32      myCurrentOffset    = 0;
uInt32      myCurrentOffset32  = 0;
uInt16      myCurrentBank      = 0;
// cartDriver is defined by M6502Low.cpp; declared extern in Cart.hxx.
uInt32      myTops[8]          = {0};
uInt32      myBottoms[8]       = {0};
uInt32      myCounters[8]      = {0};
uInt8       myFlags[8]         = {0};
uInt8       myRandomNumber     = 0;
uInt8       myMusicMode[3]     = {0};
uInt32      myMusicCycles      = 0;
uInt8       bSaveStateXL       = 0;
char        my_filename[256]   = {0};

CartInfo       myCartInfo;
GlobalCartInfo myGlobalCartInfo = { 0, 3, 0, 0, 0, 0, 0, 0, 0, 0 };

uInt8 tv_type_requested = NTSC;

// --- SC (SuperChip) probe --------------------------------------------------
static bool isProbablySC_local(const uInt8* image, uInt32 size)
{
  uInt32 banks = size / 4096;
  for (uInt32 b = 0; b < banks; ++b)
  {
    const uInt8* p = image + b * 4096;
    for (uInt32 i = 0; i < 256; ++i)
      if (p[i] != p[i & 0x7F]) return false;
  }
  return true;
}

static void resetCartInfoDefaults()
{
  memset(&myCartInfo, 0, sizeof(myCartInfo));
  myCartInfo.banking              = BANK_4K;
  myCartInfo.controllerType       = CTR_LJOY;
  myCartInfo.special              = SPEC_NONE;
  myCartInfo.frame_mode           = MODE_NO;
  myCartInfo.vblankZero           = 1;
  myCartInfo.hBlankZero           = 1;
  myCartInfo.analogSensitivity    = 10;
  myCartInfo.tv_type              = tv_type_requested;
  myCartInfo.displayStartScanline = 34;
  myCartInfo.displayNumScalines   = 210;
  myCartInfo.yScale               = 100;
  myCartInfo.xOffset              = 0;
  myCartInfo.yOffset              = 0;
  myCartInfo.aButton              = BUTTON_FIRE;
  myCartInfo.bButton              = BUTTON_FIRE;
  myCartInfo.xButton              = BUTTON_FIRE;
  myCartInfo.yButton              = BUTTON_FIRE;
  myCartInfo.soundQuality         = SOUND_20KHZ;
  myCartInfo.left_difficulty      = DIFF_B;
  myCartInfo.right_difficulty     = DIFF_B;
  myCartInfo.thumbOptimize        = 0;
  myCartInfo.palette_type         = 0;
  myCartInfo.bus_driver           = 1;
  myCartInfo.clearRAM             = 0;
  myCartInfo.xStretch             = 0;
}

uInt8 Cartridge::autodetectType(const uInt8* image, uInt32 size)
{
  resetCartInfoDefaults();
  if (size <= 2048 ||
      (size == 4096 && memcmp(image, image + 2048, 2048) == 0))
    return BANK_2K;
  if (size == 4096)  return BANK_4K;
  if (size == 8192)  return isProbablySC_local(image, size) ? BANK_F8SC : BANK_F8;
  if (size == 12288) return BANK_FASC;
  if (size == 16384) return isProbablySC_local(image, size) ? BANK_F6SC : BANK_F6;
  return BANK_4K;
}

Cartridge* Cartridge::create(const uInt8* image, uInt32 size)
{
  uInt8 banking = autodetectType(image, size);
  myCartInfo.banking = banking;
  #ifdef HOST_TEST
  fprintf(stderr, "[Cart::create] after autodetect: banking=%u start=%u num=%u\n",
          banking, myCartInfo.displayStartScanline, myCartInfo.displayNumScalines);
  #endif

  Cartridge* cartridge = 0;
  const char* name = "4K";
  switch (banking) {
    case BANK_2K:   cartridge = new Cartridge2K(image);   name = "2K";   cartDriver = 1; break;
    case BANK_4K:   cartridge = new Cartridge4K(image);   name = "4K";   cartDriver = 1; break;
    case BANK_F8:   cartridge = new CartridgeF8(image);   name = "F8";   cartDriver = 2; break;
    case BANK_F8SC: cartridge = new CartridgeF8SC(image); name = "F8SC"; cartDriver = 6; break;
    case BANK_F6:   cartridge = new CartridgeF6(image);   name = "F6";   cartDriver = 3; break;
    case BANK_F6SC: cartridge = new CartridgeF6SC(image); name = "F6SC"; cartDriver = 7; break;
    case BANK_FASC: cartridge = new CartridgeFASC(image); name = "FASC"; cartDriver = 0; break;
    default:        cartridge = new Cartridge4K(image);   name = "4K?";  cartDriver = 1; break;
  }
  stella_set_cart_name(name);
  return cartridge;
}

Cartridge::Cartridge()  {}
Cartridge::~Cartridge() {}

int  Cartridge::searchForBytes(const uInt8*, uInt32, uInt8, uInt8) { return 0; }
int  Cartridge::searchForBytes3(const uInt8*, uInt32, uInt8, uInt8, uInt8) { return 0; }
int  Cartridge::searchForBytes4(const uInt8*, uInt32, uInt8, uInt8, uInt8, uInt8) { return 0; }
int  Cartridge::searchForBytes5(const uInt8*, uInt32, uInt8, uInt8, uInt8, uInt8, uInt8) { return 0; }
bool Cartridge::isProbablyFE(const uInt8*, uInt32)     { return false; }
bool Cartridge::isProbablyUA(const uInt8*, uInt32)     { return false; }
bool Cartridge::isProbably3F(const uInt8*, uInt32)     { return false; }
bool Cartridge::isProbably3E(const uInt8*, uInt32)     { return false; }
bool Cartridge::isProbably3EPlus(const uInt8*, uInt32) { return false; }
bool Cartridge::isProbablyDPCplus(const uInt8*, uInt32){ return false; }
bool Cartridge::isProbablyCDF(const uInt8*, uInt32)    { return false; }
bool Cartridge::isProbablyEF(const uInt8*, uInt32)     { return false; }
bool Cartridge::isProbablyEFSC(const uInt8*, uInt32)   { return false; }
bool Cartridge::isProbablyDFSC(const uInt8*, uInt32)   { return false; }
bool Cartridge::isProbablyDF(const uInt8*, uInt32)     { return false; }
bool Cartridge::isProbablyBFSC(const uInt8*, uInt32)   { return false; }
bool Cartridge::isProbablyBF(const uInt8*, uInt32)     { return false; }
bool Cartridge::isProbablyE0(const uInt8*, uInt32)     { return false; }
bool Cartridge::isProbablyE7(const uInt8*, uInt32)     { return false; }
bool Cartridge::isProbably0840(const uInt8*, uInt32)   { return false; }
bool Cartridge::isProbably0FA0(const uInt8*, uInt32)   { return false; }
bool Cartridge::isProbably03E0(const uInt8*, uInt32)   { return false; }
bool Cartridge::isProbablyFA2(const uInt8*, uInt32)    { return false; }
bool Cartridge::isProbablySC(const uInt8* image, uInt32 size) { return isProbablySC_local(image, size); }
