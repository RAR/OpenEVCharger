#!/usr/bin/env bash
# Run firmware and stream printk output via ARM semihosting.
# Use Ctrl-C to stop; the chip keeps running afterwards.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFG="$REPO_ROOT/tools/openocd-gd32f205.cfg"

echo "Starting OpenOCD with semihosting enabled. Ctrl-C to stop."
exec openocd -f "$CFG" \
    -c "init" \
    -c "reset halt" \
    -c "arm semihosting enable" \
    -c "reset run"
