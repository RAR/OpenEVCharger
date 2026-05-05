/* GD32F205VE linker script
 * Flash: 512 KB @ 0x08000000
 * RAM:   128 KB @ 0x20000000
 *
 * Stack at top of RAM, growing down.
 * Heap from end of .bss upward (small for M0; FreeRTOS-managed in M1+).
 *
 * Filename retained as gd32f205vc.ld for plan continuity, but content
 * matches the GD32F205VE actually present (see project memory entry
 * `project_rippleon_chip_variant`).
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH  (rx) : ORIGIN = 0x08000000, LENGTH = 512K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

_estack       = ORIGIN(RAM) + LENGTH(RAM);
_min_heap     = 0x200;     /* 512 B minimum heap */
_min_stack    = 0x800;     /* 2 KB minimum stack */

SECTIONS
{
    .isr_vector : {
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    .text : {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)

        KEEP (*(.init))
        KEEP (*(.fini))

        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .rodata : {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM       : { __exidx_start = .; *(.ARM.exidx*); __exidx_end = .; } > FLASH

    .preinit_array : {
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP (*(.preinit_array*))
        PROVIDE_HIDDEN (__preinit_array_end = .);
    } > FLASH

    .init_array : {
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP (*(SORT(.init_array.*)))
        KEEP (*(.init_array*))
        PROVIDE_HIDDEN (__init_array_end = .);
    } > FLASH

    .fini_array : {
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP (*(SORT(.fini_array.*)))
        KEEP (*(.fini_array*))
        PROVIDE_HIDDEN (__fini_array_end = .);
    } > FLASH

    /* RAM-resident initialised data; loaded from flash at startup */
    _sidata = LOADADDR(.data);
    .data : {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    /* RAM-resident code. Used by the OTA apply path so the FMC erase /
     * word-program sequence (and the orchestrator that drives it) runs
     * entirely out of RAM, leaving the flash bus free to be erased.
     * Loaded from flash at startup like .data, but executed from RAM.
     * Vendor startup_gd32f20x_cl.S only knows how to copy .data — main()
     * does the .ramfunc copy explicitly before invoking any function in
     * this section. */
    _siramfunc = LOADADDR(.ramfunc);
    .ramfunc : {
        . = ALIGN(4);
        _sramfunc = .;
        *(.ramfunc)
        *(.ramfunc.*)
        . = ALIGN(4);
        _eramfunc = .;
    } > RAM AT > FLASH

    .bss : {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > RAM

    /* Heap + stack room check */
    ._user_heap_stack : {
        . = ALIGN(8);
        PROVIDE ( end = . );
        PROVIDE ( _end = . );
        . = . + _min_heap;
        . = . + _min_stack;
        . = ALIGN(8);
    } > RAM

    /DISCARD/ : {
        libc.a ( * )
        libm.a ( * )
        libgcc.a ( * )
    }

    .ARM.attributes 0 : { *(.ARM.attributes) }
}
