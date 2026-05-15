# dropbear for the Delta EVMU30

`build-dcofimage.sh --dropbear <binary>` injects an SSH daemon into the
distribution image. dropbear is *not* built here — provision the binary one of
two ways.

## Option A — lift it from the AC Mini Plus firmware (preferred, try first)

The Delta AC Mini Plus firmware (V02.0B.06) ships a `dropbear` binary built for
the same SoC (SPEAr320), the same 2.6.37.6 kernel family, and the same
STLinux/glibc-2.10 toolchain era. It very likely runs on our older EVMU3015
as-is.

1. Unpack the V02.0B.06 `DcoFImage` rootfs (see the firmware-comparison notes).
2. Extract `/sbin/dropbear` and `/usr/bin/dropbearkey`.
3. Bench-test: `cp` it to the unit's `/Storage`, run `dropbear -s -r <hostkey>`,
   confirm an SSH login works.
4. If it runs, commit nothing binary to the repo — keep the binary out of git;
   document the V02.0B.06 source here and pass it via `--dropbear`.

## Option B — cross-compile (fallback, only if Option A fails)

Build dropbear with the musl armv5te toolchain (same `CROSS` prefix as the
bridge). Static, `--disable-zlib`, password auth disabled at build time.

## Runtime posture (enforced by build-dcofimage.sh)

- Key-only auth (`dropbear -s`) — no password login. The stock `vern` account
  password is unknown anyway.
- Host keys generated at **first boot** onto `/Storage/dropbear/` so they
  survive a rootfs re-flash.
- `authorized_keys` supplied to the image builder via `--authorized-key`.
