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
		KEEP(*(.text.stellapd_hot.*))
		. = ALIGN(32);

		*(.text)
		*(.text.*)

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
