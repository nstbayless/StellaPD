/* StellaPD link map -- SDK default + a stabilized prologue at the start of
 * .text that pins our hot emulator functions to fixed, 32-byte-cache-line-
 * aligned slots. Without this, every small code edit elsewhere shifts the
 * addresses of (e.g.) cpu_step_custom or TIA::poke, and landing on a bad
 * cache-line alignment can swing FPS by 10%+ on the M7.
 *
 * NOTE: pure ALIGN(32) only -- no per-function pad offsets and no ITCM
 * relocation. ITCM only helps Rev A; ALIGN works on both.
 */

ENTRY(eventHandlerShim)
GROUP(libgcc.a libc.a libm.a)

SECTIONS
{
	.text :
	{
		/* Anchor the entire hot-emulator region at a 256-byte boundary,
		   so unrelated code changes elsewhere in .text can't shift the
		   ENTIRE region around to a different cache-line residue and
		   regress FPS by a few percent each time. Inside the region,
		   place 32-byte alignment ONLY between large functions (cache-
		   line granularity); back-to-back small helpers can share a
		   line.
		   Tried ALIGN(0x1000) here previously -- catastrophic 50%
		   regression because every hot function then shared the SAME
		   low 12 bits, maxing branch-predictor aliasing. ALIGN(0x100)
		   gives stable anchoring with 256 distinct PC residues. */
		. = ALIGN(0x100);

		/* ITCM relocation region FIRST inside the hot block. Putting it
		 * at the head keeps execute_smol's address (and its I-cache set
		 * residue) stable at the same low offset that .text.stellapd_hot
		 * historically used -- otherwise relocating its section name to
		 * .text.itcm.stella.* shifted the function several KB later and
		 * cost ~1 fps from cache-set drift. See ce_iface.c
		 * maybe_relocate_to_itcm() for the runtime copy. */
		. = ALIGN(32);
		__stella_itcm_text_start = .;
		KEEP(*(.text.itcm.stella.*))
		. = ALIGN(32);
		__stella_itcm_text_end = .;
		. = ALIGN(32);

		KEEP(*(.text.stellapd_hot.cpu_step_custom))
		. = ALIGN(32);
		KEEP(*(.text.stellapd_hot.execute_smol))
		. = ALIGN(32);
		KEEP(*(.text.stellapd_hot.tia_poke))
		. = ALIGN(32);
		KEEP(*(.text.stellapd_hot.tia_updateFrame))
		. = ALIGN(32);
		KEEP(*(.text.stellapd_hot.tia_handleObjectsAndCollisions))
		. = ALIGN(32);
		KEEP(*(.text.stellapd_hot.tia_handleObjectsNoCollisions))
		. = ALIGN(32);
		KEEP(*(.text.stellapd_hot.stella_emit_scanline))
		. = ALIGN(32);
		KEEP(*(.text.stellapd_hot.*))
		. = ALIGN(32);

		/* Default .text -- everything not explicitly cold. Excluding
		 * ce_iface.o keeps libcrankemu glue code (~7 KB of new functions
		 * none of which are called on the standalone hot path) from
		 * displacing warm functions to different I-cache set residues.
		 * Functions tagged .text_cold.* (note the underscore, so the
		 * generic .text.* glob doesn't pick them up here) are scooped
		 * after the default sweep below. */
		*(EXCLUDE_FILE (*ce_iface.o) .text)
		*(EXCLUDE_FILE (*ce_iface.o) .text.*)

		/* Explicitly-cold functions placed after warm code. */
		*(.text_cold.*)
		*ce_iface.o(.text .text.*)

		KEEP(*(.init))
		KEEP(*(.fini))

		/* .ctors */
		*crtbegin.o(.ctors)
		*crtbegin?.o(.ctors)
		*(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
		*(SORT(.ctors.*))
		*(.ctors)

		/* .dtors */
 		*crtbegin.o(.dtors)
 		*crtbegin?.o(.dtors)
 		*(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
 		*(SORT(.dtors.*))
 		*(.dtors)

		*(.rodata*)

		KEEP(*(.eh_frame*))
	}

	.data :
	{
		__etext = .;

		__data_start__ = .;
		*(vtable)
		*(.data*)

		. = ALIGN(4);
		PROVIDE_HIDDEN (__preinit_array_start = .);
		KEEP(*(.preinit_array))
		PROVIDE_HIDDEN (__preinit_array_end = .);

		. = ALIGN(4);
		PROVIDE_HIDDEN (__init_array_start = .);
		KEEP(*(SORT(.init_array.*)))
		KEEP(*(.init_array))
		PROVIDE_HIDDEN (__init_array_end = .);

		. = ALIGN(4);
		PROVIDE_HIDDEN (__fini_array_start = .);
		KEEP(*(SORT(.fini_array.*)))
		KEEP(*(.fini_array))
		PROVIDE_HIDDEN (__fini_array_end = .);

		. = ALIGN(4);
		__data_end__ = .;
	}

	.bss :
	{
		. = ALIGN(4);
		__bss_start__ = .;

		/* Anchor the hottest BSS tables at a fixed 256-byte-aligned slot
		 * at the very start of BSS so unrelated BSS growth elsewhere can't
		 * shift them to a different D-cache set residue and regress FPS.
		 * Symmetric to the .text.stellapd_hot anchor above. */
		. = ALIGN(0x100);
		/* M6502Low instance first: its `this`-relative loads dominate the
		 * inner interpreter loop, and the instance straddles a 32 B line
		 * boundary, so its address is the most cache-residue-sensitive. */
		KEEP(*(.bss.stellapd_hot.m6502))
		. = ALIGN(32);
		KEEP(*(.bss.stellapd_hot.opdecode_table))
		. = ALIGN(32);
		KEEP(*(.bss.stellapd_hot.lumaLUT))
		. = ALIGN(32);
		KEEP(*(.bss.stellapd_hot.*))
		. = ALIGN(32);

		*(.bss*)
		*(COMMON)
		*(COM)
		. = ALIGN(4);
		__bss_end__ = .;
	}

  /DISCARD/ :
  {
		*(.ARM.exidx)
  }
}
