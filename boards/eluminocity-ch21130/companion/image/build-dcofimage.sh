#!/bin/sh
# build-dcofimage.sh — assemble a USB-flashable DcoFImage for the Delta EVMU30.
#
# Per docs/22, the M12 image *replaces* the stock leaf I/O daemons
# (RFID, MeterIC_new, Adc, LED_control) with delta-bridge personalities,
# keeps stock for the safety-critical/under-decoded pieces (main, Pri_Comm,
# Charging_Standard_RFID, FlashLog, RTC, ErrorHandle, snmpd, mini_httpd,
# wpa_supplicant), and trims dead weight (DeltaOCPP + the factory build
# tools).
#
# Produces, in the output dir:
#   DcoFImage                — our M12 trimmed + replaced image
#   DcoFImage-stock-restore  — untouched stock rootfs, wrapped (rollback path)
#
# Usage:
#   build-dcofimage.sh <stock-rootfs-dir> <delta-bridge-binary> <output-dir>
#
# Requires: mkfs.jffs2 (mtd-utils), python3.
# mtd5 geometry — MUST match the unit (verified against /proc/mtd +
# U-Boot flinfo + a fresh DcoFImage-stock-restore reflash that booted
# cleanly through /sbin/init on 2026-05-19):
#   erase block size 64 KiB (0x10000), little-endian, 16 MiB total.
# The earlier value 0x20000 (128 KiB) was wrong and caused stock's flash
# routine to half-erase the upper half of mtd5 (alternating 64 KiB
# sectors); the resulting Frankenstein JFFS2 mounted but panicked at
# /sbin/init with a libc symbol mismatch (init from one image, libc from
# the other). See docs/24.
set -e

if [ $# -lt 3 ]; then
    sed -n '2,/^set -e$/p' "$0"
    exit 1
fi

STOCK_ROOTFS="$1"
BRIDGE_BIN="$2"
OUT_DIR="$3"

if [ ! -d "$STOCK_ROOTFS" ]; then
    echo "error: stock rootfs '$STOCK_ROOTFS' is not a directory" >&2
    exit 1
fi
if [ ! -f "$BRIDGE_BIN" ]; then
    echo "error: delta-bridge binary '$BRIDGE_BIN' not found" >&2
    exit 1
fi
if ! command -v mkfs.jffs2 >/dev/null 2>&1; then
    echo "error: mkfs.jffs2 not found (apt install mtd-utils)" >&2
    exit 1
fi

# --squash-uids: builder runs unprivileged but the target unit's kernel
# expects all rootfs files owned by root:root. Squash both stock and our
# images so the byte-level layout matches a real on-device extract.
# --eraseblock=0x10000: see the geometry comment above — this MUST match
# the hardware sector size or stock's flasher will produce a
# half-erased mtd5 that bricks the bench.
JFFS2_OPTS="--little-endian --eraseblock=0x10000 --pad=0x1000000 --squash-uids"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$OUT_DIR"

# --- 1. stock-restore image: untouched rootfs, just wrapped ---
echo "[1/4] mkfs.jffs2 stock rootfs"
mkfs.jffs2 $JFFS2_OPTS -r "$STOCK_ROOTFS" -o "$WORK/stock.jffs2"
echo "[1/4] wrap_dco stock"
python3 "$HERE/wrap_dco.py" "$WORK/stock.jffs2" "$OUT_DIR/DcoFImage-stock-restore"

# --- 2. stage our M12 rootfs ---
echo "[2/4] stage M12 rootfs"
ROOT="$WORK/rootfs"
cp -a "$STOCK_ROOTFS" "$ROOT"

# Our binary.
install -m 0755 "$BRIDGE_BIN" "$ROOT/root/delta-bridge"

# --- 2a. wrappers replacing stock leaf daemons ---
# Each wrapper preserves the path /root/main expects to fork. Rollback
# is a one-line cp from /Storage/stk/ (populated by first-boot.sh from
# operator-supplied /UsbFlash/stk/ — see docs/22 §rollback).

cat > "$ROOT/root/RFID" <<'EOF'
#!/bin/ash
# M12 wrapper — main delta-bridge (MQTT + web + RFID reader).
# Rollback: cp /Storage/stk/RFID /root/RFID; sync; reboot
exec /root/delta-bridge -c /Storage/delta-bridge.conf \
    >> /Storage/delta-bridge/bridge-boot.log 2>&1
EOF
chmod 0755 "$ROOT/root/RFID"

cat > "$ROOT/root/Adc" <<'EOF'
#!/bin/ash
# M12 wrapper — delta-bridge adc personality (docs/17).
# Rollback: cp /Storage/stk/Adc /root/Adc; sync; reboot
exec /root/delta-bridge --personality=adc
EOF
chmod 0755 "$ROOT/root/Adc"

cat > "$ROOT/root/MeterIC_new" <<'EOF'
#!/bin/ash
# M12 wrapper — delta-bridge meter personality (docs/16).
# Rollback: cp /Storage/stk/MeterIC_new /root/MeterIC_new; sync; reboot
exec /root/delta-bridge --personality=meter
EOF
chmod 0755 "$ROOT/root/MeterIC_new"

cat > "$ROOT/root/LED_control" <<'EOF'
#!/bin/ash
# M12 wrapper — delta-bridge led personality (docs/19+21).
# Rollback: cp /Storage/stk/LED_control /root/LED_control; sync; reboot
exec /root/delta-bridge --personality=led
EOF
chmod 0755 "$ROOT/root/LED_control"

# --- 2b. default config + first-boot script ---
mkdir -p "$ROOT/etc/delta-bridge"

cat > "$ROOT/etc/delta-bridge.conf.default" <<'EOF'
# delta-bridge default config — copied to /Storage/delta-bridge.conf on
# first boot by /etc/delta-bridge/first-boot.sh. Edit via the web UI
# (Config tab) or directly on /Storage/delta-bridge.conf.

# MQTT — point at your broker (defaults are placeholders).
broker_host  = 127.0.0.1
broker_port  = 1883
topic_prefix = delta-bridge

# Web UI — http://<unit-ip>:8080/, HTTP Basic auth.
# CHANGE THE PASSWORD on first login (Config tab).
web_enable   = true
web_port     = 8080
web_user     = admin
web_pass     = changeme

# Write controls — required for any UI action that changes state.
# Off by default so a misconfig can't accidentally drive the EVSE.
write_enable = false

# RFID reader — delta-bridge owns /dev/ttyAMA4 + PWM init.
rfid_enable  = true
rfid_port    = /dev/ttyAMA4

# Logging — log_path matches the /root/RFID wrapper's stderr redirect
# so /api/log reads real content.
log_level    = info
log_path     = /Storage/delta-bridge/bridge-boot.log

# Metering — empirical voltage scale; tune per-unit if needed.
meter_v_scale = 60.000
EOF

cat > "$ROOT/etc/delta-bridge/stk-manifest.txt" <<'EOF'
RFID
Adc
MeterIC_new
LED_control
EOF

cat > "$ROOT/etc/delta-bridge/first-boot.sh" <<'EOF'
#!/bin/ash
# first-boot.sh — idempotent, runs on every boot from /etc/funs.
# Runs BEFORE /root/main starts, so we have exclusive USB access.
#
# Responsibilities:
#   1. Ensure /Storage/delta-bridge/ exists for the bridge's stderr log.
#   2. Seed /Storage/delta-bridge.conf from the shipped default if missing.
#   3. Mount USB if a stick is present, then:
#       a. Import /UsbFlash/delta-bridge.conf if byte-different from the
#          on-disk copy (mirrors stock's DeltaEVSEConfig pattern, see
#          docs/23). Source left in place; operator unplugs when done.
#       b. Pre-stage stock binaries from /UsbFlash/stk/ into /Storage/stk/
#          for in-place per-binary rollback. (Full rollback is always
#          available via DcoFImage-stock-restore reflash.)
#      Unmount USB on the way out so main's polled mount logic takes over.
#
# Caveat: USB must be inserted BEFORE power-on for first-boot.sh to fire
# on that boot — this script is one-shot, not polled. (Stock's
# DeltaEVSEConfig handler in /root/main is polled and handles hotplug.)
#
# All steps no-op once their target exists. Output goes to /etc/funs's
# stdout (kernel console / boot log).

DBDIR=/etc/delta-bridge
STK=/Storage/stk
CONF=/Storage/delta-bridge.conf
LOGDIR=/Storage/delta-bridge
UF=/UsbFlash
USB_SRC=$UF/delta-bridge.conf

mkdir -p "$LOGDIR" "$STK" "$UF"

# Reap any partial .new from an interrupted prior import.
rm -f "$CONF.new"

# Seed config from default if missing (independent of USB).
if [ ! -f "$CONF" ]; then
    cp "$DBDIR/delta-bridge.conf.default" "$CONF"
    echo "first-boot: seeded $CONF from default"
fi

# Mount USB if a stick is present. /etc/rc runs `mdev -s` before us, so
# /dev/sda{,1} block-device nodes exist iff the kernel sees the stick.
mounted_by_us=0
if ! grep -q " $UF " /proc/mounts 2>/dev/null; then
    for dev in /dev/sda /dev/sda1; do
        if [ -b "$dev" ] && mount "$dev" "$UF" 2>/dev/null; then
            mounted_by_us=1
            break
        fi
    done
fi

# Import operator-supplied config from USB if byte-different. Atomic via
# .new+mv; nothing else writes $CONF at this point in boot.
if [ -f "$USB_SRC" ]; then
    if ! cmp -s "$USB_SRC" "$CONF"; then
        if cp "$USB_SRC" "$CONF.new"; then
            mv "$CONF.new" "$CONF"
            echo "first-boot: imported $USB_SRC -> $CONF"
        fi
    fi
fi

# Pre-stage stock binaries for in-place rollback. No-op once $STK is full.
if [ -f "$DBDIR/stk-manifest.txt" ]; then
    while read -r name; do
        [ -z "$name" ] && continue
        [ -f "$STK/$name" ] && continue
        if [ -f "$UF/stk/$name" ]; then
            cp "$UF/stk/$name" "$STK/$name"
            chmod +x "$STK/$name"
            echo "first-boot: backed up $UF/stk/$name -> $STK/$name"
        fi
    done < "$DBDIR/stk-manifest.txt"
fi

# Release USB so main's polled mount logic can take over.
if [ "$mounted_by_us" = "1" ]; then
    umount "$UF" 2>/dev/null || true
fi
EOF
chmod 0755 "$ROOT/etc/delta-bridge/first-boot.sh"

# --- 2c. patch /etc/funs to run first-boot.sh before any daemon ---
# Inject as the second line (after `#! /bin/ash`); keep the rest unchanged.
{
    head -n 1 "$ROOT/etc/funs"
    echo "/etc/delta-bridge/first-boot.sh"
    tail -n +2 "$ROOT/etc/funs"
} > "$ROOT/etc/funs.new"
mv "$ROOT/etc/funs.new" "$ROOT/etc/funs"
chmod 0755 "$ROOT/etc/funs"

# --- 2d. trim dead weight ---
# Each removal is justified in docs/22 §"Rootfs contents — keep/replace/remove".
TRIMMED_BYTES=0
for f in DeltaOCPP ACFWMaker FWMaker ScenarioMaker Pri_Comm_cqc NTC_tmp \
         PowerCard-UltraLight Charging_Standard; do
    if [ -f "$ROOT/root/$f" ]; then
        SZ=$(wc -c < "$ROOT/root/$f")
        TRIMMED_BYTES=$((TRIMMED_BYTES + SZ))
        rm -f "$ROOT/root/$f"
        echo "    trimmed /root/$f (${SZ} B)"
    fi
done
echo "    total trimmed: ${TRIMMED_BYTES} B"

# --- 3. mkfs.jffs2 ours ---
echo "[3/4] mkfs.jffs2 M12 rootfs"
mkfs.jffs2 $JFFS2_OPTS -r "$ROOT" -o "$WORK/ours.jffs2"

# --- 4. wrap_dco ours ---
echo "[4/4] wrap_dco M12"
python3 "$HERE/wrap_dco.py" "$WORK/ours.jffs2" "$OUT_DIR/DcoFImage"

echo
echo "built:"
ls -la "$OUT_DIR/DcoFImage" "$OUT_DIR/DcoFImage-stock-restore"
echo
# JFFS2 fills the partition with 0xff padding to 16 MiB; comparing total
# file size is meaningless (both hit the ceiling). Approximate real
# savings by counting non-0xff bytes in the JFFS2 payload — that's a
# rough proxy for "data written" (JFFS2 uses zlib so it's smaller than
# raw binary size, but the ratio of the two images is what matters).
jffs2_used() {
    python3 -c '
import sys
b = open(sys.argv[1], "rb").read()[:-13]   # strip DELTADCOF + sum trailer
print(sum(1 for x in b if x != 0xff))
' "$1"
}
SR_USED=$(jffs2_used "$OUT_DIR/DcoFImage-stock-restore")
M12_USED=$(jffs2_used "$OUT_DIR/DcoFImage")
SAVED=$((SR_USED - M12_USED))
PCT=$((SAVED * 100 / SR_USED))
echo "JFFS2 footprint (non-padding bytes, zlib-compressed):"
echo "  stock-restore: $SR_USED B"
echo "  M12:           $M12_USED B"
echo "  saved:         $SAVED B  (${PCT}%)"
