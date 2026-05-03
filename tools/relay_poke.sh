#!/usr/bin/env bash
# Manual GPIO probe for the relay cascade investigation.
# Usage: tools/relay_poke.sh <cmd>
#
# Subcommands:
#   init        - reconfigure PB12 as output PP 50MHz (default is input float).
#                 Run once per power cycle; PB12 reverts to input on reset.
#   uninit      - reconfigure PB12 back to input float.
#   pe12-hi     - drive PE12 HIGH (Model A: close), leave PB12 alone
#   pe12-lo     - drive PE12 LOW  (Model A: open),  leave PB12 alone
#   pb12-hi     - drive PB12 HIGH (needs `init` first), leave PE12 alone
#   pb12-lo     - drive PB12 LOW  (needs `init` first), leave PE12 alone
#   both-hi     - drive PE12 + PB12 both HIGH
#   both-lo     - drive both LOW
#   read        - dump PE12, PB12 OCTL+ISTAT, NTC2 sense raw
#
# Runs in halt+poke+resume mode so the firmware's safety_task can't
# fight your manual writes.

set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
CFG="$REPO/tools/openocd-gd32f205.cfg"
OCD() { openocd -f "$CFG" "$@" 2>&1 | grep -E '0x4001|0x20000|halted' || true; }

case "${1:-help}" in
init)    OCD -c init -c halt -c "mmw 0x40010C04 0x00030000 0x000F0000" -c "mdw 0x40010C04 1" -c resume -c shutdown ;;
uninit)  OCD -c init -c halt -c "mmw 0x40010C04 0x00040000 0x000F0000" -c "mdw 0x40010C04 1" -c resume -c shutdown ;;
pe12-hi) OCD -c init -c halt -c "mww 0x40011810 0x00001000" -c resume -c shutdown ;;
pe12-lo) OCD -c init -c halt -c "mww 0x40011814 0x00001000" -c resume -c shutdown ;;
pb12-hi) OCD -c init -c halt -c "mww 0x40010C10 0x00001000" -c resume -c shutdown ;;
pb12-lo) OCD -c init -c halt -c "mww 0x40010C14 0x00001000" -c resume -c shutdown ;;
both-hi) OCD -c init -c halt -c "mww 0x40011810 0x00001000" -c "mww 0x40010C10 0x00001000" -c resume -c shutdown ;;
both-lo) OCD -c init -c halt -c "mww 0x40011814 0x00001000" -c "mww 0x40010C14 0x00001000" -c resume -c shutdown ;;
read)    OCD -c init -c halt \
             -c "echo {PE12_OCTL:}" -c "mdw 0x4001180C 1" \
             -c "echo {PB12_OCTL:}" -c "mdw 0x40010C0C 1" \
             -c "echo {PB12_ISTAT:}" -c "mdw 0x40010C08 1" \
             -c "echo {NTC2_sense:}" -c "mdh 0x20000028 1" \
             -c resume -c shutdown ;;
*) sed -n '2,20p' "$0"; exit 1 ;;
esac
