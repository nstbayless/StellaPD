// M6502Smol.cpp — OPTIONAL tiny 6502 core, adapted from smolnes' compact
// table-driven CPU (../smolnes/playdate/src/cpu.c). Enabled with the compiler
// flag -DUSE_SMOLNES_CPU.
//
// Rationale: the Stella M6502Low interpreter compiles each cart variant into a
// ~18 KB inline opcode switch. On the cache-bound Cortex-M7 a much smaller
// dispatcher can win on instruction-cache pressure even if it does more work
// per opcode. This wires smolnes' ~330-line core to Stella's memory + timing.
//
// Coupling notes:
//   * mem() routes through Stella's page-access table -- the same accurate
//     path Stella's own generic peek/poke use -- so 2600 address mirroring,
//     per-access cycle counting (gSystemCycles), and TIA register pokes all
//     work, and the frame ends when a TIA write calls m6502().stop().
//   * Registers live in this core's own file during execution and sync with
//     Stella's globals (A/X/Y/SP + flag bytes via PS()) at entry/exit.
//
// Known gaps (acceptable for an experimental option):
//   * No decimal (BCD) mode -- smolnes targets the NES 6502. Atari games that
//     use SED/CLD around score math may show wrong digits.
//   * Illegal/undocumented opcodes are treated as NOPs.

// Compiled when the smolnes core is the only CPU (-DUSE_SMOLNES_CPU) OR when
// both cores are built for runtime A/B comparison (-DSTELLA_DUAL_CPU).
#if defined(USE_SMOLNES_CPU) || defined(STELLA_DUAL_CPU)

#include "pd_compat.h"
#include <stdint.h>
#include <stdbool.h>

#include "System.hxx"
#include "M6502.hxx"
#include "M6502Low.hxx"
#include "Cart.hxx"

extern uInt16 gPC;
extern uInt8  A, X, Y, SP;
extern int*   stack_executionStatus;

// --- this core's register file ---------------------------------------------
static uint8_t s_A, s_X, s_Y, s_S;
static uint8_t s_PCL, s_PCH;
typedef union {
    uint8_t byte;
    struct { uint8_t c:1, z:1, i:1, d:1, b:1, u:1, v:1, n:1; } bits;
} smol_p_t;
static smol_p_t s_Pun;
static uint8_t s_nmi_irq = 0;   // 6507 has no NMI; IRQ tied off -> always 0

// Predict mode (instruction-level comparison): when set, smol_mem must NOT
// mutate the machine. ONLY exists in the dual-CPU comparison build -- gating
// it under STELLA_DUAL_CPU compiles the dead if(s_predict) branch out of the
// device build, cutting ~2.3% off smol_mem and ~2.7% off smol_read_pc per
// pd-trace (the load+branch on a non-const global was unavoidable for the
// compiler otherwise).
#ifdef STELLA_DUAL_CPU
static int s_predict = 0;
static int s_predict_io = 0;
#endif

// --- memory: Stella page-access table (accurate path) -----------------------
// always_inline: under -Os gcc declines to inline these despite the `inline`
// hint, and pd-trace showed smol_mem + smol_read_pc as the #3/#5 hottest
// symbols. Inlining lets the compiler fold the `write` constant at each
// caller (dropping the read/write branch entirely on each path), and lets it
// keep the PageAccess load close to the value use across the cpu_step_custom
// instruction body.
static inline __attribute__((always_inline)) uint8_t smol_mem(uint8_t lo, uint8_t hi, uint8_t val, uint8_t write)
{
    uInt16 address = (uInt16)(((uInt16)hi << 8) | lo);
    PageAccess& acc = myPageAccessTable[(address & MY_ADDR_MASK) >> MY_PAGE_SHIFT];
#ifdef STELLA_DUAL_CPU
    if (s_predict) {
        if (write) return 0;                                  // suppress writes
        if (acc.directPeekBase) return *(acc.directPeekBase + (address & MY_PAGE_MASK));
        s_predict_io = 1;                                     // device read -> can't validate
        return 0;
    }
#endif
    gSystemCycles++;
    if (write) {
        if (acc.directPokeBase) *(acc.directPokeBase + (address & MY_PAGE_MASK)) = val;
        else                    acc.device->poke(address, val);
        return 0;
    }
    if (acc.directPeekBase) return *(acc.directPeekBase + (address & MY_PAGE_MASK));
    return acc.device->peek(address);
}

static inline __attribute__((always_inline)) uint8_t smol_read_pc(void)
{
    uint8_t v = smol_mem(s_PCL, s_PCH, 0, 0);
    if (++s_PCL == 0) s_PCH++;
    return v;
}

// Captures the opcode FETCH8'd at the start of cpu_step_custom so the wrapper
// can read it for cycle accounting WITHOUT doing a second PageAccess lookup.
// Previously the wrapper smol_peek_nocycle'd the same byte before calling
// cpu_step, costing ~10 cycles per instruction.
static uint8_t s_last_opcode;

// --- macros consumed by the included core -----------------------------------
#define $A   s_A
#define $X   s_X
#define $Y   s_Y
#define $S   s_S
#define $P   s_Pun.byte
#define $PCL s_PCL
#define $PCH s_PCH
#define $C   s_Pun.bits.c
#define $Z   s_Pun.bits.z
#define $I   s_Pun.bits.i
#define $D   s_Pun.bits.d
#define $B   s_Pun.bits.b
#define $U   s_Pun.bits.u
#define $V   s_Pun.bits.v
#define $N   s_Pun.bits.n
#define nmi_irq      s_nmi_irq
#define FETCH8()     smol_read_pc()
#define mem(lo,hi,val,write) smol_mem((uint8_t)(lo),(uint8_t)(hi),(uint8_t)(val),(uint8_t)(write))
#define PULL         mem(++s_S, 1, 0, 0)
#define PUSH(x)      mem(s_S--, 1, (x), 1)

// Fallback for rare/illegal opcodes: cpu.c rewinds PC then calls this. Treat
// as a 2-cycle NOP (consume the opcode byte).
static unsigned cpu_step_smolnes(void) { smol_read_pc(); return 2; }

// --- exact 6502 cycle accounting --------------------------------------------
// Bus accesses (mem/FETCH8) already deposit 1 gSystemCycles each, so a store's
// TIA strobe lands on the correct cycle. To match Stella's cycle counts we add
// the remaining *internal* cycles per instruction from the standard NMOS 6502
// base table, plus conditional taken-branch / read-page-cross penalties the
// core exports below. SMOL_CYCLE_EXACT enables those exports in the .inc.
#define SMOL_CYCLE_EXACT
// Per-instruction extras (taken-branch + page-cross penalty) now flow back as
// the cpu_step_custom() return value -- the wrapper folds them into the cycle
// accounting. The old smol_taken/smol_rcross globals were a write+read per
// instruction; an in-register return is free.

// BCD conversion tables matching Stella's M6502::ourBCDTable exactly, so the
// SMOLNES_BCD ADC/SBC produce identical flags/results to the Stella core.
//   bcd0: packed-BCD byte -> decimal value   ((t>>4)*10 + (t&0x0f))
//   bcd1: decimal value  -> packed-BCD byte   ((((t%100)/10)<<4) | (t%10))
static uint8_t smol_bcd0[256];
static uint8_t smol_bcd1[256];
static int smol_bcd_inited = 0;
static void smol_bcd_init(void) {
    for (int t = 0; t < 256; ++t) {
        smol_bcd0[t] = (uint8_t)(((t >> 4) * 10) + (t & 0x0f));
        smol_bcd1[t] = (uint8_t)((((t % 100) / 10) << 4) | (t % 10));
    }
    smol_bcd_inited = 1;
}

// Standard NMOS 6502 base cycle counts (page-cross / branch-taken extras are
// added separately via smol_rcross / smol_taken).
static const uint8_t smol_base_cyc[256] = {
/*0*/ 7,6,2,8,3,3,5,5,3,2,2,2,4,4,6,6,
/*1*/ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*2*/ 6,6,2,8,3,3,5,5,4,2,2,2,4,4,6,6,
/*3*/ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*4*/ 6,6,2,8,3,3,5,5,3,2,2,2,3,4,6,6,
/*5*/ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*6*/ 6,6,2,8,3,3,5,5,4,2,2,2,5,4,6,6,
/*7*/ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*8*/ 2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
/*9*/ 2,6,2,6,4,4,4,4,2,5,2,5,5,5,5,5,
/*A*/ 2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
/*B*/ 2,5,2,5,4,4,4,4,2,4,2,4,4,4,4,4,
/*C*/ 2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
/*D*/ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*E*/ 2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
/*F*/ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
};

// Pull in smolnes' table-driven core (cpu_custom_init + cpu_step_custom).
#include "smolnes_cpu.inc"

// --- entry/exit state sync ---------------------------------------------------
static int s_inited = 0;

__attribute__((section(".text.stellapd_hot.execute_smol")))
void M6502Low::execute_smol(void)
{
    if (!s_inited) { cpu_custom_init(); s_inited = 1; }
    if (!smol_bcd_inited) smol_bcd_init();

    // Load 6502 state from Stella globals.
    s_A = A; s_X = X; s_Y = Y; s_S = SP;
    s_PCL = (uint8_t)(gPC & 0xFF);
    s_PCH = (uint8_t)((gPC >> 8) & 0xFF);
    s_Pun.byte = PS();              // pack N/V/B/D/I/Z/C from Stella's flag bytes

    int executionStatus = 0;
    stack_executionStatus = &executionStatus;

    int guard = 0;
    while (!executionStatus) {
        // mem()/FETCH8 already advance gSystemCycles once per memory access, so
        // mid-instruction TIA strobes (e.g. STA RESP0) land on the correct
        // cycle. The remaining *internal* cycles (RMW dummy, implied padding,
        // taken-branch, indexed-read page-cross) are added here from the
        // standard 6502 base table so total per-instruction timing matches
        // Stella -- otherwise cumulative drift smears positioned sprites.
        uInt32 before = gSystemCycles;
        unsigned extras = cpu_step_custom();         // fetches opcode + operands, executes
        // Pulled from inside cpu_step_custom -- avoids the duplicate
        // PageAccess lookup smol_peek_nocycle used to perform.
        uint8_t op = s_last_opcode;
        uInt32 used = gSystemCycles - before;        // all bus cycles this instruction
        int total = (int)smol_base_cyc[op] + (int)extras;
        if (total > (int)used) gSystemCycles += (total - (int)used);
        if (unlikely(++guard > 2000000)) break;      // safety against a runaway frame
    }

    // Write 6502 state back to Stella globals. Leave gPC unmasked to match the
    // Stella interpreter (the 6507 masks at memory-access time, not in PC).
    A = s_A; X = s_X; Y = s_Y; SP = s_S;
    gPC = (uInt16)(((uInt16)s_PCH << 8) | s_PCL);
    PS(s_Pun.byte);                 // unpack flags back into Stella's flag bytes
}

// Instruction-level predictor for the comparison harness. Loads the supplied
// pre-state, runs exactly one instruction WITHOUT mutating the machine (predict
// mode), and reports the resulting registers + the cycle count this core would
// charge. Returns nonzero if the instruction touched a device address (TIA/
// RIOT) -- the driver skips comparing those, since predict can't reproduce
// read side effects without corrupting canonical state.
//   pre/post layout: [0]=PClo [1]=PChi [2]=A [3]=X [4]=Y [5]=SP [6]=P
#ifdef STELLA_DUAL_CPU
extern "C" int smol_predict(const uint8_t pre[7], uint8_t post[7], unsigned* out_cyc)
{
    if (!s_inited) { cpu_custom_init(); s_inited = 1; }
    if (!smol_bcd_inited) smol_bcd_init();

    s_PCL = pre[0]; s_PCH = pre[1];
    s_A = pre[2]; s_X = pre[3]; s_Y = pre[4]; s_S = pre[5];
    s_Pun.byte = pre[6];
    s_nmi_irq = 0;

    s_predict = 1; s_predict_io = 0;
    unsigned extras = cpu_step_custom();
    uint8_t op = s_last_opcode;
    s_predict = 0;

    post[0] = s_PCL; post[1] = s_PCH;
    post[2] = s_A; post[3] = s_X; post[4] = s_Y; post[5] = s_S;
    post[6] = s_Pun.byte;
    *out_cyc = (unsigned)smol_base_cyc[op] + extras;
    return s_predict_io;
}
#endif // STELLA_DUAL_CPU

#endif // USE_SMOLNES_CPU || STELLA_DUAL_CPU
