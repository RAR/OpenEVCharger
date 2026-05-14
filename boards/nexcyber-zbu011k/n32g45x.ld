/* Nations N32G457RxL linker script (Nexcyber bench unit)
 *
 * Confirmed via 2026-05-08 SWD memory probe (see project memory entry
 * project_nexcyber_n32g45x_swd_probe):
 *   Flash:   128 KB @ 0x08000000  (probed: full 0x00000-0x1FFFF dumped)
 *   SRAM:    80 KB  @ 0x20000000  (probed: read passes at +0x13FFC,
 *                                   bus-faults at +0x14000 → 80 KB exactly)
 *
 * Reservations:
 *   - Top 8 KB of flash (0x0801E000-0x0801FFFF) carved off for
 *     persistence ping-pong (cal record + boot config + small
 *     RFID authlist + event log). Per the feasibility doc Option A,
 *     this loses the W25Q's 100k-write-cycle endurance but the cal
 *     record changes once per chassis lifetime and the event log can
 *     be rate-limited.
 *
 *     Sector geometry on N32G457RxL is per-2 KB pages (Nations
 *     N32G45x reference manual, FMC chapter — same as STM32F1).
 *     8 KB = 4 sectors; two ping-pong slots × 2 sectors apiece.
 *     CONFIRM at bench by probing FMC_OBR / page-erase write cycles.
 *
 *   - OTA staging: NONE on internal flash. v1 ships single-bank,
 *     CRC-verify-pre-flash, atomic flash-from-RAM apply. Power loss
 *     mid-write bricks the chip → SWD recovery only. Documented in
 *     docs/ports/nexcyber-feasibility.md "Persistence + OTA design".
 *
 * Stack at top of RAM, growing down.
 * Heap from end of .bss upward (small for bring-up; FreeRTOS-managed
 * once the task layer is ported).
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH      (rx) : ORIGIN = 0x08000000, LENGTH = 120K
    PERSIST    (r)  : ORIGIN = 0x0801E000, LENGTH = 8K
    RAM        (rwx): ORIGIN = 0x20000000, LENGTH = 80K
}

_estack       = ORIGIN(RAM) + LENGTH(RAM);
_min_heap     = 0x200;     /* 512 B minimum heap */
_min_stack    = 0x800;     /* 2 KB minimum stack */

/* Persistence region symbols — exposed for the persist-task drivers. */
_spersist     = ORIGIN(PERSIST);
_epersist     = ORIGIN(PERSIST) + LENGTH(PERSIST);

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

    /* RAM-resident code (RAM-staged flash apply path; same idiom as
     * the rippleon target). The persist driver erases / programs
     * the persistence sectors above through this path. */
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
