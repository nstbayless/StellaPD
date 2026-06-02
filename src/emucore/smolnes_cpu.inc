// Custom CPU implementation. This file is #included directly into nes.c so it
// shares the translation unit (the nes_state_t struct, the field-access
// macros, mem()/read_pc()/set_nz() helpers, and the cpu_step_smolnes
// reference are all in scope).
//
// On the simulator (debug builds), nes_run_until_frame() runs this step
// alongside cpu_step_smolnes on a snapshot of the state and memcmps the
// results after every instruction — any divergence raises a Playdate error.
//
// Cycle deposits mirror smolnes's bookkeeping byte-for-byte so the validation
// memcmp stays green even though the cycle counts are NOT real 6502 values
// (smolnes is approximate, ~2 cycles low for many modes). The modeflags_table
// below is built around smolnes-matching base cycles; real-6502 cycles are
// noted in the comments for each row if you ever want to retune.

#include <stdbool.h>

// ----- 32-bit bit reversal (used to fill the rare-opcode bitmap at init) ----
static uint32_t bit_reverse32(uint32_t x) {
    x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
    x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
    x = ((x >> 4) & 0x0f0f0f0fu) | ((x & 0x0f0f0f0fu) << 4);
    x = ((x >> 8) & 0x00ff00ffu) | ((x & 0x00ff00ffu) << 8);
    return (x >> 16) | (x << 16);
}

#define BITZ32(a, b, c, d) bit_reverse32(0b##a##b##c##d)

// Each uint32 covers 32 opcodes; bit (op & 0x1F) set means the opcode is
// "rare" / unsupported by the table-driven dispatcher and is delegated to
// cpu_step_smolnes. Marked opcodes here are the standard set of NES-unused
// illegal opcodes. Filled in at init time so BITZ32 doesn't need to be a
// constant expression.
static uint32_t rare_instructions_table[8];

// Flat per-opcode bool table, derived from rare_instructions_table at init.
// Hot path uses this (1 byte load + cbnz) instead of the bit-packed table
// (shift + LUT load + mask + shift + and + branch).
// Aligned so the loaded byte sits on a clean offset.
static uint8_t rare_opcode_byte_table[256] __attribute__((aligned(4)));

// Pre-decoded (opcat, mode) per opcode. Populated by cpu_custom_init from the
// original branchy decode logic (carve-outs for cc=00/cc=10 irregulars +
// regular aaa-bbb-cc + LDX/STX index fixups). Per-instruction cost goes from
// ~10-20 cycles of mask-and-compare to two byte loads.
static uint8_t opcat_table[256] __attribute__((aligned(4)));
static uint8_t mode_table[256]  __attribute__((aligned(4)));

// Per-opcode mf (modeflags) preview, folding the mode_table -> modeflags_table
// chain into a single load. Used by the hot path; mode_table itself stays for
// init bookkeeping. Saves ~2-3 cycles per instruction in cpu_step_custom.
static uint16_t mf_per_op_table[256] __attribute__((aligned(4)));

// Single combined LUT for the hot decode path: ONE 32-bit load per
// instruction in cpu_step_custom yields mf (modeflags) + opcat_raw +
// reads_operand bit + rare-opcode flag. Replaces 4 separate small-LUT
// loads. Layout (matches the field accesses in cpu_step_custom):
//   bits  0..15 : mf (modeflags, same encoding as modeflags_table)
//   bits 16..22 : opcat (0..36)
//   bit  23     : 1 if this opcat does NOT read its operand (STA/STX/STY/JMP)
//   bit  24     : 1 if this opcode is rare/illegal -- bail to cpu_step_smolnes
//   bits 25..31 : reserved (0)
// Populated by cpu_custom_init from the three smaller tables above.
// Anchored into the .bss.stellapd_hot region by link_map.ld so the
// table's D-cache set residue doesn't drift when unrelated BSS grows.
static uint32_t opdecode_table[256]
    __attribute__((aligned(4)))
    __attribute__((section(".bss.stellapd_hot.opdecode_table")));
#define OPDEC_MF(x)        ((uint16_t)((x) & 0xFFFFu))
#define OPDEC_OPCAT(x)     ((uint8_t)(((x) >> 16) & 0x7Fu))
#define OPDEC_SKIP_READ(x) (((x) >> 23) & 1u)
#define OPDEC_RARE(x)      (((x) >> 24) & 1u)

#define OPCAT_ORA 1
#define OPCAT_ASL 2
#define OPCAT_BIT 4
#define OPCAT_AND 5
#define OPCAT_ROL 6
#define OPCAT_JMP 8
#define OPCAT_EOR 9
#define OPCAT_LSR 10
#define OPCAT_JPI 12
#define OPCAT_ADC 13
#define OPCAT_ROR 14
#define OPCAT_STY 16
#define OPCAT_STA 17
#define OPCAT_STX 18
#define OPCAT_LDY 20
#define OPCAT_LDA 21
#define OPCAT_LDX 22
#define OPCAT_CPY 24
#define OPCAT_CMP 25
#define OPCAT_DEC 26
#define OPCAT_CPX 28
#define OPCAT_SBC 29
#define OPCAT_INC 30

#define OPCAT_CFLOW 32
#define OPCAT_BRANCH 33
#define OPCAT_STACK 34
#define OPCAT_FLAGOP 35
#define OPCAT_REGOP 36

#define MODE_IMM_n0 0
#define MODE_X_IND 1
#define MODE_ZPG_n0 2
#define MODE_ZPG_n1 3
#define MODE_A 4
#define MODE_IMM_n1 5
#define MODE_ABS_n0 6
#define MODE_ABS_n1 7
#define MODE_NONE  8
#define MODE_IND_Y  9
#define MODE_ZPG_X_n0  10
#define MODE_ZPG_X_n1  11
#define MODE_ZPG_Y     12
#define MODE_ABS_Y     13
#define MODE_ABS_X_n0  14
#define MODE_ABS_X_n1  15

#define MF_CATEGORY_BITS 7
#define MFC_NONE 0
#define MFC_A    1
#define MFC_IMM  2
#define MFC_ZPG  3
#define MFC_ABS  4
#define MFC_IND  5

#define MF_USE_X 8
#define MF_USE_Y 16

#define MF_CYCLES_SHIFT 10
#define CYCLES(x) ((x) << MF_CYCLES_SHIFT)

// Mode table: cycle base + MFC + USE_X/USE_Y flags. The CYCLES() values here
// match smolnes's bookkeeping; opcode-specific cycle deltas (BRK +1, PHP +1,
// PLP/PLA +2, branch taken +1/+2, JMP -1, JMP-ind +1, RMW writeback +2, plus
// page-cross / STA-on-indexed-abs / RMW-abs,X bumps) are added in the opcat
// switch and address resolver below.
//                                                  // real 6502 / smolnes
static const uint16_t modeflags_table[16] = {
    CYCLES(0) | MFC_IMM,                            // 0  IMM_n0     2 / 0 (LDY/CPY/CPX# via case-0 goto nomemop; BRK/JSR/RTI/RTS add +4 in OPCAT_CFLOW)
    CYCLES(6) | MFC_IND | MF_USE_X,                 // 1  X-IND      6 / 6
    CYCLES(1) | MFC_ZPG,                            // 2  ZPG_n0     3 / 1
    CYCLES(1) | MFC_ZPG,                            // 3  ZPG_n1     3 / 1
    CYCLES(0) | MFC_A,                              // 4  A          2 / 0
    CYCLES(0) | MFC_IMM,                            // 5  IMM_n1     2 / 0
    CYCLES(2) | MFC_ABS,                            // 6  ABS_n0     4 / 2
    CYCLES(2) | MFC_ABS,                            // 7  ABS_n1     4 / 2
    CYCLES(0) | MFC_NONE,                           // 8  NONE       2 / 0
    CYCLES(3) | MFC_IND | MF_USE_Y,                 // 9  IND_Y      5 / 3+cross
    CYCLES(2) | MFC_ZPG | MF_USE_X,                 // 10 ZPG_X_n0   4 / 2
    CYCLES(2) | MFC_ZPG | MF_USE_X,                 // 11 ZPG_X_n1   4 / 2
    CYCLES(2) | MFC_ZPG | MF_USE_Y,                 // 12 ZPG_Y      4 / 2
    CYCLES(2) | MFC_ABS | MF_USE_Y,                 // 13 ABS_Y      4 / 2+cross
    CYCLES(2) | MFC_ABS | MF_USE_X,                 // 14 ABS_X_n0   4 / 2+adj
    CYCLES(2) | MFC_ABS | MF_USE_X,                 // 15 ABS_X_n1   4 / 2+adj
};

// One-time init for the custom CPU. Called from nes_init.
static void cpu_custom_init(void) {
    rare_instructions_table[0] = BITZ32(00111001, 00011001, 00111001, 00111001);
    rare_instructions_table[1] = BITZ32(00110001, 00010001, 00111001, 00111001);
    rare_instructions_table[2] = BITZ32(00111001, 00010001, 00111001, 00111001);
    rare_instructions_table[3] = BITZ32(00111001, 00010001, 00111001, 00111001);
    rare_instructions_table[4] = BITZ32(10110001, 01010001, 00110001, 00011011);
    rare_instructions_table[5] = BITZ32(00010001, 00010001, 00110001, 00010001);
    rare_instructions_table[6] = BITZ32(00110001, 00010001, 00110001, 00010001);
    rare_instructions_table[7] = BITZ32(00110001, 00010001, 00111001, 00111001);

    // Expand the bit-packed table into a 256-byte bool LUT for the hot path.
    for (int i = 0; i < 256; ++i) {
        rare_opcode_byte_table[i] =
            (rare_instructions_table[i >> 5] & (1u << (i & 0x1F))) ? 1 : 0;
    }

    // Precompute opcat/mode per opcode by mirroring the branchy decode that
    // used to run per-instruction in cpu_step_custom().
    for (int op = 0; op < 256; ++op) {
        uint8_t opcode = (uint8_t)op;
        uint8_t mode = MODE_NONE;
        uint8_t opcat = 0;

        // Carve-outs for irregular cc=00 / cc=10 opcodes.
        if ((opcode & 0x07) == 0 || (opcode & 0x07) == 2) {
            if      ((opcode & 0x9F) == 0x00) opcat = OPCAT_CFLOW;   // BRK/JSR/RTI/RTS
            else if ((opcode & 0x1F) == 0x10) { opcat = OPCAT_BRANCH; mode = MODE_IMM_n0; }
            else if ((opcode & 0x9F) == 0x08) opcat = OPCAT_STACK;   // PHP/PLP/PHA/PLA
            else if ((opcode & 0x9D) == 0x18) opcat = OPCAT_FLAGOP;  // CLC/SEC/CLI/SEI
            else if ((opcode & 0x8D) == 0x88) opcat = OPCAT_REGOP;   // DEY/TYA/TAY/CLV/INY/CLD/INX/SED/TXA/TXS/TAX/TSX/DEX/NOP
            // (LDX-imm 0xA2 falls through to the regular dispatch below.)
        }

        // Regular aaa-bbb-cc decode for the rest.
        if (!opcat) {
            opcat = (uint8_t)(((opcode & 0xE0) >> 3) | (opcode & 0x03));
            mode  = (uint8_t)((opcode & 1) | ((opcode & 0x1C) >> 1));

            if (opcat == OPCAT_LDX || opcat == OPCAT_STX) {
                if      ((mode & ~1) == MODE_ZPG_X_n0) mode = MODE_ZPG_Y;
                else if ((mode & ~1) == MODE_ABS_X_n0) mode = MODE_ABS_Y;
            }
            if (opcat == OPCAT_LDX && mode == MODE_IMM_n0) mode = MODE_IMM_n1;
        }

        // Encode "this opcat does not read its operand" in bit 7 of the
        // opcat slot. Hot-path replacement for the 4-compare expression
        //   (opcat != STA && opcat != STX && opcat != STY && opcat != JMP)
        // The opcat values themselves only use bits 0-5, so bit 7 is free.
        if (opcat == OPCAT_STA || opcat == OPCAT_STX ||
            opcat == OPCAT_STY || opcat == OPCAT_JMP)
            opcat |= 0x80;

        opcat_table[opcode]      = opcat;
        mode_table[opcode]       = mode;
        uint16_t mf = modeflags_table[mode];
        mf_per_op_table[opcode]  = mf;

        // Pack everything the cpu_step_custom hot path needs from a single
        // 32-bit slot. Note opcat already has bit 7 set for skip-read; we
        // shift it into bit 23 of the combined word.
        uint32_t skip_read = (opcat & 0x80) ? 1u : 0u;
        uint32_t rare      = rare_opcode_byte_table[opcode] ? 1u : 0u;
        opdecode_table[opcode] =
            (uint32_t)mf
          | ((uint32_t)(opcat & 0x7F) << 16)
          | (skip_read << 23)
          | (rare << 24);
    }
}

// Helper: set N/Z from a value (matches smolnes's set_nz). Force-inlined so
// the ITCM-relocated execute_smol body contains no external direct BL to
// this helper (DTCM is far out of BL range from flash on STM32H7).
static inline __attribute__((always_inline)) void cpu_set_nz(uint8_t v) {
    $N = (v >> 7) & 1;
    $Z = (v == 0);
}

__attribute__((section(".text.stellapd_hot.cpu_step_custom")))
// Per-instruction extras (taken-branch / page-cross penalty). Local to cpu_step
// (returned to the wrapper) so gcc can register-allocate it -- the old globals
// smol_taken / smol_rcross cost a write per instruction.
#define smol_taken  extras
#define smol_rcross extras

static unsigned cpu_step_custom(void) {
    unsigned extras = 0;
    unsigned cyc = 0;

    // ----- NMI/IRQ -----------------------------------------------------------
    // Matches smolnes's case-0 path for the NMI/IRQ branch.
    // On the 6507 (Atari 2600) NMI is not pinned out and IRQ is tied off, so
    // this is *always* false in practice -- but the branch still has to be
    // checked every instruction. unlikely() lets the compiler shove the
    // interrupt-vector code out of the hot fall-through.
    if (unlikely(nmi_irq)) {
        PUSH($PCH);
        PUSH($PCL);
        PUSH($P | 0x20);
        uint8_t veclo = (uint8_t)(0xFE - (nmi_irq & 4));
        $PCL = mem(veclo, 0xFF, 0, 0);
        $PCH = mem((uint8_t)(veclo + 1), 0xFF, 0, 0);
        nmi_irq = 0;
        return 5;
    }

    uint8_t opcode = FETCH8();
    // No s_last_opcode global -- opcode is packed into the high byte of the
    // return value (extras in the low byte). Wrapper extracts both with one
    // shift each, avoiding a global store + load.

    // Single 32-bit load yields rare-flag, opcat, skip-read bit, and mf
    // (modeflags). Replaces 4 separate small-LUT loads.
    uint32_t dec = opdecode_table[opcode];

    // ----- Rare-opcode bail-out (illegal / unsupported) ----------------------
    if (unlikely(OPDEC_RARE(dec))) {
        if ($PCL-- == 0) $PCH--;   // smolnes refetches the opcode byte
        return cpu_step_smolnes();
    }
    uint8_t opcat = OPDEC_OPCAT(dec);

    // ----- Resolve operand from mode ----------------------------------------
    uint16_t mf      = OPDEC_MF(dec);
    cyc += (mf >> MF_CYCLES_SHIFT);
    uint8_t  mfc     = mf & MF_CATEGORY_BITS;
    bool     use_x   = !!(mf & MF_USE_X);
    bool     use_y   = !!(mf & MF_USE_Y);
    uint8_t  operand = 0;
    uint16_t addr    = 0;
    int      cross   = 0;
    // Bit 23 of dec is "skip operand read" (STA/STX/STY/JMP).
    bool     reads_operand = !OPDEC_SKIP_READ(dec);

    switch (mfc) {
    case MFC_NONE:
        break;
    case MFC_A:
        operand = $A;
        break;
    case MFC_IMM:
        operand = FETCH8();
        break;
    case MFC_ZPG: {
        uint8_t a = FETCH8();
        if (use_x) a = (uint8_t)(a + $X);
        if (use_y) a = (uint8_t)(a + $Y);
        addr = a;
        if (reads_operand) operand = mem(a, 0, 0, 0);
        break;
    }
    case MFC_ABS: {
        uint8_t lo  = FETCH8();
        uint8_t hi  = FETCH8();
        uint8_t idx = use_x ? $X : (use_y ? $Y : 0);
        cross = (lo + idx) > 0xFF;
        addr = (uint16_t)((((uint16_t)hi << 8) | lo) + idx);
        if (use_x || use_y) {
            int sta_grp   = (opcode & 0xE0) == 0x80;
            int rmw_abs_e = ((opcode & 0x0F) == 0x0E) && (opcode != 0xBE);
            cyc += (unsigned)((sta_grp | rmw_abs_e) | cross);
#ifdef SMOL_CYCLE_EXACT
            // Extra cycle on a read that crosses a page boundary (abs,X/abs,Y).
            if (unlikely(reads_operand && cross)) smol_rcross = 1;
#endif
        }
        if (reads_operand) operand = mem((uint8_t)addr, (uint8_t)(addr >> 8), 0, 0);
        break;
    }
    case MFC_IND: {
        uint8_t za = FETCH8();
        if (use_x) za = (uint8_t)(za + $X);
        uint8_t lo = mem(za,                 0, 0, 0);
        uint8_t hi = mem((uint8_t)(za + 1),  0, 0, 0);
        if (use_y) {
            cross = (lo + $Y) > 0xFF;
            addr = (uint16_t)((((uint16_t)hi << 8) | lo) + $Y);
            int sta_grp = (opcode & 0xE0) == 0x80;
            cyc += (unsigned)(sta_grp | cross);
#ifdef SMOL_CYCLE_EXACT
            if (unlikely(reads_operand && cross)) smol_rcross = 1;   // (ind),Y read crossing a page
#endif
        } else {
            addr = ((uint16_t)hi << 8) | lo;
        }
        if (reads_operand) operand = mem((uint8_t)addr, (uint8_t)(addr >> 8), 0, 0);
        break;
    }
    }

    // ----- Execute by opcat -------------------------------------------------
    bool writeback = false;

    switch (opcat) {
    case OPCAT_ORA: $A |= operand; cpu_set_nz($A); break;
    case OPCAT_AND: $A &= operand; cpu_set_nz($A); break;
    case OPCAT_EOR: $A ^= operand; cpu_set_nz($A); break;

    case OPCAT_ADC: {
        uint16_t sum = (uint16_t)$A + operand + $C;
        $V = (($A ^ sum) & (operand ^ sum) & 0x80) ? 1 : 0;
#ifdef SMOLNES_BCD
        // 6507 decimal mode (Atari uses it for scores). Match Stella's ADC-BCD
        // exactly: Z from the binary result, N from the BCD result, C if the
        // decimal sum exceeds 99, V from the BCD sign quirk.
        // Decimal mode is set with SED only around score math -- rare relative
        // to total ADC/SBC count -- so the branch is unlikely().
        if (unlikely($D)) {
            uint8_t oldA = $A;
            int dsum = smol_bcd0[$A] + smol_bcd0[operand] + $C;
            $Z = ((uint8_t)sum == 0);
            $C = (dsum > 99) ? 1 : 0;
            $A = smol_bcd1[dsum & 0xFF];
            $N = ($A >> 7) & 1;
            $V = ((((oldA ^ $A) & 0x80) && (($A ^ operand) & 0x80))) ? 1 : 0;
            break;
        }
#endif
        $C = (sum > 0xFF) ? 1 : 0;
        $A = (uint8_t)sum;
        cpu_set_nz($A);
        break;
    }
    case OPCAT_SBC: {
        uint8_t inv = (uint8_t)~operand;
        uint16_t sum = (uint16_t)$A + inv + $C;
        $V = (($A ^ sum) & (inv ^ sum) & 0x80) ? 1 : 0;
#ifdef SMOLNES_BCD
        // 6507 decimal-mode SBC, matching Stella's SBC-BCD exactly (uses the
        // *original* operand, not the inverted one; Z/N from the BCD result).
        if (unlikely($D)) {
            uint8_t oldA = $A;
            uint8_t oldC = $C;
            int diff = smol_bcd0[$A] - smol_bcd0[operand] - (oldC ? 0 : 1);
            if (diff < 0) diff += 100;
            $A = smol_bcd1[diff & 0xFF];
            $Z = ($A == 0);
            $N = ($A >> 7) & 1;
            $C = (oldA >= (uint8_t)(operand + (oldC ? 0 : 1))) ? 1 : 0;
            $V = ((((oldA ^ $A) & 0x80) && (($A ^ operand) & 0x80))) ? 1 : 0;
            break;
        }
#endif
        $C = (sum > 0xFF) ? 1 : 0;
        $A = (uint8_t)sum;
        cpu_set_nz($A);
        break;
    }

    case OPCAT_ASL:
        $C = (operand >> 7) & 1;
        operand = (uint8_t)(operand << 1);
        cpu_set_nz(operand);
        writeback = true;
        break;
    case OPCAT_ROL: {
        uint8_t c = $C;
        $C = (operand >> 7) & 1;
        operand = (uint8_t)((operand << 1) | c);
        cpu_set_nz(operand);
        writeback = true;
        break;
    }
    case OPCAT_LSR:
        $C = operand & 1;
        operand >>= 1;
        cpu_set_nz(operand);
        writeback = true;
        break;
    case OPCAT_ROR: {
        uint8_t c = $C;
        $C = operand & 1;
        operand = (uint8_t)((operand >> 1) | (c << 7));
        cpu_set_nz(operand);
        writeback = true;
        break;
    }
    case OPCAT_INC:
        operand = (uint8_t)(operand + 1);
        cpu_set_nz(operand);
        writeback = true;
        break;
    case OPCAT_DEC:
        operand = (uint8_t)(operand - 1);
        cpu_set_nz(operand);
        writeback = true;
        break;

    case OPCAT_BIT:
        $N = (operand >> 7) & 1;
        $V = (operand >> 6) & 1;
        $Z = (($A & operand) == 0);
        break;

    case OPCAT_LDA: $A = operand; cpu_set_nz($A); break;
    case OPCAT_LDX: $X = operand; cpu_set_nz($X); break;
    case OPCAT_LDY: $Y = operand; cpu_set_nz($Y); break;

    case OPCAT_STA: operand = $A; writeback = true; break;
    case OPCAT_STX: operand = $X; writeback = true; break;
    case OPCAT_STY: operand = $Y; writeback = true; break;

    case OPCAT_CMP: $C = ($A >= operand) ? 1 : 0; cpu_set_nz((uint8_t)($A - operand)); break;
    case OPCAT_CPX: $C = ($X >= operand) ? 1 : 0; cpu_set_nz((uint8_t)($X - operand)); break;
    case OPCAT_CPY: $C = ($Y >= operand) ? 1 : 0; cpu_set_nz((uint8_t)($Y - operand)); break;

    case OPCAT_JMP:
        $PCL = (uint8_t)addr;
        $PCH = (uint8_t)(addr >> 8);
        cyc -= 1;                        // smolnes: cycles-- on JMP
        break;

    case OPCAT_JPI: {                    // JMP (indirect)
        uint8_t addr_lo = (uint8_t)addr;
        uint8_t addr_hi = (uint8_t)(addr >> 8);
        $PCL = operand;
        $PCH = mem((uint8_t)(addr_lo + 1), addr_hi, 0, 0);
        cyc += 1;
        break;
    }

    case OPCAT_BRANCH: {
        static const uint8_t bmask[4] = { 0x80, 0x40, 0x01, 0x02 };
        uint8_t take = (uint8_t)((!($P & bmask[opcode >> 6])) ^ ((opcode >> 5) & 1));
        if (take) {
            int sum = (int)$PCL + (int8_t)operand;
            int xc  = sum >> 8;
            $PCH = (uint8_t)($PCH + xc);
            $PCL = (uint8_t)sum;
            cyc += xc ? 2u : 1u;
#ifdef SMOL_CYCLE_EXACT
            smol_taken = xc ? 2 : 1;   // +1 taken, +1 more if page crossed
#endif
        }
        break;
    }

    case OPCAT_CFLOW: {                  // BRK / JSR / RTI / RTS
        // mode is NONE → cyc starts at 0; add 4 here to match smolnes's
        // case-0 tail, then per-opcode deltas.
        cyc += 4;
        switch (opcode >> 5) {
        case 0:                          // BRK
            if (++$PCL == 0) $PCH++;
            PUSH($PCH);
            PUSH($PCL);
            PUSH($P | 0x20);
            $PCL = mem(0xFE, 0xFF, 0, 0);
            $PCH = mem(0xFF, 0xFF, 0, 0);
            cyc += 1;
            break;
        case 1: {                        // JSR
            uint8_t lo = FETCH8();
            PUSH($PCH);
            PUSH($PCL);
            $PCH = FETCH8();
            $PCL = lo;
            break;
        }
        case 2:                          // RTI
            // Smolnes clears bit 5 (Unused) on the pulled P, not bit 4 (B).
            // It's a quirk but we match it for the validation memcmp.
            $P   = PULL & ~0x20;
            $PCL = PULL;
            $PCH = PULL;
            break;
        case 3:                          // RTS
            $PCL = PULL;
            $PCH = PULL;
            if (++$PCL == 0) $PCH++;
            break;
        }
        break;
    }

    case OPCAT_STACK: {                  // PHP / PLP / PHA / PLA
        switch (opcode >> 5) {
        case 0: PUSH($P | 0x30);                       cyc += 1; break; // PHP
        case 1: $P = PULL & ~0x10;                     cyc += 2; break; // PLP
        case 2: PUSH($A);                              cyc += 1; break; // PHA
        case 3: $A = PULL; cpu_set_nz($A);             cyc += 2; break; // PLA
        }
        break;
    }

    case OPCAT_FLAGOP:                   // CLC / SEC / CLI / SEI
    case OPCAT_REGOP: {                  // DEY/TYA/TAY/CLV/INY/CLD/INX/SED/TXA/TXS/TAX/TSX/DEX/NOP
        // The implied-mode instructions split between two opcat values, but
        // smolnes treats them as one block — keep them together here too.
        switch (opcode) {
        case 0x88: $Y--;     cpu_set_nz($Y);          break; // DEY
        case 0x98: $A = $Y;  cpu_set_nz($A);          break; // TYA
        case 0xA8: $Y = $A;  cpu_set_nz($Y);          break; // TAY
        case 0xC8: $Y++;     cpu_set_nz($Y);          break; // INY
        case 0xE8: $X++;     cpu_set_nz($X);          break; // INX
        case 0x8A: $A = $X;  cpu_set_nz($A);          break; // TXA
        case 0x9A: $S = $X;                           break; // TXS
        case 0xAA: $X = $A;  cpu_set_nz($X);          break; // TAX
        case 0xBA: $X = $S;  cpu_set_nz($X);          break; // TSX
        case 0xCA: $X--;     cpu_set_nz($X);          break; // DEX
        case 0xEA:                                    break; // NOP
        // CLC/SEC/CLI/SEI + CLV/CLD/SED: mirror smolnes's table-driven
        // P = P & ~mask[opcode>>4 + 3] | mask[opcode>>4 + 4].
        default: {
            static const uint8_t fmask[20] = {
                128, 64, 1, 2, 1, 0, 0, 1, 4, 0,
                  0, 4, 0, 0, 64, 0, 8, 0, 0, 8
            };
            uint8_t sw = (uint8_t)(opcode >> 4);
            $P = (uint8_t)(($P & ~fmask[sw + 3]) | fmask[sw + 4]);
            break;
        }
        }
        break;
    }
    }

    // ----- Writeback (RMW result / store) -----------------------------------
    // Smolnes adds +2 cycles for an RMW writeback (ASL/ROL/LSR/ROR/INC/DEC
    // touching memory) via its memop: label; the STA/STX/STY default branch
    // just calls mem(..., 1) with no cycle delta. Mirror that distinction.
    if (writeback) {
        if (mfc == MFC_A) {
            $A = operand;
        } else {
            bool is_store = (opcat == OPCAT_STA ||
                             opcat == OPCAT_STX ||
                             opcat == OPCAT_STY);
            if (!is_store) cyc += 2;
            mem((uint8_t)addr, (uint8_t)(addr >> 8), operand, 1);
        }
    }

    (void)cyc;   // vestigial; gcc DCEs all 10+ cyc updates since the result
                 // is unused. The wrapper measures elapsed cycles via
                 // gSystemCycles, not via this return.
    // Pack opcode (high byte) and extras (low byte) into the return value:
    //   bits 0..7  -- taken-branch + page-cross penalty (was smol_taken/rcross)
    //   bits 8..15 -- the opcode we executed (used by the wrapper to look up
    //                 smol_base_cyc and apply the deficit). Single u32
    //                 return is cheaper than the old global s_last_opcode
    //                 store + reload.
    return ((unsigned)opcode << 8) | (extras & 0xFF);
}
#undef smol_taken
#undef smol_rcross
