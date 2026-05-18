# DeltaEVSEConfig — USB stick config-import RE

**Date:** 2026-05-18
**Status:** Static RE complete from stock `/root/main` (1.1 MB, debug-symboled).
**Source:** `/home/rar/device-configs/esphome/testcharger/delta/dump/rootfs-unpacked/root/main`

The stock charger ships with a "drop a config file on a USB stick, charger picks it up" provisioning path. We keep `/root/main` stock in M12 (see docs/22), so this whole feature **still works post-flash** — useful for field provisioning Wi-Fi/OCPP without a serial console. This doc is the canonical key list + trigger semantics.

---

## TL;DR

| Item | Value |
|------|-------|
| File path on USB | `/UsbFlash/DeltaEVSEConfig` |
| Format | UTF-8 text, one `Key: value` per line, colon-separated |
| Trigger | Polled every ~1 s by `main`'s top-level loop |
| Mount source | `/dev/sda` (preferred) or `/dev/sda1` (fallback) |
| Mount point | `/UsbFlash` |
| Reader | `GetConfig()` at `0xd394` in `/root/main` |
| Caller | `main()` at `0x1681c` (inside the polled loop) |
| Cleanup | Config file is **not** deleted post-import |
| Audit trail | `/Storage/DownloadConfiguration` (snapshot) + `/Storage/EncodeLogMessage` (diffs) |
| Applies to | shmem-backed config struct → flushed to `/dev/mtdblock4` by `StoreFlash` |

---

## 1. Trigger semantics — polled, not hotplug

Confirmed by disassembly of `main` (`0x149e8 .. 0x17108`). The end of `main` is an unconditional backward branch (`b 160d8` at `0x17104`), so `main` is a long `while(1)` with a `sleep(1)` at the bottom. The GetConfig path sits inside this loop:

```text
0x16790  access("/dev/sda",  R_OK) -> [fp-116]
0x167b4  access("/dev/sda1", R_OK) -> [fp-48]   ; only if /dev/sda missing
0x167c4  if both missing -> branch past USB block (0x16e38)
0x167f4  system("...")                          ; mkdir/mount commands
0x16808  system("mount /dev/sda  /UsbFlash")    ; or
0x16814  system("mount /dev/sda1 /UsbFlash")
0x1681c  bl  GetConfig                          ; <-- the actual import
```

**Implications:**
- A USB stick inserted at *any time* (not just boot) gets picked up on the next loop iteration.
- The DeltaEVSEConfig file is **not removed** after import — leaving the stick in means the values are re-applied every loop tick. The EncodeLogMessage diff log only grows when something actually changes, so re-applying identical values is silent.
- For one-shot provisioning, the operator must unplug the stick after the first apply (or write the file once and rely on the no-op re-apply).

---

## 2. Recognized keys (canonical, all 36)

Extracted by `strings root/main | grep '^[A-Za-z0-9 ]\+:$'`, then audited against rodata xrefs (some matches like `Mask:` turned out to be from `ifconfig` parsing — those are excluded).

### Wi-Fi (9 keys)

| Key | Value type | Notes |
|---|---|---|
| `SSID:` | string | |
| `PWD:` | string | WPA passphrase |
| `WiFi DHCP:` | int (0/1) | 1 = DHCP, 0 = static IP (see next group) |
| `WiFi Prorocol:` | int | **(sic — firmware typo)** |
| `Wifi Encryption:` | int | |
| `Wifi Authentication:` | int | |
| `Wifi Idientity:` | string | **(sic)** EAP identity |
| `Wifi Phase1:` | string | EAP phase-1 |
| `Wifi Phase2:` | string | EAP phase-2 |

### Static IP (3 keys, only effective when `WiFi DHCP: 0`)

| Key | Value type |
|---|---|
| `IP address:` | string (dotted quad) |
| `Netmask:` | string (dotted quad) |
| `Gateway:` | string (dotted quad) |

### 3G / cellular (2 keys)

| Key | Value type |
|---|---|
| `3G Network Operator:` | string |
| `3G APN:` | string |

### VPN (4 keys)

| Key | Value type |
|---|---|
| `VPN Server IP:` | string |
| `VPN Name:` | string |
| `VPN Pwd:` | string |
| `VPN Psk:` | string |

### Charging (6 keys)

| Key | Value type | Notes |
|---|---|---|
| `Max Charging current:` | int (amps) | **Clamped to ≤ 30** by main's boot-init (see docs/07 §1.1) |
| `Max Charging time:` | int (minutes, presumed) | |
| `Backend System:` | int (0..3) | Selects backend (0 = none/local, others = OCPP/proprietary variants) |
| `Offline Charge:` | int (0/1) | Allow charge when CSMS unreachable |
| `Remote Control Charge:` | int (0/1) | Allow CSMS RemoteStartTransaction |
| `Authentication Mode:` | int | RFID vs auto-start vs plug-and-charge etc. |

### OCPP (7 keys)

| Key | Value type |
|---|---|
| `OCPP Charge Box ID:` | string |
| `OCPP Charge Point Model:` | string |
| `OCPP Server URL:` | string (e.g. `ws://...:8887/ocpp16/`) |
| `OCPP User ID:` | string |
| `OCPP Security:` | int (0/1) | |
| `OCPP local list:` | int (0/1) | |
| `OCPP offline policy:` | int | |

### SNMP (3 keys)

| Key | Value type |
|---|---|
| `SNMP Trap Receiver:` | string (IP or hostname) |
| `SNMP Port:` | int |
| `Trap Port:` | int |

### Admin / command-style (2 keys)

These don't store a value into the config struct — they trigger a side effect when present:

| Key | Effect |
|---|---|
| `Reset Charger Time:` | Resets the per-charger cumulative-time counter |
| `Reset To Factory:` | Wipes `/Storage/` config + reverts shmem to ScenarioMaker defaults |

### Time (1 key)

| Key | Value type | Effect |
|---|---|---|
| `RTC setting time:` | string (date/time) | One-shot RTC set (used as a manual NTP substitute) |

---

## 3. Output destinations

Three places get updated on every successful GetConfig:

### 3.1 In-memory shmem
`/mnt/ShareMemory.bin` (the 256 KiB shared region, key `0x153E`). Each setting writes to a fixed offset in the global config struct — for example `Max Charging current` writes to `shmem[0x0a24]` (see docs/07). The shmem region is the live source-of-truth for everything downstream (Pri_Comm, Charging_Standard_RFID, snmpd, etc.).

### 3.2 Persisted to flash
Every config change calls `StoreFlash` which copies the shmem config block into `/dev/mtdblock4`. This is what makes a USB-imported config survive reboot — *not* the file on the stick itself.

### 3.3 Human-readable audit
- **`/Storage/DownloadConfiguration`** — full snapshot, rewritten each import. Format:
  ```
  ***** 2026.05.18 - 14:32:01 *****
  Authentication Mode: 2
  Max Charging current: 30
  Max Charging time: 0
  WiFi SSID: bench-net
  ...
  ```
- **`/Storage/EncodeLogMessage`** — append-only delta log. One line per *changed* key:
  ```
  00123 - 2026.05.18 14:32:01 - 02 - Configure by USB
   SSID: old-net => bench-net
   PWD: oldpass => newpass
   Max Charging current: 16 => 30
  ```

A USB stick re-insert with identical values won't add anything to the delta log; the snapshot file gets rewritten unconditionally.

---

## 4. What this does *not* touch

These persist independently and are *not* exposed via DeltaEVSEConfig:

- **`/Storage/Gain`** — per-unit metering calibration (Vgain/Igain/Wgain). Lives in a separate text file owned by stock metering daemons, not the config struct.
- **`/Storage/delta-bridge.conf`** — our M12 companion config. See §5 for a proposed mirror.
- **`/Storage/EncodeLogMessage`** itself — log file, not a config.
- Anything in `/etc/` — read-only JFFS2, only changeable by reflash.
- Per-connector state (`user_state`, `rated_amps` deltas applied at runtime, etc.) — see docs/07 for those write paths.

---

## 5. Companion implication for M12

`/root/main` stays stock in M12, so DeltaEVSEConfig provisioning **works out of the box** on a flashed unit — operators can ship a USB stick with Wi-Fi + OCPP creds and the charger applies them on first boot.

What's **missing** is a parallel hook for our `delta-bridge.conf` (MQTT broker, RFID topic, web auth, meter scale). Today the operator has to scp/TFTP-edit `/Storage/delta-bridge.conf` by hand after flash.

Symmetric design (deferred, not in M12 baseline):
- `first-boot.sh` (or a new `usb-config.sh` invoked from `/etc/funs`) checks for `/UsbFlash/delta-bridge.conf` and atomically copies it to `/Storage/delta-bridge.conf` if newer.
- Same one-shot semantics as Delta: don't delete the source file; rely on operator unplug.
- Optional matching dump-out path (`/UsbFlash/delta-bridge-snapshot.conf`) for symmetry with `/Storage/DownloadConfiguration`.

Tracked separately; not blocking M12.

---

## 6. Bidirectional USB usage (for completeness)

`/root/main` also reads/writes these other USB paths (relevant when designing the M12 USB hook so we don't collide):

| Path | Direction | Purpose |
|---|---|---|
| `/UsbFlash/DeltaEVSEConfig` | read | This doc |
| `/UsbFlash/ACmini_Primary.bin` | read | STM32 secondary-MCU firmware update payload |
| `/UsbFlash/DcoFImage` | read | Full filesystem image (the M12 format — see docs/22) |
| `/UsbFlash/DcoKImage` | read | Kernel image |
| `/UsbFlash/Log_<serial>_<date>_V<ver>.csv` | write | Diagnostic log dump on USB insert |
| `/UsbFlash/Configuration_<serial>_<date>_V<ver>` | write | Current config snapshot on USB insert |

The auto-dump-on-insert behavior is independent of DeltaEVSEConfig — every USB insert pulls a log/config snapshot off to the stick, even when no DeltaEVSEConfig is present. Convenient for field diagnostics.

---

## References

- `/root/main` GetConfig: `0xd394` (function), call site `0x1681c` (inside main loop)
- Output strings: rodata `0xfb00..0x10c00` region of `/root/main`
- Related project docs: docs/02 (IPC + main architecture), docs/07 (persistence paths), docs/22 (M12 DcoFImage)
