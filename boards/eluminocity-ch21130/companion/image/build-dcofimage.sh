#!/bin/sh
# build-dcofimage.sh — assemble a USB-flashable DcoFImage for the Delta EVMU30.
#
# Produces, in the output dir:
#   DcoFImage                — stock rootfs + delta-bridge + dropbear + autostart
#   DcoFImage-stock-restore  — untouched stock rootfs, same wrapper (revert path)
#
# Usage:
#   build-dcofimage.sh <stock-rootfs-dir> <delta-bridge-binary> <output-dir> \
#                      [--authorized-key <pubkey-file>] [--dropbear <binary>]
#
# Requires: mkfs.jffs2 (mtd-utils), python3.
# mtd5 geometry — MUST match the unit (validated at milestone M2 by flashing
# DcoFImage-stock-restore first and confirming an identical boot):
#   erase block size 128 KiB, little-endian, 16 MiB total.
set -e

STOCK_ROOTFS="$1"
BRIDGE_BIN="$2"
OUT_DIR="$3"
shift 3 || { echo "usage: see header"; exit 1; }

AUTH_KEY=""
DROPBEAR_BIN=""
while [ $# -gt 0 ]; do
    case "$1" in
        --authorized-key) AUTH_KEY="$2"; shift 2 ;;
        --dropbear)       DROPBEAR_BIN="$2"; shift 2 ;;
        *) echo "unknown arg: $1"; exit 1 ;;
    esac
done

JFFS2_OPTS="--little-endian --eraseblock=0x20000 --pad=0x1000000"
HERE="$(dirname "$0")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$OUT_DIR"

# --- 1. stock-restore image: untouched rootfs, just wrapped ---
mkfs.jffs2 $JFFS2_OPTS -r "$STOCK_ROOTFS" -o "$WORK/stock.jffs2"
python3 "$HERE/wrap_dco.py" "$WORK/stock.jffs2" "$OUT_DIR/DcoFImage-stock-restore"

# --- 2. our image: copy rootfs, inject bridge + dropbear + autostart ---
cp -a "$STOCK_ROOTFS" "$WORK/rootfs"
install -m 0755 "$BRIDGE_BIN" "$WORK/rootfs/root/delta-bridge"

# autostart line(s) appended to /etc/funs
echo '/root/delta-bridge &' >> "$WORK/rootfs/etc/funs"

if [ -n "$DROPBEAR_BIN" ]; then
    install -m 0755 "$DROPBEAR_BIN" "$WORK/rootfs/sbin/dropbear"
    mkdir -p "$WORK/rootfs/etc/dropbear"
    # host keys are generated on the unit at first boot onto /Storage; the
    # autostart line points dropbear there. key-only auth (-s: no passwords).
    echo 'mkdir -p /Storage/dropbear' >> "$WORK/rootfs/etc/funs"
    echo '[ -f /Storage/dropbear/dropbear_rsa_host_key ] || dropbearkey -t rsa -f /Storage/dropbear/dropbear_rsa_host_key' \
        >> "$WORK/rootfs/etc/funs"
    echo '/sbin/dropbear -s -r /Storage/dropbear/dropbear_rsa_host_key &' \
        >> "$WORK/rootfs/etc/funs"
    if [ -n "$AUTH_KEY" ]; then
        mkdir -p "$WORK/rootfs/root/.ssh"
        install -m 0600 "$AUTH_KEY" "$WORK/rootfs/root/.ssh/authorized_keys"
    fi
fi

mkfs.jffs2 $JFFS2_OPTS -r "$WORK/rootfs" -o "$WORK/ours.jffs2"
python3 "$HERE/wrap_dco.py" "$WORK/ours.jffs2" "$OUT_DIR/DcoFImage"

echo "built:"
echo "  $OUT_DIR/DcoFImage"
echo "  $OUT_DIR/DcoFImage-stock-restore"
