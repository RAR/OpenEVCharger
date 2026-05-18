# Bench brick recovery — U-Boot USB flash procedure

**Date:** 2026-05-19 (procedure verified on the eluminocity bench during the first failed DcoFImage-stock-restore flash)
**Use when:** the bench panics at boot (kernel hangs at `/sbin/init` symbol-lookup error, or fails to mount rootfs, or any other "kernel mounted mtd5 but userland is broken" scenario).
**Tool required:** serial console + USB stick. No JTAG/SWD needed.

The bench's U-Boot is functional even when the rootfs (mtd5) is corrupt — that's our recovery surface.

---

## 0. Symptoms that mean "go to this doc"

- Kernel mounts JFFS2 on mtd5 successfully but panics immediately after with `Attempted to kill init!`.
- `init: symbol lookup error: /lib/libc.so.6: undefined symbol: X, version GLIBC_2.Y`. This is a half-flashed rootfs — init from one image, libc from another (e.g. partial DcoFImage write, eraseblock-size mismatch).
- Bench reboots in a panic loop (kernel.panic sysctl forces auto-reboot — but if you can't even get to userland, this just loops).

**Distinguish from "the kernel itself won't load":** if you don't see the kernel banner (CPU/DRAM/JFFS2 lines), the issue is in U-Boot or mtd2 (Kernel partition), not mtd5. That recovery is different (mtd2 reflash; not covered here).

---

## 1. Prep the recovery USB stick

U-Boot's `fatload usb` is hardcoded to look at partition 1, so the stick MUST have an MBR with a real partition. This is **different** from how the stock-time DcoFImage flash wants it (partition-less). For recovery you want partitioned. Plan a separate stick or be willing to reformat between phases.

```bash
# On laptop. lsblk before/after plug-in to identify the stick. Triple-check.
DEV=/dev/sdX   # REPLACE
sudo umount $DEV* 2>/dev/null
sudo wipefs -a $DEV
# Make a DOS partition table with one primary FAT32 partition:
echo -e 'o\nn\np\n1\n\n\nt\nb\nw\n' | sudo fdisk $DEV
sudo mkfs.vfat ${DEV}1
sudo mkdir -p /mnt/stk
sudo mount ${DEV}1 /mnt/stk
sudo cp /home/rar/device-configs/build/m12/DcoFImage-stock-restore /mnt/stk/DcoFImage
sudo sync
sudo umount /mnt/stk
```

The file MUST be named exactly `DcoFImage` at the partition root.

---

## 2. Get into U-Boot

1. Plug the USB stick into the bench.
2. Power-cycle the bench (full off, ≥ 5 s, on).
3. Watch the serial console (115200 8N1).
4. During the autoboot countdown ("`Hit SPACE in 1 seconds to stop autoboot.`") — **immediately mash space**. The window is short; if you miss it, the bench will fail-boot the same way and you'll get another autoboot prompt to interrupt.

You should land at `u-boot>`. If U-Boot's autoboot is calling `bootm` and bypassing the autoboot prompt, set the bootdelay env at the prompt (`setenv bootdelay 3; saveenv`) — but on this device the default is already 1 s and it does prompt.

---

## 3. Sanity-check before flashing

```
usb storage          # Should list your stick as a Mass Storage device on the EHCI hub.
fatls usb 0:1        # Should list DcoFImage (and whatever else is on the stick).
```

If `usb storage` says "0 Storage Device(s) found": `usb reset` (may fail with "EHCI fail to reset" on warm boot — power-cycle and try cold).
If `fatls usb 0` says "Partition 1 not valid": your stick is partition-less. See §1 to repartition.

---

## 4. Load + flash + verify

mtd5 (rootfs) sits at flash address **0xf9000000**, size **0x1000000** (16 MiB), in Bank #2 of the parallel NOR. Confirm via `flinfo` if you want to be paranoid.

```
# Load the file into RAM at a safe address (DRAM is 0x00000000..0x07FFFFFF):
fatload usb 0:1 0x01000000 DcoFImage

# Verify size — expect filesize=1000000d (= 16777229 = 16 MiB + 13-byte DELTADCOF trailer):
printenv filesize

# Verify first bytes look like JFFS2 (85 19 ... = LE magic, with a CLEANMARKER first):
md.b 0x01000000 0x10
# Expect something like:
#   01000000: 85 19 03 20 0c 00 00 00 ?? ?? ?? ?? 85 19 01 e0    ...

# Disable write protection on Bank #2 (covers mtd5):
protect off 0xf9000000 +0x1000000

# Erase mtd5 (256 sectors of 64 KiB each — ~30-60 s):
erase 0xf9000000 +0x1000000

# Write JFFS2 payload (first 16 MiB of the loaded file; trailing 13-byte
# DELTADCOF wrapper is ignored — it's metadata for the stock flasher only):
cp.b 0x01000000 0xf9000000 0x1000000

# Verify byte-for-byte:
cmp.b 0x01000000 0xf9000000 0x1000000
# Expect: "Total of 16777216 bytes were the same"
# Any mismatch — STOP, don't reset. Re-check the loaded file, retry.
```

If `cmp.b` is clean:

```
reset
```

You should see the kernel boot through, `Mounted root (jffs2 filesystem) on device 31:5`, then normal stock init. After 30-60 s the bench will be at a normal idle state.

---

## 5. Why this works (and what to watch for)

- We bypass `/root/main`'s flash routine entirely. Our `cp.b` writes the same bytes the stock flasher would write, just without the half-flash bug that bricked us in the first place.
- The 13-byte `DELTADCOF` trailer is for the *stock* flasher to verify integrity. U-Boot doesn't check it; we just ignore the last 13 bytes of the loaded file by copying only 16 MiB (`0x1000000`).
- The `--eraseblock=0x10000` setting in `build-dcofimage.sh` MUST match the device's hardware sector size. If you generate a fresh recovery image with a wrong eraseblock, this procedure will re-brick. The current builder is correct; verify if you change anything in there.
- U-Boot's NOR flash driver does the per-sector program internally on `cp.b` to flash addresses; we don't need a separate `nandwrite`. This is parallel NOR, not NAND, despite the `nand` command being in the U-Boot help.

## 6. Known U-Boot quirks on this platform

- **`usb reset` fails on warm boots** with `EHCI fail to reset / Error, couldn't init Lowlevel part`. Workaround: full power-cycle. USB enumerates fine on cold start.
- **Default env CRC is bad** (`*** Warning - bad CRC, using default environment`). U-Boot just uses defaults. Don't bother with `saveenv` unless you have a specific reason.
- **Autoboot fails-back to the U-Boot prompt** automatically when no DELTADCOK is on the stick (the autoboot recovery path is for kernel updates, not filesystem). So if you're worried about missing the autoboot interrupt, just put a stick in without `DELTADCOK` — autoboot will fail, prompt will appear, no rush.
- **`fatls usb 0` defaults to partition 1**, not the whole device. Partition-less sticks (mkfs.vfat -I) will not work in U-Boot.

## 7. What's NOT recoverable via this procedure

If the kernel (mtd2) is corrupt, U-Boot's `bootm 0xf8050000` will fail before reaching userland. Recovery for that is also possible via U-Boot — load a kernel uImage into RAM via `fatload usb` and either `bootm` it directly (one-shot) or `erase` + `cp.b` to mtd2 first. We haven't needed this yet.

If U-Boot itself (mtd1) is corrupt, you're in SoC-bootrom territory — JTAG only.

## Cross-refs

- docs/22 §"Bench-validation results" — first-attempt brick + lessons learned
- docs/03 — DELTADCOF format spec (header + checksum trailer)
- companion/image/build-dcofimage.sh — image builder with the eraseblock fix
