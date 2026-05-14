# nx_flash.tcl — Manual N32G45x flash programmer for OpenOCD.
#
# Drives the STM32F1-style flash controller at 0x40022000 by direct
# register pokes. Bypasses OpenOCD's stm32f1x flash driver because
# its auto_probe rejects the Nations device_id (DEV_ID 0x511) — the
# rippleon GD32F205 trick (DEV_ID 0x418, which IS in the F1 allowlist)
# doesn't work here because Nations chose a Nations-specific dev_id.
#
# Strategy: load_image bulk-uploads the binary to RAM at 0x20002000,
# then a TCL loop reads halfwords from RAM via SWD and writes to flash
# via SWD with PG=1 in FLASH_CR. The flash controller registers are
# F1-compatible by inspection.
#
# Run with:
#   NX_FLASH_BIN=/path/to/foo.bin openocd \
#     -f interface/stlink.cfg \
#     -c "transport select hla_swd" \
#     -f target/stm32f4x.cfg \
#     -f nx_flash.tcl

# Flash controller (STM32F1-style FPEC).
set NX_FLASH_BASE        0x40022000
set NX_FLASH_KEYR        [expr {$NX_FLASH_BASE + 0x04}]
set NX_FLASH_SR          [expr {$NX_FLASH_BASE + 0x0C}]
set NX_FLASH_CR          [expr {$NX_FLASH_BASE + 0x10}]
set NX_FLASH_AR          [expr {$NX_FLASH_BASE + 0x14}]
set NX_FLASH_KEY1        0x45670123
set NX_FLASH_KEY2        0xCDEF89AB
set NX_FLASH_CR_PG       0x01
set NX_FLASH_CR_PER      0x02
set NX_FLASH_CR_STRT     0x40
set NX_FLASH_CR_LOCK     0x80
set NX_FLASH_SR_BSY      0x01
set NX_FLASH_SR_PGERR    0x04
set NX_FLASH_SR_WRPRTERR 0x10
set NX_FLASH_SR_EOP      0x20

set NX_PAGE_SIZE         0x800  ;# 2 KB (high-density F1 layout)
set NX_FLASH_ORIGIN      0x08000000
set NX_RAM_STAGING       0x20002000  ;# avoid first 8 KB (vector table + FreeRTOS heap)

proc nx_read32 {addr} {
    set out [capture "mdw $addr"]
    if {[regexp {0x[0-9a-fA-F]+:\s*([0-9a-fA-F]+)} $out -> val]} {
        return [expr {"0x$val"}]
    }
    error "nx_read32: failed to parse '$out'"
}

proc nx_wait_bsy {} {
    global NX_FLASH_SR NX_FLASH_SR_BSY
    for {set i 0} {$i < 10000} {incr i} {
        set sr [nx_read32 $NX_FLASH_SR]
        if {($sr & $NX_FLASH_SR_BSY) == 0} {
            return $sr
        }
    }
    error "nx_wait_bsy: timeout (last SR=0x[format %x $sr])"
}

proc nx_unlock {} {
    global NX_FLASH_CR NX_FLASH_CR_LOCK NX_FLASH_KEYR NX_FLASH_KEY1 NX_FLASH_KEY2
    set cr [nx_read32 $NX_FLASH_CR]
    if {($cr & $NX_FLASH_CR_LOCK) != 0} {
        mww $NX_FLASH_KEYR $NX_FLASH_KEY1
        mww $NX_FLASH_KEYR $NX_FLASH_KEY2
    }
    set cr [nx_read32 $NX_FLASH_CR]
    if {($cr & $NX_FLASH_CR_LOCK) != 0} {
        error "nx_unlock: flash still locked, CR=0x[format %x $cr]"
    }
    puts "flash: unlocked  (CR=0x[format %x $cr])"
}

proc nx_clear_sr {} {
    global NX_FLASH_SR NX_FLASH_SR_PGERR NX_FLASH_SR_WRPRTERR NX_FLASH_SR_EOP
    mww $NX_FLASH_SR [expr {$NX_FLASH_SR_PGERR | $NX_FLASH_SR_WRPRTERR | $NX_FLASH_SR_EOP}]
}

proc nx_erase_page {page_addr} {
    global NX_FLASH_CR NX_FLASH_AR NX_FLASH_CR_PER NX_FLASH_CR_STRT
    nx_wait_bsy
    nx_clear_sr
    mww $NX_FLASH_CR $NX_FLASH_CR_PER
    mww $NX_FLASH_AR $page_addr
    mww $NX_FLASH_CR [expr {$NX_FLASH_CR_PER | $NX_FLASH_CR_STRT}]
    set sr [nx_wait_bsy]
    mww $NX_FLASH_CR 0
    puts [format "flash: erased page 0x%08x  (SR=0x%02x)" $page_addr $sr]
}

proc nx_flash {bin_path {target_offset 0} {do_init 1} {do_reset_run 1}} {
    global NX_FLASH_CR NX_FLASH_CR_PG NX_FLASH_ORIGIN NX_PAGE_SIZE
    global NX_FLASH_CR_LOCK NX_FLASH_SR NX_FLASH_SR_PGERR NX_FLASH_SR_WRPRTERR
    global NX_RAM_STAGING

    set target_base [expr {$NX_FLASH_ORIGIN + $target_offset}]
    puts [format "flash: target base = 0x%08x  (offset 0x%x from origin)" \
                 $target_base $target_offset]

    set sz [file size $bin_path]
    if {$sz == 0} {
        error "nx_flash: empty file '$bin_path'"
    }
    # Pad up to halfword boundary in-memory by reading file size + 1 if odd.
    if {[expr {$sz & 1}] != 0} {
        set sz [expr {$sz + 1}]
        puts "flash: file has odd size; padding final byte with 0xFF"
    }
    set num_halfwords [expr {$sz / 2}]
    set pages_needed  [expr {($sz + $NX_PAGE_SIZE - 1) / $NX_PAGE_SIZE}]
    puts [format "flash: %d bytes (%d halfwords), %d pages of %d B" \
                 $sz $num_halfwords $pages_needed $NX_PAGE_SIZE]

    if {$do_init} {
        init
        reset halt
    }

    # Bulk-load the binary into RAM via openocd's load_image (uses SWD
    # block writes, ~10× faster than per-word mww). Address 0x20002000
    # leaves the vector table area + early FreeRTOS heap untouched.
    puts "flash: load_image -> RAM @ [format 0x%08x $NX_RAM_STAGING]"
    load_image $bin_path $NX_RAM_STAGING bin

    nx_unlock

    # Erase target pages.
    for {set p 0} {$p < $pages_needed} {incr p} {
        set page_addr [expr {$target_base + $p * $NX_PAGE_SIZE}]
        nx_erase_page $page_addr
    }

    # Programming: PG=1, write halfwords from RAM staging.
    nx_wait_bsy
    nx_clear_sr
    mww $NX_FLASH_CR $NX_FLASH_CR_PG
    puts "flash: programming..."

    # Read RAM in 32-bit words; write to flash with mww (word).
    # Single-halfword `mwh` writes silently fail through the ST-Link
    # AHB-AP when PG=1 (bench-confirmed 2026-05-11 on this STLink
    # V2J37S7). Word writes go through cleanly — the flash controller
    # accepts them as 2-halfword bursts internally.
    set num_words [expr {($num_halfwords + 1) / 2}]
    for {set w 0} {$w < $num_words} {incr w} {
        set ram_addr [expr {$NX_RAM_STAGING + $w * 4}]
        set word [nx_read32 $ram_addr]
        set flash_addr [expr {$target_base + $w * 4}]

        mww $flash_addr $word

        # BSY drain + progress every 128 words.
        if {($w & 0x7F) == 0x7F || $w == $num_words - 1} {
            set sr [nx_wait_bsy]
            if {($sr & ($NX_FLASH_SR_PGERR | $NX_FLASH_SR_WRPRTERR)) != 0} {
                mww $NX_FLASH_CR 0
                error "flash: error SR=0x[format %x $sr] at word $w"
            }
            puts [format "  %d/%d words  (SR=0x%02x)" \
                         [expr {$w + 1}] $num_words $sr]
        }
    }
    nx_wait_bsy
    puts ""
    puts "flash: programming done"

    # Clear PG, lock.
    mww $NX_FLASH_CR 0
    mww $NX_FLASH_CR $NX_FLASH_CR_LOCK

    # Verify: spot-check a few words across the image.
    puts "flash: verify..."
    foreach offset [list 0 [expr {$sz / 2}] [expr {$sz - 4}]] {
        if {$offset < 0} { set offset 0 }
        set f [nx_read32 [expr {$target_base + $offset}]]
        set r [nx_read32 [expr {$NX_RAM_STAGING + $offset}]]
        set ok [expr {$f == $r ? "OK" : "MISMATCH"}]
        puts [format "  +0x%04x  flash=0x%08x  ram=0x%08x  %s" $offset $f $r $ok]
    }

    if {$do_reset_run} {
        reset run
        puts "flash: reset run — new firmware should be executing"
    }
}

# Auto-invoke if NX_FLASH_BIN env var is set.
# NX_FLASH_TARGET_OFFSET (optional, hex/dec) shifts the target base from
# 0x08000000 — used by restore_stock_nexcyber.sh for chunked 128 KB flashes.
if {[info exists ::env(NX_FLASH_BIN)]} {
    set _ofs 0
    if {[info exists ::env(NX_FLASH_TARGET_OFFSET)]} {
        set _ofs [expr $::env(NX_FLASH_TARGET_OFFSET)]
    }
    set _no_reset 0
    if {[info exists ::env(NX_FLASH_NO_RESET_RUN)]} {
        set _no_reset 1
    }
    nx_flash $::env(NX_FLASH_BIN) $_ofs 1 [expr {!$_no_reset}]
    shutdown
}
