# StellaPD

An MVP **Atari 2600 (VCS) emulator for the Playdate**, ported from
[StellaDS Phoenix Edition](https://github.com/wavemotion-dave/StellaDS) by
wavemotion-dave (Dave Bernazzani), which in turn is built on the
[Stella](https://github.com/stella-emu/stella) project. The original
StellaDS authors and the Stella Team did the hard work; everything here is
a slimmed-down adaptation of theirs.

> **Status:** scaffolded port. Builds for device + simulator. Verified to
> launch in the Playdate simulator; correct rendering / input / audio for
> real games is **not yet validated**. This is an MVP scaffold to iterate
> on, not a finished emulator. See "Known limitations" below.

## What's been done

- Custom Makefile that builds the C++ emucore alongside the C entry point
  using the Playdate SDK's `common.mk`.
- A `pd_compat.h` shim that no-ops the libnds-specific decorators
  (`__attribute__((section(".dtcm")))`, `ITCM_CODE`, `BG_GFX`,
  `dmaCopyWordsAsynch`, `isDSiMode()`, …).
- A `stella_glue.cpp` that:
  - Exposes a tiny C API (`stella_init`, `stella_run_frame`,
    `stella_framebuffer`, `stella_set_input`) for `main.c` to call.
  - Defines the globals StellaDS relied on (`gAtariFrames`, `mySoundFreq`,
    framebuffer storage, `aptr`/`bptr` dummy audio sinks…).
  - Supplies `operator new`/`delete`, `__cxa_pure_virtual`, and newlib
    syscall stubs (`_exit`, `_kill`, …) so the C++ runtime links cleanly.
- A `main.c` Playdate entry point that loads `rom.a26` (or `rom.bin`),
  drives the core at 30 Hz, dithers the 160×N TIA framebuffer to the
  Playdate's 1-bit 400×240 LCD (2× horizontal scale, 4×4 Bayer dither on
  Stella palette luma), maps the D-pad and A/B → joystick, and exposes
  Reset / Select via the system menu.

## What's been deliberately dropped from StellaDS

The StellaDS source is ~35 000 lines and supports virtually every
Atari 2600 bank-switching scheme + ARM-assisted carts + a full-game MD5
database with per-title overrides. For MVP I cut all of:

- Every bank-switching scheme except **2K, 4K, F8, F8SC, F6, F6SC, FASC**.
- The MD5 / per-game database in `Cart.cpp` (replaced with a 12-line
  size-based autodetect).
- Supercharger (AR), DPC, DPC+, CDF/CDFJ/+, CTY, JANE, MB, SB, etc carts.
- The Thumbulator ARM-assisted cart emulator.
- All controllers except Joystick — no paddles, driving, keypad,
  Genesis, QuadTari, SaveKey, BoosterGrip.
- High-score persistence, save states, screenshots, intro, on-screen
  menus, screen-pan controls, frame blending modes.
- The Tia sound path that wrote into fixed DS VRAM addresses.

## Build

```bash
export PLAYDATE_SDK_PATH=/path/to/PlaydateSDK
make
```

Drop a 2K / 4K / 8K / 16K Atari 2600 ROM next to the pdxinfo as
`Source/rom.a26` (or `rom.bin`).

Build produces:

- `Source/pdex.elf` — Cortex-M7 device binary
- `Source/pdex.so`  — Linux simulator dylib
- `StellaPD.pdx/`   — the bundle for `pdc`

## Known limitations (MVP)

- **Rendering** is at the correct geometry but likely needs tuning of
  start scanline, vertical alignment, and dither parameters.
- **No audio output.** TIASound runs but writes into a dummy buffer; the
  Playdate audio system isn't connected to it yet.
- **No frame-blend / flicker reduction.** Locked to MODE_NO.
- **No save state, no high scores, no on-device file picker.** One
  fixed `rom.a26` is loaded.
- **Performance not measured.** The Dirty Optimization Secrets writeup
  suggests big wins from `-Os` + careful linker placement; we already
  build the C++ core at `-Os` but haven't tuned the linker map or
  considered ITCM relocation.

## Credits

This port stands entirely on:

- Stella — Bradford W. Mott, Stephen Anthony, and the Stella Team.
- StellaDS Phoenix Edition — Dave Bernazzani (wavemotion-dave),
  Alekmaul (original port).
- Playdate SDK & C_API — Panic Inc.
- Playdate optimization guidance: "Dirty Optimization Secrets" forum
  post (June 2025).

Any bugs in *this* port are mine, not theirs.
