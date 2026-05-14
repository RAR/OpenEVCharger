# mtd4-sharemem.bin — Decoded Snapshot

Decoded 2026-05-14 from the live 256 KiB shared-memory snapshot
(`dump/mtd4-sharemem.bin`) using the offset map in
`02-IPC-and-main-architecture.md`. Decoder: `decode_sharemem.py`.

## Segment structure

The 256 KiB segment holds **three identical copies** of a ~44 KiB config
blob (redundancy / corruption recovery):

| Copy | Base | Notes |
|---|---|---|
| A | `0x00000` | half-0 primary |
| B | `0x10000` | half-0 mirror |
| (gap) | `0x20000` | all zero in this snapshot |
| C | `0x30000` | half-1 primary |

Checksums: BE-u32 at `+0x1FFFC` of each 128 KiB half.
- Half 0 (`0x1FFFC`): stored `0x5596`, computed `0xac17` → **MISMATCH**
  (expected — segment was live/being-modified at dump time)
- Half 1 (`0x3FFFC`): stored `0x55b1` → **OK**

At boot `FlashToShrMem()` would prefer the valid half. The mismatch is
not a fault — it just means a daemon wrote to half 0 since the last
`StoreFlash()`.

## Network config — stored TWICE, two formats

**Binary form** (IP bytes stored **reversed / little-endian**):
| Offset | Field | Value |
|---|---|---|
| `0x10a` | IP address | `192.168.100.10` |
| `0x158` | SNMP trap mgr IP | `192.168.100.1` |
| `0x190` | SNTP server IP | `192.168.100.1` |
| `0x12c`,`0x194`,`0x7c2` | (gateway/dns mirrors) | `192.168.100.1` |

**ASCII string form** (around `0xa2b`):
- IP: `192.168.100.10`
- Netmask: `255.255.255.0`
- Gateway: **`102.168.100.1`**  ← the "102" is a REAL factory-data typo,
  not a display artifact. Confirmed in the binary AND in the live
  `DownloadConfiguration` dump. Should be `192.168.100.1`. Every Delta
  AC Mini of this vintage likely ships this broken default gateway.
  (Harmless when DHCP is used.)

## OCPP / config block (`0x400`)

| Offset | Field | Value |
|---|---|---|
| `0x400` | OCPP Charge Box Id | `Delta Charging Station` |
| `0x440` | OCPP Charge Point Model | `Delta Charging Station` |
| `0x460` | (flag) | `0` |
| `0x480` | Charge Point Location | `Location` (default) |
| `0x4c0` `0x4e0` `0x500` | Server URL / user / etc | `0` (unconfigured) |
| `0x520` `0x540` | Charge Point Vendor | `Delta` |
| `0x5c0` `0x5e0` | (flags) | `0` |

## 🔓 SNMPv3 credentials (`0x560`) — factory default, weak

| Offset | Field | Value |
|---|---|---|
| `0x560` | Security level | `authPriv` |
| `0x570` | Auth protocol | `MD5` |
| `0x578` | Auth passphrase | **`password`** |
| `0x598` | Privacy protocol | `DES` |
| `0x5a0` | Privacy passphrase | **`password`** |

SNMPv3 is `authPriv` but both passphrases are literally `password`.
Agent port `161`, trap port `162` (ASCII at `0x1f4`/`0x1fc`).

## Wi-Fi credentials (`0x700`) — cleartext

- SSID: `STTest`
- PSK: `123456789`

(Factory test AP — matches `wpa_supplicant.conf`.)

## Misc config

- `0x2f6` timezone: `+00:00` (never localized)
- `0x100` UPDATE_IP flag: `0` (no pending change)
- `0x138..0x157` trap-alarm bitmap: **all zero** — no latched faults
  (factory-fresh, never had a real fault)
- `0x1d3/0x1d4` serial bytes: `00 00` — the unit serial
  (`JCF164800030WE`) is NOT in shmem; it lives in `/Storage/SerialNumber`
- `0xa24` IRMS byte: `0x1e` (30) — matches the 30 A rating

## /Storage state files (cross-referenced)

| File | Content | Meaning |
|---|---|---|
| `Gain` | `Vgain:342 / Igain:557 / Wgain:3199` | **Per-unit meter calibration** — voltage/current/power gain constants. CRITICAL: must survive any board swap or reflash or metering accuracy is lost. NOT mirrored verbatim into shmem — separate ASCII file. |
| `SerialNumber` | `JCF164800030WE` | Unit serial |
| `Energy` | `100.06` | Lifetime energy, kWh (accumulated during 2017 factory test) |
| `PassTime` | `84362` | Elapsed-time counter (units TBD — likely charging seconds or uptime) |
| `LogCount` | `131` | Event log entry count |
| `DownloadConfiguration` | (full config) | Regenerated on every USB config-dump button press |

## Region `0xc0..0xe0` (32 bytes) — unidentified numeric state

```
00 00 00 00  00 20 00 01  0b b8 00 64  8f a0 44 e2
41 f4 00 02  8b 86 8b ea  95 7c 00 00  00 00 00 00
```
Contains `0x0bb8`=3000, `0x0064`=100, and `0x41f4...` (≈30.5 as a BE
float prefix). Does NOT match the `Gain` file values (342/557/3199), so
it is not the calibration block — likely live metering accumulators or
a derived-stats region. Needs dynamic observation to pin down.

## Practical takeaways

1. **Config block layout is now a known contract** — replacement
   firmware can read/write the same offsets and stay compatible with
   Delta's daemons.
2. **The `Gain` calibration file is the one irreplaceable per-unit
   artifact.** Back it up separately; never lose it.
3. **Two latent security findings**: SNMPv3 `password`/`password`, and
   Wi-Fi PSK in cleartext in a world-readable shmem segment + flash
   partition. Expected for 2016 industrial gear; note them.
4. **The `102.168.100.1` gateway typo is genuine factory data** — useful
   as a fingerprint for identifying sibling Delta units, and worth
   fixing if anyone ever runs this unit on a static IP.
5. **Snapshot is pristine**: zero alarms, all-default config, server URL
   empty. Confirms the unit was factory-tested and never field-deployed.
