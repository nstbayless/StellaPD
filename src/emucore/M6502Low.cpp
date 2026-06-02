//============================================================================
//
//   SSSS    tt          lll  lll
//  SS  SS   tt           ll   ll
//  SS     tttttt  eeee   ll   ll   aaaa
//   SSSS    tt   ee  ee  ll   ll      aa
//      SS   tt   eeeeee  ll   ll   aaaaa  --  "An Atari 2600 VCS Emulator"
//  SS  SS   tt   ee      ll   ll  aa  aa
//   SSSS     ttt  eeeee llll llll  aaaaa
//
// Copyright (c) 1995-2024 by Bradford W. Mott, Stephen Anthony
// and the Stella Team
//
// This file has been modified by Dave Bernazzani (wavemotion-dave)
// for optimized execution on the DS/DSi platform. Please seek the
// official Stella source distribution which is far cleaner, newer,
// and better maintained.
//
// See the file "License.txt" for information on usage and redistribution of
// this file, and for a DISCLAIMER OF ALL WARRANTIES.
//============================================================================

#include "pd_compat.h"
#include "Cart.hxx"
#include "System.hxx"
#include "M6502Low.hxx"
#include "TIA.hxx"
#include "M6532.hxx"
#include "Random.hxx"

// MVP build: no Supercharger support; forward-declare so the (stripped)
// references in the original sources still satisfy the link.
class CartridgeAR;

// 6502 register file (PC, A, X, Y, SP, flags) -- read and written on every
// emulated instruction by execute_smol / cpu_step_custom. Park them in
// .bss.stellapd_hot, packed into a single 16-byte slab so they share one or
// two D-cache lines and don't get evicted by unrelated BSS reads. Without
// this, the hot interpreter spent ~2x more samples on the `ldr` instructions
// that read these globals (e.g. PC 0x2a8 = `ldr r3,[r7,#0]` where r7 holds
// the literal-pool address of gSystemCycles, which sits near gPC/A/X/Y).
#define CPU_REG __attribute__((section(".bss.stellapd_hot.cpu_regs")))
CPU_REG uInt16 gPC      ;   // Program Counter
CPU_REG uInt8 A         ;   // Accumulator
CPU_REG uInt8 X         ;   // X index register
CPU_REG uInt8 Y         ;   // Y index register
CPU_REG uInt8 SP        ;   // Stack Pointer

CPU_REG uInt8 N         ;   // N flag for processor status register
CPU_REG uInt8 V         ;   // V flag for processor status register
CPU_REG uInt8 B         ;   // B flag for processor status register
CPU_REG uInt8 D         ;   // D flag for processor status register
CPU_REG uInt8 I         ;   // I flag for processor status register
CPU_REG uInt8 notZ      ;   // Z flag complement for processor status register
CPU_REG uInt8 C         ;   // C flag for processor status register
uInt32 *pLocalExecutionStatus            ;   // This is what's used to end a frame when the time comes

uInt32 NumberOfDistinctAccesses          ;         // For AR cart use only - track the # of distinct PC accesses
uInt8  cartDriver                         = 0;     // Set to 1 for carts that are non-banking to invoke faster peek/poke handling
uInt16 f8_bankbit                         = 0x1FFF;// We use this as a bit of a speed-hack for 8K games so we can bank/mask quickly
uInt8  myDataBusState                     = 0x00;  // Last state of the data bus (needed for maximum accuracy drivers)

CartridgeAR* myAR = 0;

Int32 debug[40] = {0};
int *stack_executionStatus               = (int*)&debug[19];

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
M6502Low::M6502Low(uInt32 systemCyclesPerProcessorCycle)
    : M6502(systemCyclesPerProcessorCycle)
{
    Random random;
    NumberOfDistinctAccesses = 0;
    cartDriver = 0;
    A = random.next() & 0xFF;
    X = random.next() & 0xFF;
    Y = random.next() & 0xFF;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
M6502Low::~M6502Low()
{
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
const char* M6502Low::name() const
{
  return "M6502Low";
}

// -----------------------------------------------------------------------------------
// These handle what are known as 'phantom reads and writes'. This is a side-effect
// of some 6502 instructions where the bus contains an intermediate value. Generally
// its not needed to emulate this perfectly so we skip the actual read which takes
// time and just chew up the cycles that would be required. Speed over accuracy here.
// -----------------------------------------------------------------------------------
#define fake_peek()  gSystemCycles++;
#define fake_poke()  gSystemCycles++;

// -------------------------------------------------------------------------------
// This is the normal driver - optimized as best we can. Note that this is the 
// only drive in which we are setting the bus state to the last value that 
// would be presented on the BUS. The myDataBusState is used in the TIA::peek()
// handler to drive unused bits for a few "buggy" games that require this for
// proper operation [note: the games themselves are not really buggy but they 
// rely on the undriven TIA bus pins to reflect the most recent data that was
// written to the bus... in general, games shouldn't rely on this behavior and
// some later 2600 cost-reduced units will not reflect the last bits on the bus
// in this way and those few carts that rely on it may not work...]
// -------------------------------------------------------------------------------
inline uInt8 peek(uInt16 address)
{
  gSystemCycles++;

  PageAccess& access = myPageAccessTable[(address & MY_ADDR_MASK) >> MY_PAGE_SHIFT];
  if(access.directPeekBase != 0) myDataBusState =  *(access.directPeekBase + (address & MY_PAGE_MASK));
  else myDataBusState = access.device->peek(address);

  return myDataBusState;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
inline uInt8 peek_PC(uInt32 address)
{
  gSystemCycles++;

  PageAccess& access = myPageAccessTable[(address & MY_ADDR_MASK) >> MY_PAGE_SHIFT];
  if(access.directPeekBase != 0) myDataBusState = *(access.directPeekBase + (address & MY_PAGE_MASK));
  else myDataBusState = access.device->peek(address);

  return myDataBusState;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
inline void poke(uInt16 address, uInt8 value)
{
  gSystemCycles++;

  PageAccess& access = myPageAccessTable[(address & MY_ADDR_MASK) >> MY_PAGE_SHIFT];
  if(access.directPokeBase != 0) *(access.directPokeBase + (address & MY_PAGE_MASK)) = value;
  else access.device->poke(address, value);

  myDataBusState = value;
}


inline uInt8 peek_zpg(uInt16 address)
{
  gSystemCycles++;

  if (address & 0x80) myDataBusState = myRAM[address & 0x7F];
  else
  {
     // Unfortunately we can't just blindly call TIA as some carts (3E, 3F, WD) have hotspots here... so call the device handler for the ZPG
     myDataBusState = myPageAccessTable[0].device->peek(address);
  }

  return myDataBusState;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void M6502Low::execute(void)
{
    uInt16 operandAddress;

    int executionStatus=0;
    stack_executionStatus = &executionStatus;

    uInt32 PC = gPC;  // Move PC local so compiler can optimize/registerize

    // -------------------------------------------------------------------------------------------------------------
    // vBlankIntr() will check for more than 32K instructions in a frame and issue the STOP bit in ExecutionStatus
    // -------------------------------------------------------------------------------------------------------------
    while (!executionStatus)
    {
      #define operand myDataBusState
      // Get the next 6502 instruction - do this the fast way!
      ++gSystemCycles;
      PageAccess& access = myPageAccessTable[(PC & MY_ADDR_MASK) >> MY_PAGE_SHIFT];
      if (access.directPeekBase != 0) operand = *(access.directPeekBase + (PC & MY_PAGE_MASK));
      else operand = access.device->peek(PC);
      PC++;

      // 6502 instruction emulation is generated by an M4 macro file
      switch (operand)
      {
        #include "M6502Low.ins"
      }
      #undef operand
    }
    gPC = PC;
}


void M6502Low::illegal_op(uInt8 operand)
{
    (void)operand;
}


// ==============================================================================
// Special Non-Banked (2k and 4k carts) handling. This is highly optimized
// for a cart whose memory can be entirely mapped into the 6502 address space
// and doesn't require us to check for hot spots or other things that slow us down.
// This gets us about 15% speed boost on these games and makes them playable
// on the older/slower DS-LITE/PHAT hardware. Be warned - since we do very
// little checking here, badly behaved games which try to write to ROM or do
// other strange things like rely on the state of undriven TIA pins will fail
// using this optimized driver. Those will need to use the normal cart driver.
// ==============================================================================
inline uInt8 peek_4K_PC(uInt32 address)
{
  gSystemCycles++;
  return fast_cart_buffer[address & 0xFFF];
}


inline uInt8 peek_4K(uInt16 address)
{
  gSystemCycles++;

  if (unlikely(address & 0x1000))
  {
      return fast_cart_buffer[address & 0xFFF];
  }
  else
  {
      // Note: this is not perfectly accurate to mimic the real Atari 2600 incomplete decoding address to
      // provide a true representation of lower memory mirrors. But it's good enough for well-behaved carts.
      if (address & 0x200) return theM6532.peek(address);
      else if (address & 0x80) return myRAM[address & 0x7F];
      else return theTIA.peek(address);
  }
}


inline void poke_4K(uInt16 address, uInt8 value)
{
  gSystemCycles++;

  // Note: this is not perfectly accurate to mimic the real Atari 2600 incomplete decoding address to
  // provide a true representation of lower memory mirrors. But it's good enough for well-behaved carts.
  if (address & 0x200) theM6532.poke(address, value);
  else if (address & 0x80) myRAM[address & 0x7F] = value;
  else theTIA.poke(address, value);
}

void M6502Low::execute_4K(void)
{
    // Clear all of the execution status bits
    int executionStatus=0;
    stack_executionStatus = &executionStatus;
    uInt32 PC = gPC;  // Move PC local so compiler can optimize/registerize

    // -------------------------------------------------------------------------------------------------------------
    // vBlankIntr() will check for more than 32K instructions in a frame and issue the STOP bit in ExecutionStatus
    // -------------------------------------------------------------------------------------------------------------
    while (!executionStatus)
    {
      uInt16 operandAddress;
      // Get the next 6502 instruction - do this the fast way!
      ++gSystemCycles;
      uInt8 operand = fast_cart_buffer[PC++ & 0xFFF];

      // 6502 instruction emulation is generated by an M4 macro file
      switch (operand)
      {
        // A trick of the light... here we map peek/poke to the "NB" cart versions. This improves speed for non-bank-switched carts.
        #define peek     peek_4K
        #define peek_zpg peek_4K
        #define peek_PC  peek_4K_PC
        #define poke     poke_4K
        #include "M6502Low.ins"
        #undef peek
        #undef peek_zpg
        #undef peek_PC
        #undef poke
      }
    }
    gPC = PC;
}

// -------------------------------------------------------------------------------
// Special F8 driver for much faster speeds but comes at a cost of
// compatibility - so in cart.cpp we enable this only for the
// well-behaved carts that can support it...
// -------------------------------------------------------------------------------

inline uInt8 peek_PCF8(uInt16 address)
{
  gSystemCycles++;
  return fast_cart_buffer[address & f8_bankbit];
}


inline uInt8 peek_F8(uInt16 address)
{
  gSystemCycles++;

  if (address & 0x1000)
  {
      if ((address & 0xFFF) == 0x0FF8) f8_bankbit=0x0FFF;
      else if ((address & 0xFFF) == 0x0FF9) f8_bankbit=0x1FFF;
      return fast_cart_buffer[address & f8_bankbit];
  }
  else
  {
      // Note: this is not perfectly accurate to mimic the real Atari 2600 incomplete decoding address to
      // provide a true representation of lower memory mirrors. But it's good enough for well-behaved carts.
      if (address & 0x200) return theM6532.peek(address);
      else if (address & 0x80) return myRAM[address & 0x7F];
      else return theTIA.peek(address);
  }
}


inline void poke_F8(uInt16 address, uInt8 value)
{
  gSystemCycles++;

  if (unlikely(address & 0x1000))
  {
      if ((address & 0x0FFF) == 0x0FF8) f8_bankbit=0x0FFF;
      else if ((address & 0x0FFF)) f8_bankbit=0x1FFF;
  }
  else
  {
      // Note: this is not perfectly accurate to mimic the real Atari 2600 incomplete decoding address to
      // provide a true representation of lower memory mirrors. But it's good enough for well-behaved carts.
      if (address & 0x200) theM6532.poke(address, value);
      else if (address & 0x80) myRAM[address & 0x7F] = value;
      else theTIA.poke(address, value);
  }
}


void M6502Low::execute_F8(void)
{
    uInt16 operandAddress;
    uInt32 PC = gPC;  // Move PC local so compiler can optimize/registerize

    // Clear all of the execution status bits
    int executionStatus=0;
    stack_executionStatus = &executionStatus;

    // -------------------------------------------------------------------------------------------------------------
    // vBlankIntr() will check for more than 32K instructions in a frame and issue the STOP bit in ExecutionStatus
    // -------------------------------------------------------------------------------------------------------------
    while (!executionStatus)
    {
      // Get the next 6502 instruction - do this the fast way!
      ++gSystemCycles;
      uInt8 operand = fast_cart_buffer[PC & f8_bankbit];
      PC++;

      // 6502 instruction emulation is generated by an M4 macro file
      switch (operand)
      {
        // A trick of the light... here we map peek/poke to the "F8" cart versions. This improves speed for non-bank-switched carts.
        #define peek     peek_F8
        #define peek_zpg peek_F8
        #define peek_PC  peek_PCF8
        #define poke     poke_F8
        #include "M6502Low.ins"
        #undef peek
        #undef peek_zpg
        #undef peek_PC
        #undef poke
      }
    }
    gPC = PC;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void M6502Low::execute_F8SC(void)
{
    uInt16 operandAddress;
    uInt32 PC = gPC;  // Move PC local so compiler can optimize/registerize

    // Clear all of the execution status bits
    int executionStatus=0;
    stack_executionStatus = &executionStatus;

    // -------------------------------------------------------------------------------------------------------------
    // vBlankIntr() will check for more than 32K instructions in a frame and issue the STOP bit in ExecutionStatus
    // -------------------------------------------------------------------------------------------------------------
    while (!executionStatus)
    {
      // Get the next 6502 instruction - do this the fast way!
      ++gSystemCycles;
      uInt8 operand = fast_cart_buffer[PC & f8_bankbit];
      PC++;

      // 6502 instruction emulation is generated by an M4 macro file
      switch (operand)
      {
        #define peek_PC    peek_PCF8
        #define peek_zpg   peek_F8
        #include "M6502Low.ins"
        #undef  peek_zpg
        #undef peek_PC
      }
    }
    gPC = PC;
}

// -------------------------------------------------------------------------------
// Special F6 driver for much faster speeds but comes at a cost of
// compatibility - so in cart.cpp we enable this only for the
// well-behaved carts that can support it...
// -------------------------------------------------------------------------------
inline uInt8 peek_PCF6(uInt16 address)
{
  gSystemCycles++;
  return cart_buffer[myCurrentOffset | (address & 0xFFF)];
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
inline uInt8 peek_PCF6SC(uInt16 address)
{
  gSystemCycles++;
  return cart_buffer[myCurrentOffset | (address & 0xFFF)];
}

inline uInt8 peek_F6(uInt16 address)
{
  gSystemCycles++;

  if (address & 0x1000)
  {
      address &= 0xFFF;
      if (address >= 0xFF6)
      {
          if      (address == 0x0FF6) myCurrentOffset = 0x0000;
          else if (address == 0x0FF7) myCurrentOffset = 0x1000;
          else if (address == 0x0FF8) myCurrentOffset = 0x2000;
          else if (address == 0x0FF9) myCurrentOffset = 0x3000;
      }
      return cart_buffer[myCurrentOffset | address];
  }
  else
  {
      // Note: this is not perfectly accurate to mimic the real Atari 2600 incomplete decoding address to
      // provide a true representation of lower memory mirrors. But it's good enough for well-behaved carts.
      if (address & 0x200) return theM6532.peek(address);
      else if (address & 0x80) return myRAM[address & 0x7F];
      else return theTIA.peek(address);
  }
}


inline void poke_F6(uInt16 address, uInt8 value)
{
  gSystemCycles++;

  if (address & 0x1000)
  {
      address &= 0xFFF;
      if      (address == 0x0FF6) myCurrentOffset = 0x0000;
      else if (address == 0x0FF7) myCurrentOffset = 0x1000;
      else if (address == 0x0FF8) myCurrentOffset = 0x2000;
      else if (address == 0x0FF9) myCurrentOffset = 0x3000;
  }
  else
  {
      // Note: this is not perfectly accurate to mimic the real Atari 2600 incomplete decoding address to
      // provide a true representation of lower memory mirrors. But it's good enough for well-behaved carts.
      if (address & 0x200) theM6532.poke(address, value);
      else if (address & 0x80) myRAM[address & 0x7F] = value;
      else theTIA.poke(address, value);
  }
}


void M6502Low::execute_F6(void)
{
    uInt16 operandAddress;
    uInt8 operand;
    uInt32 PC = gPC;  // Move PC local so compiler can optimize/registerize

    // Clear all of the execution status bits
    int executionStatus=0;
    stack_executionStatus = &executionStatus;

    // -------------------------------------------------------------------------------------------------------------
    // vBlankIntr() will check for more than 32K instructions in a frame and issue the STOP bit in ExecutionStatus
    // -------------------------------------------------------------------------------------------------------------
    while (!executionStatus)
    {
      // Get the next 6502 instruction - do this the fast way unless we're in a possible hotspot situation
      if (PC & 0x800)
      {
          operand = peek_F6(PC++);
      }
      else
      {
          gSystemCycles++;
          operand = cart_buffer[myCurrentOffset | (PC++ & 0xFFF)];
      }

      // 6502 instruction emulation is generated by an M4 macro file
      switch (operand)
      {
        // A trick of the light... here we map peek/poke to the "F6" cart versions. This improves speed for non-bank-switched carts.
        #define peek     peek_F6
        #define peek_zpg peek_F6
        #define peek_PC  peek_PCF6
        #define poke     poke_F6
        #include "M6502Low.ins"
        #undef peek
        #undef peek_zpg
        #undef peek_PC
        #undef poke
      }
    }
    gPC = PC;
}



// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void M6502Low::execute_F6SC(void)
{
    uInt16 operandAddress;
    uInt32 PC = gPC;  // Move PC local so compiler can optimize/registerize

    // Clear all of the execution status bits
    int executionStatus=0;
    stack_executionStatus = &executionStatus;

    // -------------------------------------------------------------------------------------------------------------
    // vBlankIntr() will check for more than 32K instructions in a frame and issue the STOP bit in ExecutionStatus
    // -------------------------------------------------------------------------------------------------------------
    while (!executionStatus)
    {
      uInt8 operand;
      // Get the next 6502 instruction - do this the fast way unless we're in a possible hotspot situation
      if (PC & 0x800)
      {
          operand = peek_F6(PC++);
      }
      else
      {
          gSystemCycles++;
          operand = cart_buffer[myCurrentOffset | (PC++ & 0xFFF)];
      }

      // 6502 instruction emulation is generated by an M4 macro file
      switch (operand)
      {
        #define peek_PC    peek_PCF6SC
        #define peek_zpg   peek_F6
        #include "M6502Low.ins"
        #undef  peek_zpg
        #undef peek_PC
      }
    }
    gPC = PC;
}


// ---------------------------------------------------------------------------
// Stubs for execute_*() variants we don't ship in the Playdate MVP.
// The header still declares them as virtuals, so they must exist to satisfy
// the vtable, but cartDriver values that would invoke them are never set.
// ---------------------------------------------------------------------------
void M6502Low::execute_F4(void)          { execute(); }
void M6502Low::execute_AR(void)          { execute(); }
void M6502Low::execute_DPCP(void)        { execute(); }
void M6502Low::execute_CDFJ(void)        { execute(); }
void M6502Low::execute_CDFJPlus(void)    { execute(); }
void M6502Low::execute_CDFJPlusPlus(void){ execute(); }
void M6502Low::execute_DPC(void)         { execute(); }
void M6502Low::execute_CTY(void)         { execute(); }

#if !defined(USE_SMOLNES_CPU) && !defined(STELLA_DUAL_CPU)
// When the smolnes core isn't compiled in, this should never be dispatched;
// fall back to the accurate interpreter just in case.
void M6502Low::execute_smol(void)        { execute(); }
#endif

#ifndef STELLA_DUAL_CPU
void M6502Low::execute_compare(void)     { execute(); }
#else
// ===========================================================================
// Instruction-level A/B comparison driver (only in -DSTELLA_DUAL_CPU builds).
//
// For each instruction we first run the smol core as a non-mutating PREDICTOR
// (reads hit ROM/RAM directly, writes suppressed) to capture the registers and
// cycle count it WOULD produce, then run the real Stella instruction (which
// stays canonical -- the machine and rendering advance normally). Comparing
// the predictor's result against Stella's actual post-state pinpoints the exact
// instruction where the two cores disagree. Instructions that read a device
// (TIA/RIOT) are skipped (predict can't reproduce read side effects).
// ===========================================================================
extern "C" int smol_predict(const uInt8 pre[7], uInt8 post[7], unsigned* out_cyc);

// Divergence report, read by the host comparison harness.
extern "C" {
  int    g_cmp_diverged   = 0;
  uInt16 g_cmp_pc         = 0;
  uInt8  g_cmp_op         = 0;
  char   g_cmp_field[8]   = {0};
  uInt32 g_cmp_stella     = 0;
  uInt32 g_cmp_smol       = 0;
}

static int s_cmp_execStatus;

void M6502Low::execute_compare(void)
{
    s_cmp_execStatus = 0;
    stack_executionStatus = &s_cmp_execStatus;
    uInt32 PC = gPC;
    uInt16 operandAddress;

    while (!s_cmp_execStatus)
    {
        // --- 1. predictor: what does smol think this instruction does? ------
        uInt8 pre[7]  = { (uInt8)(PC & 0xFF), (uInt8)(PC >> 8), A, X, Y, SP, PS() };
        uInt8 post[7];
        unsigned smolcyc = 0;
        int io = smol_predict(pre, post, &smolcyc);
        uInt32 cyc0 = gSystemCycles;

        // --- 2. one real Stella instruction (generic fetch + .ins body) -----
        #define operand myDataBusState
        ++gSystemCycles;
        PageAccess& access = myPageAccessTable[(PC & MY_ADDR_MASK) >> MY_PAGE_SHIFT];
        if (access.directPeekBase != 0) operand = *(access.directPeekBase + (PC & MY_PAGE_MASK));
        else operand = access.device->peek(PC);
        uInt8 thisop = operand;
        PC++;
        switch (operand)
        {
          #include "M6502Low.ins"
        }
        #undef operand
        uInt32 stellacyc = gSystemCycles - cyc0;

        // --- 3. compare (skip device-touching instrs and after first diverge)
        if (!io && !g_cmp_diverged)
        {
            uInt8 sp_[7] = { (uInt8)(PC & 0xFF), (uInt8)(PC >> 8), A, X, Y, SP, PS() };
            static const char* nm[7] = {"PClo","PChi","A","X","Y","SP","P"};
            for (int r = 0; r < 7; ++r)
            {
                // P: compare only real flag bits (mask out B=0x10 / unused=0x20).
                uInt8 m = (r == 6) ? 0xCF : 0xFF;
                if ((post[r] & m) != (sp_[r] & m))
                {
                    g_cmp_diverged = 1; g_cmp_pc = pre[0] | (pre[1] << 8); g_cmp_op = thisop;
                    for (int k = 0; k < 7; ++k) g_cmp_field[k] = nm[r][k] ? nm[r][k] : 0;
                    g_cmp_field[7] = 0;
                    g_cmp_stella = sp_[r] & m; g_cmp_smol = post[r] & m;
                    break;
                }
            }
            if (!g_cmp_diverged && smolcyc != stellacyc)
            {
                g_cmp_diverged = 1; g_cmp_pc = pre[0] | (pre[1] << 8); g_cmp_op = thisop;
                const char* c = "CYC";
                for (int k = 0; k < 4; ++k) g_cmp_field[k] = c[k];
                g_cmp_stella = stellacyc; g_cmp_smol = smolcyc;
            }
        }
    }
    gPC = PC;
}
#endif
