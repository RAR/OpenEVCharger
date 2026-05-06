#!/usr/bin/env bash
# Run firmware and stream printk output via ARM semihosting.
# Use Ctrl-C to stop. CHIP MAY HANG AFTER you stop — see below.
#
# IMPORTANT: ARM semihosting BKPT 0xAB halts the chip when no host is
# servicing it AND DHCSR.C_DEBUGEN is set (sticky after openocd
# disconnect). Default firmware build has OPENEVCHARGER_SEMIHOSTING=0 so
# the chip never issues the BKPT — printk goes only over the (PA9)
# UART, which is physically inaccessible on this bench.
#
# To capture printk: rebuild with the flag, flash, then run monitor:
#
#   cmake -B build -DOPENEVCHARGER_SEMIHOSTING=1
#   cmake --build build
#   tools/flash.sh
#   tools/openocd-monitor.sh   # leave this running; do NOT Ctrl-C
#                              # without re-flashing semihost-off
#                              # build first or chip will freeze on
#                              # next printk.
#
# When done debugging: flash a semihost-off build to recover chip
# autonomy.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFG="$REPO_ROOT/tools/openocd-gd32f205.cfg"

echo "Starting OpenOCD with semihosting enabled. Ctrl-C to stop."
echo "(Reminder: chip will freeze on next printk after you Ctrl-C"
echo " unless firmware was built with OPENEVCHARGER_SEMIHOSTING=0.)"
exec openocd -f "$CFG" \
    -c "init" \
    -c "reset halt" \
    -c "arm semihosting enable" \
    -c "reset run"
