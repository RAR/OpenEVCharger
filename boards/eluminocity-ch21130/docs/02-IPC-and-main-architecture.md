# Delta EVMU Shared-Memory IPC + `main` Daemon Architecture

## TL;DR

* The Delta EVMU30 firmware runs **all** of its userspace coordination through a
  single SysV shared memory segment created with `shmget(MeterSMKey, MeterSMSize, 0777)`.
* `MeterSMKey = 0x0000153E` (5438 decimal), `MeterSMSize = 0x00040000` (256 KiB).
  The segment is **byte-addressed**, not a struct — every daemon hard-codes
  numeric offsets that line up with `#define SHRMEM_FOO N` macros in the
  unshipped `define.h`.
* The 256 KiB segment is mirrored 1:1 on **`/dev/mtdblock4`** (256 KiB NOR
  partition labelled "sharemem"). `main` rehydrates it on boot via
  `FlashToShrMem()` and rewrites it via `StoreFlash()` whenever config changes.
  We confirmed this by matching the `mtd4-sharemem.bin` extract size to
  `MeterSMSize`.
* `main` is the supervisor: it forks
  `Charging_Standard_RFID`, `LED_control`, `RFID`, `Pri_Comm`, `FlashLog`,
  `DeltaOCPP`. The boot ramp (`/etc/funs`) separately launches
  `RTC`, `Adc`, `MeterIC_new`, `snmpd`, `ErrorHandle` before `main`. The
  external watchdog `/sbin/watchdog -t 30 -T 120 /dev/watchdog` is started
  with the others in `funs`, **not** by `main`.
* The 32-byte trap-alarm bitmap lives at SHRMEM offset **`0x138..0x157`**
  (= `SHRMEM_TRAP_ALARM`). `ErrorHandle` scans it and sends SNMP traps; every
  daemon ORs bits into byte 31 (`+0x157`) for self-reported faults. A
  separate, longer per-bit fault catalog (31 alarms, RCD/OVP/UVP/RA_*/GMI/…)
  comes from the safety MCU over `/dev/ttyAMA1`, parsed by `Pri_Comm`.
* The kernel/rootfs firmware update flow we can confirm runs from a USB
  stick (`/UsbFlash/DcoKImage` → `/dev/mtdblock2`,
  `/UsbFlash/DcoFImage` → `/dev/mtdblock5`) via `main`'s `UpdateCSU()` —
  this path is reachable independently of the web CGI's
  `SHRMEM_UPDATE_FIRMWARE` flag. The web-CGI / SNMP-TFTP path lands files
  in `/mnt/uImage` and `/mnt/rootfs_nor.img`, then hands them to **`FWMaker`**
  (separate binary), which prepends the `DELTADCOK` / `DELTADCOF` magic
  headers and writes `/mnt/DcoKImage` / `/mnt/DcoFImage`. We did **not** find
  a binary that polls `SHRMEM_UPDATE_FIRMWARE` and re-runs `UpdateCSU`; in
  stock firmware that flag may rely on FWMaker being invoked manually or
  may be dead.

---

## Shared memory segment

| Field          | Value           | Evidence |
|----------------|-----------------|----------|
| `MeterSMKey`   | `0x0000153E`    | `main` 0x14a78: `ldr r0, [pc, #284]` → literal at 0x14b9c = `0x0000153e`; first arg to `shmget@plt`. |
| `MeterSMSize`  | `0x00040000` (262 144) | `main` 0x14a7c: `mov r1, #262144 @ 0x40000`; second arg to `shmget@plt`. Matches the dumped `mtd4-sharemem.bin` size exactly (262 144 B). |
| `shmflg`       | `0x000003FF`    | `main` 0x14a80: literal at 0x14ba0; third arg to `shmget`. Equals `01777` — SHM_R\|SHM_W for owner/group/world; no IPC_CREAT (segment is created elsewhere — likely by the kernel sharemem driver or by `main` running first per `/etc/funs`). |
| `MeterSMPtr`   | `BSS @ 0x00022714` (in `main`) | `main` 0x14ab8: `ldr r3, [pc, #332]` → literal at 0x14c0c = `0x00022714`; immediately after `shmat`, `str r2, [r3]` parks the returned pointer. Every other daemon stores it in its own BSS at a different address (table below). |

Per-daemon `MeterSMPtr` global address (all from the post-`shmat` `str`
literal-pool entry):

| Daemon                  | Global addr  |
|-------------------------|--------------|
| `main`                  | 0x00022714 |
| `ErrorHandle`           | 0x00013cf4 |
| `Charging_Standard`     | 0x00017238 |
| `Charging_Standard_RFID`| 0x00019434 |
| `LED_control`           | 0x00011628 |
| `RFID`                  | 0x00012360 |
| `FlashLog`              | 0x00011200 |
| `MeterIC_new`           | 0x00014c8c |
| `Adc`                   | 0x00012dc8 |
| `NTC_tmp`               | 0x000158a4 |
| `RTC`                   | 0x0001172c |
| `Pri_Comm`              | 0x0001620c |

(See raw extracts in `02-shrmem-offsets-from-all-binaries.txt` and
`02-shrmem-rolemap.txt`.)

> **Note** — `NTC_tmp` is **not** in `/etc/funs`. By string and code shape
> it is a near-duplicate of `Pri_Comm` (same 31-alarm string table, same
> `/dev/ttyAMA1` UART path, same `/mnt/PrimaryFW`). It's probably a dev /
> debug variant left on the rootfs. The running stack uses `Pri_Comm`.

### Backing store on flash

`FlashToShrMem(MeterSMPtr)` (at `0x00008f80`, called once from `main` at
0x14b68) opens `/dev/mtdblock4` (literal `0x00017b00`), reads the entire
256 KiB partition in two 128 KiB halves into a malloc buffer, validates a
32-bit big-endian checksum stored at offsets `0xFFFC..0xFFFF` of each half,
and memcpy's into the segment.  If either half's checksum mismatches, it
re-reads up to twice and then degrades:

1. ORs `SHRMEM_TRAP_ALARM[31] |= 0x20` ("CSU memory error"), then `|= 0x40`
   ("Initial error"). (Code at 0x8fb4–0x9008.)
2. Executes `system("cd /root;./ScenarioMaker")` (literal at 0x17b10)
   followed by `system("reboot -f")` (literal at 0x17b2c) to recreate the
   default config blob and warm-reboot.

This is how we know the segment offsets `0xFFFC..0xFFFF` are a checksum
trailer and **not** part of the live IPC layout (they shouldn't be touched
by daemons at runtime).

### Initialised defaults (BSS hydration)

After `FlashToShrMem`, `main` initialises a config block via a series of
23-byte / 9-byte / 2-byte `memcpy`s from `.rodata` defaults (code at
0x14c14–0x14d80). These hydrate offsets `0x400` (Charge Box Id),
`0x440` (Charge Point Model), `0x460`, `0x480` (Server URL), `0x4C0`,
`0x4E0`, `0x500`, …  Defaults loaded:

| .rodata addr | String                  | SHRMEM dest |
|--------------|-------------------------|-------------|
| 0x19d7c      | `"Delta Charging Station"` | offset 0x400 (23 B) |
| 0x19d94      | `"0"`                    | offset 0x460 (2 B) |
| 0x19d98      | `"Location"`             | offset 0x440 (23 B) — *no, this is OCPP "Location"; see source order* |
| 0x19da4      | `"Delta"`                | offset 0x480 (9 B) — Charge Point Vendor |
| ...          | ...                      | ... |
| `0xC0A8010A` (192.168.1.10) | static IP default | …  |
| `0xC0A801FE` (192.168.1.254) | static gateway default | … |
| `0xC0A80101` (192.168.1.1)  | static DNS / gateway? | … |

(The exact mapping of each default to its offset can be reproduced
mechanically from disassembly of `main+0x22c..0x398`; we list it here only
to anchor that the 0x400..0x720 cluster is the **OCPP / network
configuration block**.)

---

## Complete SHRMEM offset map (from all daemons)

The full per-binary access tables are in:

* `/home/rar/device-configs/esphome/testcharger/delta/docs/02-shrmem-offsets-from-all-binaries.txt`
* `/home/rar/device-configs/esphome/testcharger/delta/docs/02-shrmem-rolemap.txt`

The condensed map below groups offsets by their inferred meaning. **R** =
binary contains a read of that byte, **W** = contains a write, **RW** =
both. "Daemons" lists which binaries touch the field. Symbol names that
appear in the CGI (`SHRMEM_*` macros) are listed verbatim; the others are
educated guesses based on read/write patterns and surrounding code.

### Inferred macro names (cross-checked against `www/cgi-bin/*.c`)

| Likely symbol                  | Offset | Size | Confidence | Evidence |
|--------------------------------|--------|------|------------|----------|
| `SHRMEM_PRIMARY_FW_SENSOR_A`   | 0x000  | 2 B BE | medium | Pri_Comm 0xb3a0–0xb3c0 reads bytes 0,1 as BE u16, stores to a sensor global. Only Pri_Comm & NTC_tmp touch. |
| `SHRMEM_PRIMARY_FW_SENSOR_B`   | 0x004  | 2 B BE | medium | Pri_Comm 0xb3d0–0xb3f4 reads bytes 4,5 as BE u16. |
| `SHRMEM_LCD_BACKLIGHT`         | 0x091..0x093 | 3 B | low | `main` writes only; `LCDBlackLight()` is at 0x9478. Possibly RGB or PWM duty bytes. |
| `SHRMEM_UPDATE_IP_ADDRESS`     | 0x100  | 1 B  | **HIGH** | Management.c sets `*(MeterSMPtr+SHRMEM_UPDATE_IP_ADDRESS)=0x01`. `main` polls byte 0x100 at 0x160e0 (`add r3, #256; ldrb; cmp #0; bne …`). |
| `SHRMEM_IP_ADDRESS`            | 0x101  | 4 B  | **HIGH** | Management.c writes bytes IP, IP+1, IP+2, IP+3. `main` initialises 0x101..0x104 (offsets 0x102/0x103/0x104 visible in role-map). |
| `SHRMEM_MASK_ADDRESS`          | 0x105  | 4 B  | **HIGH** | Same idiom in Management.c, contiguous after IP. |
| `SHRMEM_GATEWAY_ADDRESS`       | 0x109  | 4 B  | **HIGH** | Same idiom; we see ref at 0x109 (refs=5) — only the first byte is hit by a single load; the next three are inside `memcpy` and don't surface in our scanner. |
| `SHRMEM_DNS_ADDRESS` (guess)   | 0x10d  | 4 B  | low    | Implied by the symmetry of the 0x100..0x110 block; not directly confirmed. |
| `SHRMEM_WIFI_FLAGS_*`          | 0x116..0x140 | mixed | medium | `main` reads byte 0x116, 0x117 (booleans); `main` reads/writes 0x139, 0x13b, 0x13c, 0x140 — used by `GetWlan0Info`, `WiFiAPConnection`, `WiFiRSSI`. |
| `SHRMEM_TRAP_ALARM`            | 0x138  | 32 B | **HIGH** | See dedicated section below. |
| `SHRMEM_SNMP_TRAP_MGR_IP`      | 0x158  | 4 B BE | **HIGH** | `ErrorHandle` 0x86f0–0x873c reads bytes 0x158..0x15b shifted into a 32-bit BE word — the trap-receiver IP. `main` writes them (refs at 0x158..0x15b in the role-map). |
| `SHRMEM_SNMP_TRAP_PORT`        | 0x15c  | 2 B   | low | Inferred by symmetry with SNMP IP; not directly hit in our offset table. |
| `SHRMEM_SNTP_SERVER_IP`        | 0x190  | 4 B BE | medium | `Ntpdate()` at 0x9e24 reads 0x190..0x193 BE-assembled into an IP, then formats `"/root/ntpdate %s"`. |
| `SHRMEM_HMI_CMD_BUF`           | 0x198..0x19a | 3 B | low  | Charging_Standard / Charging_Standard_RFID / RFID RW byte 0x198, 0x199, 0x19a. `HMIRecvACK` checks 0x1da bit pattern; likely HMI command/ack registers. |
| `SHRMEM_AUTH_FLAGS_*`          | 0x1ba..0x1c5 | 11 B | medium | mix of R/W flags read by Charging_Standard_RFID / RFID / FlashLog. Includes authentication mode, "offline charge", etc. (see `EncodeLogMessage` formatters in main strings.) |
| `SHRMEM_STORE_FLASH_BUSY`      | 0x1dd  | 1 B | **HIGH** | `StoreFlash()` at 0xc074 polls byte 0x1dd; if `==1` it waits, else atomically sets 1, allocates 128 KiB, writes back to `/dev/mtdblock4`. Used by main, Charging_Standard, RFID, FlashLog, MeterIC_new. |
| `SHRMEM_HMI_ACK_FLAGS`         | 0x1de  | 1 B | medium | HMIRecvACK 0xa384 reads bit `0x81` (bit 0 \| bit 7) and acts on it. |
| `SHRMEM_BACKEND_MODE` (guess)  | 0x1e1, 0x202 | 1 B each | low | main reads them as scalar config bytes. |
| Update-firmware flag (`SHRMEM_UPDATE_FIRMWARE`) | **uncertain** | 1 B | LOW | KernelUp.c/RootfsUp.c set it after writing /mnt/uImage / /mnt/rootfs_nor.img. **No daemon in our extraction reads it directly to perform an MTD-write.** It is most likely *one of* 0x1c2..0x1c5 or 0x202 — exact byte cannot be pinned without the `define.h` source. |
| `SHRMEM_UPDATE_HMI_FIRMWARE`   | **uncertain** | 1 B | LOW | HmiUp.c sets it after writing /mnt/HMI_FW. No daemon in our extraction reads it. The HMI firmware is on the STM32F334 → would be flashed via `Pri_Comm` over `/dev/ttyAMA1`, but `Pri_Comm` does not reference `/mnt/HMI_FW`. |
| `SHRMEM_SERIAL_NUMBER`         | 0x1d3, 0x1d4 | 2 B  | medium | UpdateCSU at 0x14328–0x14338 reads `MeterSMPtr+#464+#3` (= 0x1d3) and `+#468` (= 0x1d4), assembles into a 16-bit unit serial, then uses it in a `sprintf` for log filenames. Also written by Charging_Standard, RTC, Pri_Comm. |
| `SHRMEM_RTC_*` (year/month/day/h/m/s) | 0x1d5..0x1da | 6 B | medium | RTC daemon RW these bytes; Charging_Standard reads. Order/decoding requires more disasm. |
| `SHRMEM_PWR_METER_BLOCK`       | 0x800  | 1 B (start of a block) | medium | main writes byte 0x800 only; the cluster 0x800..0xc8e holds meter readings + per-connector state. |
| `SHRMEM_CONNECTOR1_STATE`      | 0xa00..0xa0b | 12 B | medium | Charging_Standard / LED_control / FlashLog / MeterIC_new / Pri_Comm cluster: 0xa00 (state byte), 0xa07 (fault flags?), 0xa08 (heartbeat), 0xa0b (`Pri_Comm` ORs 0x10 here — "device link OK"?). |
| `SHRMEM_CONNECTOR1_VRMS`       | 0xa10  | 2 B  | medium | Pri_Comm 0x9118 reads byte 0xa10 then `strh` to a global; this is one byte but the surrounding code treats it as a u16 path → likely an upstream MCU register echoed into shmem. |
| `SHRMEM_CONNECTOR1_IRMS`       | 0xa24  | 2 B  | medium | main+Charging+Adc+NTC_tmp+Pri_Comm RW; pattern similar to 0xa10. |
| `SHRMEM_RFID_TAG_BUF`          | 0xa5f, 0xa68..0xa6f | ~16 B | medium | Charging_Standard_RFID + RFID + FlashLog + MeterIC_new RW cluster. MeterIC_new writes 0xa69..0xa6f as a byte sequence (likely RFID UID bytes). |
| `SHRMEM_CONNECTOR_PWR`         | 0xa74..0xa79 | 6 B  | medium | Charging_Standard_RFID + MeterIC_new + NTC_tmp + Adc R/W cluster — power/voltage/current per-phase or per-connector. |
| `SHRMEM_LIMIT_CURRENT`         | 0xbf2..0xbf6 | 5 B | medium | main reads `Max Charging current` (per the EncodeLogMessage string); a write at 0xbf1 from main + reads at 0xbf2/0xbf3/0xbf6 from main, plus 0xbf4 from Adc, 0xbf5 from NTC_tmp and Pri_Comm. |
| `SHRMEM_OCPP_SESSION_PTR`      | 0xc14, 0xc55, 0xc8e, 0xcab, 0xcac | mixed | low | Used by main + Charging_Standard / Charging_Standard_RFID. |
| `SHRMEM_FLASHTOSHRMEM_CKSUM`   | 0xFFFC..0xFFFF | 4 B BE | **HIGH** | FlashToShrMem 0x9120–0x917c reads bytes 0xFFFC..0xFFFF as BE u32 and compares against a sum it computed earlier (sum of `[0..0xFFFC)`). Loop break at 0x9460 `.word 0x0000fffb`. |

A grid of every (offset, daemon) tuple is in `02-shrmem-rolemap.txt`.

---

## Fault bitmap (`SHRMEM_TRAP_ALARM` = offset 0x138, 32 bytes)

We have **two** independent lists of fault sources:

1. **Embedded in CGI source** (3 known bit positions):

   | Byte | Bit | Fault name              | Set by | Evidence |
   |------|-----|-------------------------|--------|----------|
   | +2 (0x13a) | 0x01 | Eth0 setting error | `Management.cgi` | `*(MeterSMPtr+SHRMEM_TRAP_ALARM+2) \|= 0x01` |
   | +31 (0x157) | 0x20 | CSU memory error  | `Management.cgi`, `FlashToShrMem` | `\|= 0x20` |
   | +31 (0x157) | 0x40 | Initial error     | `Management.cgi`, `FlashToShrMem`, `Pri_Comm` 0x9968 | `\|= 0x40` |

2. **Embedded in `Pri_Comm` `.data` as a 31-entry × 0x80-byte stride
   table**, with `<name> alarm` / `<name> alarm recovered` paired strings.
   These are the alarm conditions reported by the safety-MCU on UART1 (the
   STM32F334 over `/dev/ttyAMA1`). Indices 0..30 in order:

   | idx | Name |
   |-----|------|
   | 0 | OVP (Over-voltage) |
   | 1 | OCP (Over-current) |
   | 2 | Ambient OTP (Over-temperature) |
   | 3 | EMGSTOP (Emergency stop) |
   | 4 | RCD (Residual current) |
   | 5 | WELDING (Contactor welded) |
   | 6 | UVP (Under-voltage) |
   | 7 | RA_CPU |
   | 8 | RA_WATCHDOG |
   | 9 | RA_CLOCK |
   | 10 | RA_DATA |
   | 11 | RA_FLASH |
   | 12 | RA_RAM |
   | 13 | RA_INTERRUPT |
   | 14 | RA_TIMING |
   | 15 | RA_IO |
   | 16 | RA_ADC |
   | 17 | RCDTRIP |
   | 18 | GMI (Ground monitor isolation) |
   | 19 | PILOTERROR |
   | 20 | INITIAL |
   | 21 | Ambient NTC fail |
   | 22 | Plug OTP |
   | 23 | Plug NTC fail |
   | 24 | RCDLOCK |
   | 25 | AC drop |
   | 26 | Firmware upgrade fail |
   | 27 | PILOTERROR_Negative |
   | 28 | Relay driver fault |
   | 29 | Pri MCU Lost |
   | 30 | Wifi module fail |
   | 31 | RFID module fail |

   (Strings extracted via `readelf -p .data Pri_Comm`. The same table is
   duplicated in `NTC_tmp`'s `.data`.)

The **mapping from this 31-entry table to specific bits in the 32-byte
`SHRMEM_TRAP_ALARM` bitmap is not fully resolved by static analysis**.
`Pri_Comm` does not OR these individual flags into the 0x138..0x157 region
in a single recognisable byte-and-bit pattern; instead it stores the
raw UART status into a separate per-alarm word in `.bss` and only writes
**`0x157 |= 0x40`** ("Initial") into the trap-alarm region. The likely
correspondence — based on the CGI's hints (Eth0 at +2 bit 0, CSU memory
at +31 bit 5) and Linear bit ordering — is:

* Byte 0 (offset 0x138): J1772 / connector faults (OVP, OCP, RCD, GMI…)
* Bytes 1-2: safety-MCU self-tests (RA_*, watchdog)
* Byte 28-31 (`+0x154..0x157`): CSU-host faults (network, memory,
  initial error)

To **confirm** the exact bit ordering you would need to either find an
SNMP MIB file in the Delta firmware tree (look under `/usr/share/snmp` —
not in our current extraction), or set a single fault on a running unit
and dump the segment.

> Pri_Comm also has two non-alarm `.rodata` strings —
> `"Pilot error voltage"` and `"Pilot recover voltage"` (at 0xcdac /
> 0xcdc0) — used in `EncodeLogMessage` formatted writes; these are NOT
> in the alarm bitmap and are logged directly to
> `/Storage/EncodeLogMessage`.

---

## `main` daemon orchestration

### Function symbol map (from `nm main`)

```
0000883c T _start
000088e8 T crc32
00008b84 T CalCrc16
00008c60 T SystemBuzzer
00008cc4 T FWUP
00008d04 T ScenarioUP
00008f80 T FlashToShrMem      <- /dev/mtdblock4 → SHRMEM
00009478 T LCDBlackLight
000094ec T LEDSwitch
00009508 T GetWlan0Info
00009d10 T GetRateOfHour
00009e24 T Ntpdate
00009f60 T HMIRecvCMD
0000a23c T HMIRecvACK
0000a494 T HMISendCMD
0000a77c T RestoreFactory
0000a94c T SwitchD1D2          <- J1772 contactor switches
0000aa44 T GetD1D2
0000ab48 T GetOptoJ
0000abc4 T ConnectorLock
0000acc0 T SwitchSS
0000af54 T CriticalError       <- handler for fatal SHRMEM flags
0000b8b0 T SystemTest
0000bec4 T CardVerify
0000bf78 T PowerCard
0000c074 T StoreFlash          <- SHRMEM → /dev/mtdblock4
0000cef0 T SaveSystemLog
0000cf74 T GetPrimaryFW
0000d394 T GetConfig
00011514 T VerifyIdTag
0001214c T UpdateLocalList
00012684 T GetPPP0Info
00012808 T WiFiRSSI
000128c8 T PingGateway
000129e0 T WiFiAPConnection
00013680 T DownloadConfig
00014148 T UpdateCSU           <- USB-stick firmware update
000148a4 T RmLogfile
000149e8 T main
```

(BSS globals: `MeterSMPtr` (0x22714), `FWVersion`, `StartSOC`, `StopSOC`,
`StopReason`, `STime`, `OfflineTimes`, `Wifi_Flag`, `SeqNum`, `Crc16Table`,
`crc_table`, `ResetToDeault` (sic).)

### `main()` boot sequence

Located at `0x000149e8 .. 0x000170...` (≈2500 disassembly lines). Top-down:

1. **Local var init** (0x14a00–0x14a78): clear ~80 bytes of stack frame.

2. **Attach SHRMEM** (0x14a78–0x14acc):
   ```
   shmget(0x153e, 0x40000, 0x3ff)   ; key, size, perms
   if rc<0:        set error flag
   else:           MeterSMPtr = shmat(rc, NULL, 0)
   ```

3. **Hydrate from flash** (0x14ad8–0x14b6c):
   * On error, `SHRMEM_TRAP_ALARM[+0x155] = ...` (set "initial" group of
     bits) and reboot — code at 14ae4–14b14 walks a 24-byte struct
     setting initial-state markers.
   * `FlashToShrMem(MeterSMPtr)` returns 0 on hard failure → main jumps
     to a reboot path at 15bb8.

4. **Default config copy** (0x14b78–0x14d80): hydrate OCPP "Charge Box
   Id" / "Charge Point Model" / "Server URL" / "Vendor" / IP defaults
   from `.rodata` into segment offsets 0x400..0x720 (the OCPP /
   network config block). Uses 23-byte / 9-byte / 2-byte memcpys.

5. **Hardware bringup via shell** (0x14b2c–0x14b40): three back-to-back
   `system()` calls
   * `0x19d00`: `echo 87 > /sys/class/gpio/export`
   * `0x19d24`: `echo "out" > /sys/class/gpio/gpio87/direction`
   * `0x19d54`: `echo 1 > /sys/class/gpio/gpio87/value`
   These bring up an enable line (probably the main contactor enable
   relay or LCD power).

6. **CAN bus bringup** (around 0x15bxx): `system("/sbin/ip link set
   can0 ...")` series — probably for in-house diagnostic CAN.

7. **Initial WiFi state setup**: writes default static IP, mask, gateway
   (`0xC0A8010A` = 192.168.1.10, `0xC0A801FE` = 192.168.1.254,
   `0xC0A80101` = 192.168.1.1) into SHRMEM offsets 0x101–0x10c.

8. **Daemon forks** (0x15ff0–0x16070): sequence of `system()` calls,
   each a literal C string ending in `&` so the shell backgrounds them:

   | seq | system() arg | What it does |
   |-----|------|---|
   | 1 | `rm -f /mnt/CheckXXX` (path varies) | cleanup |
   | 2 | `/root/Charging_Standard_RFID &` | session controller |
   | 3 | `killall LED_control`           | (defensive) |
   | 4 | `/root/LED_control &`           | LED ring |
   | 5 | `/root/RFID &`                  | MiFare reader |
   | sleep 1 | | |
   | 6 | `/root/Pri_Comm &`              | safety-MCU bridge |
   | 7 | `/root/FlashLog &`              | persistent log |
   | 8 | `killall DeltaOCPP`             | (defensive) |
   | 9 | `/root/DeltaOCPP &`             | OCPP 1.6-J client |

   `Adc`, `MeterIC_new`, `RTC`, `snmpd`, `ErrorHandle` are **not**
   forked from main — they are started earlier by `/etc/funs` (see
   below). The external watchdog `/sbin/watchdog -t 30 -T 120
   /dev/watchdog &` is also in `/etc/funs`, **not** in `main`.

9. **Polling main loop** (≈0x16060–end): walks every connector and:
   * Reads byte 0x100 (`SHRMEM_UPDATE_IP_ADDRESS`?) – if non-zero,
     branches to network-reconfigure path at 0x161d0.
   * Reads byte 0x108, 0x116 – additional config-change flags.
   * Reads byte 0x1bd (RW, refs=12) – probably the authentication/RFID
     mode change flag.
   * Reads byte 0x1dd – Store-Flash busy flag.
   * Reads byte 0x1de bit 0x81 – HMI ACK status.
   * Reads byte 0x1da (UpdateCSU-trigger?) – if set, calls UpdateCSU.
   * Calls `WiFiAPConnection`, `WiFiRSSI`, `PingGateway` repeatedly to
     watchdog WiFi.
   * Calls `Ntpdate` if the SNTP-server IP at 0x190 is non-zero.

### Signal handling

We did not find handlers for SIGCHLD/SIGTERM in `main`. The PLT contains
no `signal@plt`, `sigaction@plt`, or `waitpid@plt`. main relies on the
**external `/sbin/watchdog`** to reboot if it hangs, and child daemons
that die are simply respawned by being killed-and-relaunched on the
next config-change cycle (note the `killall LED_control` /
`/root/LED_control &` pattern). This is fragile but matches what we see
in `ps`.

The "kicking" of the hardware watchdog (`/dev/watchdog`) is done by
`/sbin/watchdog -t 30 -T 120` — busybox watchdog refreshes every 30 s
and triggers HW-reset after 120 s of no kicks. Nothing in `main` opens
`/dev/watchdog`.

---

## Firmware update flow

### Path A — USB stick (`UpdateCSU`, in `main`)

Trigger: `main` periodically opens `/UsbFlash/DcoKImage` and
`/UsbFlash/DcoFImage` (literals `0x19ae8`, `0x19c94`); when files are
present and well-formed they are flashed.

The image format requires a magic header that `UpdateCSU` checks for
inside the file body:

* `DELTADCOK` (.rodata `0x000181b4`, actually `0x000181b4 = "DELTADCOK"`
  — but the loaded value via `ldr r1, [pc, #...] @ 0x14864` =
  `0x00019afc` decoding to "DELTADCOK") → kernel image.
* `DELTADCOF` (literal `0x00018181` = "DELTADCOF") → rootfs image.

Header structure (decoded from 0x142a4–0x142fc):
```
offset 0: "DELTADCO[K|F]"             # 9-byte magic
offset 9: u8   high byte of length    # length is u32 big-endian
offset 10: u8  ...
offset 11: u8  ...
offset 12: u8  ...
offset 13: <payload bytes>            # the actual kernel/rootfs image
```

The checksum before the magic is a **byte-sum** of the payload bytes,
compared against the BE u32 at offset 9..12.

On match:
* For DELTADCOK → open `/dev/mtdblock2` (literal `0x19b08`) for
  write+sync, write payload, close, increment serial number byte at
  SHRMEM `+0x1d4` and `+0x1d3+3` (so SerialNumber is 16-bit BE in
  these 2 bytes), `sprintf` log entry to `/Storage/EncodeLogMessage`,
  `system("/sbin/reboot -f")`.
* For DELTADCOF (rootfs) → same flow but `/dev/mtdblock5` (literal
  `0x19b48`).

The write is a single `write(fd, payload_ptr, payload_len)`. There is
**no** `flash_eraseall` or `flashcp` call — `mtdblock` device nodes
internally erase-on-write, but write-alignment to flash sector size is
the caller's responsibility. The disasm shows write sizes up to 2.5 MiB
(`#2621440 = 0x280000`) for kernel and 13.x MiB (`#0x0100000d`) for
rootfs, both passed as `malloc` sizes — i.e. the entire mtdblock
content is written at once.

### Path B — Web CGI + SNMP-TFTP (FWMaker chained)

CGI source (`KernelUp.c` / `RootfsUp.c`) writes uploaded HTTP POST body
to `/mnt/uImage` or `/mnt/rootfs_nor.img`, then sets
`*(MeterSMPtr+SHRMEM_UPDATE_FIRMWARE) = 1`. **No daemon in our
extraction polls this flag and chains to `UpdateCSU`.**

Looking at the separate `/root/FWMaker` binary (not in the daemon
list), its strings include:
* `tftp -gr uImage -l /mnt/uImage %s`
* `tftp -gr rootfs_nor.img -l /mnt/rootfs_nor.img %s`
* `DELTADCOK` → `/mnt/DcoKImage`
* `DELTADCOF` → `/mnt/DcoFImage`

So **`FWMaker`** is the tool that:
1. Pulls `/mnt/uImage` or `/mnt/rootfs_nor.img` (already deposited by
   the CGI or by `tftp -gr`).
2. Prepends the `DELTAD​CO?` magic header + length + checksum.
3. Writes `/mnt/DcoKImage` / `/mnt/DcoFImage`.

These files are then expected to be picked up by **the same UsbFlash
codepath** in `main` — but that path looks at `/UsbFlash/DcoKImage`,
**not** `/mnt/`. So either:

* There is a missing symlink / mount setup we haven't found that maps
  `/mnt/Dco?Image` into `/UsbFlash/`, OR
* FWMaker is invoked from a higher-level script we haven't extracted
  yet (look in `/usr/share/snmp/snmpd.conf` MIB extensions and SNMP-set
  handlers in `libnetsnmpmibs.so.20`).

This is the **single biggest gap** in our IPC story. To resolve it,
either trace a live SNMP `set` against the firmware-update OID and
watch which process gets exec'd, or inspect the
`libnetsnmpmibs.so.20` Delta MIB handler functions.

### HMI firmware update

The CGI's `HmiUp.c` writes `/mnt/HMI_FW` and sets
`*(MeterSMPtr+SHRMEM_UPDATE_HMI_FIRMWARE) = 1`. Neither `main` nor
`Pri_Comm` directly references `/mnt/HMI_FW`. The HMI is a separate
STM32F334 that lives behind `/dev/ttyAMA1`. The most likely flow:

1. CGI deposits `/mnt/HMI_FW` and flips the flag.
2. `main` (or a sibling daemon) reads the flag, kills `Pri_Comm`,
   runs `Pri_Comm <flag>` so the binary enters its OTP-update mode
   (we see `OTPCheck` at 0x900c with extensive `/mnt/PrimaryFW`
   handling — `/mnt/PrimaryFW` is the staging path for primary-MCU
   firmware).
3. `Pri_Comm` UART-transfers `/mnt/PrimaryFW` (renamed from
   `/mnt/HMI_FW` by the launcher?) to the STM32 over `/dev/ttyAMA1`
   using its primary-MCU bootloader protocol.

This is **inferred** from string evidence; no static-analysis proof of
the rename step. Function name `GetPrimaryFW` at main+0xcf74 is the
counterpart that **reads** the current STM32 firmware version into
SHRMEM (probably bytes 0..5 of the segment, matching the Pri_Comm
`+0x000` and `+0x004` access pattern).

---

## IP-address update flow (`SHRMEM_UPDATE_IP_ADDRESS`)

1. `Management.cgi` (`Management.c`) parses `IP=`, `Mask=`,
   `Gateway=` query-string fields, packs them into 4-byte big-endian
   integers in SHRMEM at offsets `SHRMEM_IP_ADDRESS` (0x101),
   `SHRMEM_MASK_ADDRESS` (0x105), `SHRMEM_GATEWAY_ADDRESS` (0x109),
   then sets `*(MeterSMPtr+SHRMEM_UPDATE_IP_ADDRESS) = 0x01` (byte
   0x100).

2. `main`'s polling loop reads byte 0x100 at code-address 0x160e0 (see
   the role-map: offset 0x100 is RW by main only). When non-zero, it
   branches to a path that:
   * Calls `system("killall udhcpc")`
   * Reformats `/root/wpa_supplicant.conf` from SHRMEM SSID/PWD fields
     (strings: `rm -f /root/wpa_supplicant.conf`,
     `/root/wpa_supplicant -B -i wlan0 -c /root/wpa_supplicant.conf &`).
   * Calls `system("/sbin/ifconfig wlan0 %s netmask %s")` with the
     new IP/mask read from SHRMEM.
   * **No equivalent eth0 reconfiguration.** The bind-CGI mentions only
     IP/mask/gateway updates — those are applied to wlan0. eth0
     reconfig is missing from the runtime path; eth0 is presumably
     left on DHCP via `/sbin/udhcpc -i wlan0 -s /root/simple.script`.
   * Clears byte 0x100 back to 0.

---

## Daemon dependency graph

```
                       /etc/rc
                          │
                  /etc/funs (boot ramp)
                          │
                  ┌───────┼────────┬─────────────┬──────────┐
                  ▼       ▼        ▼             ▼          ▼
              /root/main  RTC    Adc, MeterIC_new  snmpd  ErrorHandle
                  │       │      │               │       │
                  │       │      │               │       │
                  ▼ (forks)
        ┌─────────┬──────────┬──────────┬──────────┬─────────┐
        ▼         ▼          ▼          ▼          ▼         ▼
   Charging_   LED_control  RFID    Pri_Comm   FlashLog   DeltaOCPP
   Standard_
   RFID

Also at funs-time, separately:
   /sbin/watchdog -t 30 -T 120 /dev/watchdog &
```

### SHRMEM writer/reader summary

| Region | Writers | Readers | Pattern |
|--------|---------|---------|---------|
| 0x000..0x005 | Pri_Comm, NTC_tmp | (same) | Primary-MCU firmware-version mirror (16-bit BE values at +0x00 and +0x04) |
| 0x100..0x10c | main, Management.cgi | main | Network config block + update flag |
| 0x138..0x157 | every daemon (mainly OR-into +0x157 = byte 31) | ErrorHandle, main | 32-byte SNMP trap-alarm bitmap |
| 0x158..0x15b | main | ErrorHandle | SNMP trap manager IP (BE u32) |
| 0x190..0x193 | main, Management.cgi (?) | main (Ntpdate) | SNTP server IP (BE u32) |
| 0x198..0x19a | RFID, Charging_Standard{_RFID} | (same) | HMI command/ack registers |
| 0x1ba..0x1c5 | main, Charging_Standard{_RFID}, RFID, FlashLog | (same) | Authentication & charge-mode flags |
| 0x1d3..0x1da | main, Charging_Standard{_RFID}, RTC, Pri_Comm, FlashLog | (same) | Serial-number + RTC time tuple |
| 0x1dd        | main, Charging_Standard, RFID, FlashLog, MeterIC_new | (same) | StoreFlash atomic-write lock byte |
| 0x400..0x720 | main (on boot), `GetConfig` from main | OCPP daemons (`DeltaOCPP`), main | OCPP config block (ChargeBoxId, Server URL, etc.) — 32+ bytes per string |
| 0x800..0xc8e | main, MeterIC_new, Adc, NTC_tmp, Pri_Comm, Charging_Standard{_RFID} | (same) | Live metering + per-connector state |
| 0xa00..0xa79 | many (per-connector) | many | Connector-1 state, RFID tag UID, Vrms, Irms, kWh |
| 0xFFFC..0xFFFF | main (FlashToShrMem on rehydrate, StoreFlash on writeback) | main | BE-u32 checksum of bytes 0..0xFFFB |

Update cadences (qualitative — based on whether the read path is inside
the main `while(1)` polling loop versus a one-shot init):

* `MeterIC_new`: writes Vrms/Irms/power continuously (sub-second).
* `Adc`: writes ADC channels continuously.
* `Pri_Comm`: writes safety-MCU status frames each time the MCU sends
  one (it polls UART1 at high rate).
* `LED_control`: reads connector state continuously, drives LED state.
* `RFID`: writes RFID UID on tag-tap event.
* `Charging_Standard_RFID`: drives J1772 state machine; reads pilot,
  fault flags, RFID; writes connector state.
* `RTC`: writes RTC time block once per second.
* `FlashLog`: reads connector state, appends to `/Storage/EncodeLogMessage`
  on edge events.
* `main`: polls config-change flags (0x100, 0x108, 0x1bd, 0x1dd, 0x1de,
  0x1da) once per outer-loop iteration. Loop interval includes
  `sleep(1)` / `sleep(2)` calls — so flags are reacted to within 1-2 s.

---

## `main` state machine (high level)

`main` is not a tidy explicit state machine — it's a one-shot init
followed by a polling supervisor. Inferred phases:

1. **Boot**: shmget+shmat → FlashToShrMem (rehydrate) → default config
   bind → GPIO+CAN bringup → fork children → enter loop.
2. **Idle/normal poll**: each cycle, every config-change flag is
   checked. On flip, run the corresponding `system()` chain or
   `system("/sbin/reboot -f")`.
3. **CriticalError** (function at 0xaf54): called when several
   trap-alarm bits are simultaneously set; tears down children and
   reboots.
4. **FW-update**: when `UpdateCSU` returns success, reboot.
5. **Restore-to-default**: triggered by setting offset 0x1dc-related
   flag; calls `RestoreFactory` (0xa77c) which zeroes the OCPP/network
   config block and writes back via `StoreFlash`.

---

## Unknown / unresolved

1. **Exact bit-positions in `SHRMEM_TRAP_ALARM` for the 31 Pri_Comm
   alarms** — needs dynamic tracing (or the Delta SNMP MIB file —
   try extracting `/usr/share/snmp/` from the firmware if not already).
2. **Which binary polls `SHRMEM_UPDATE_FIRMWARE`** after the web-CGI
   sets it.  None of our 12 daemons reads that exact flag with a
   "read==1 → flash MTD" pattern.  Most likely a missing step where
   FWMaker is invoked manually or by SNMP set-OID. Need either a
   stock-firmware boot trace or an SNMP MIB walk of OIDs the Delta
   MIB module registers.
3. **Which byte is `SHRMEM_UPDATE_HMI_FIRMWARE`** specifically — the
   handoff to Pri_Comm's OTP-update mode (`OTPCheck` at 0x900c) is
   inferred from string + function structure, not directly proved.
4. **`SHRMEM_UPDATE_IP_ADDRESS` offset** — we strongly believe it is
   byte 0x100 (the byte before the 4-byte IP at 0x101) based on the
   read pattern in main+0x16fc. The CGI source doesn't disambiguate
   which byte it is. To confirm, one could write `1` to offset 0x100
   on a running unit and watch for the wlan0 ifconfig.
5. **NTC_tmp**: present in `/root/` but not started by `funs` or
   `main`. It is a near-twin of `Pri_Comm` — same alarm strings, same
   UART, same `/mnt/PrimaryFW`. Probably an earlier version that was
   left on the rootfs. Confirm by running an `strace` on the live unit.
6. **The 23-byte OCPP "ChargeBoxId" / "Model" / "Vendor" / "Server URL"
   default copies in main 0x14c14..0x14d80** — we've shown they hydrate
   SHRMEM offsets 0x400, 0x440, 0x460, 0x480, 0x4C0, 0x4E0, 0x500 — but
   the exact mapping per offset to "field" needs cross-reference with
   `DeltaOCPP`'s SHRMEM-read map (not analysed here; that binary is
   730 KB and the OCPP-side analysis is doc 03's territory).
7. **DNS-server SHRMEM offset** (probably 0x10d..0x110) is implied by
   the symmetry of the IP/mask/gateway block but not directly proven.

---

## Recommended next steps for clean-room replacement

If the goal is to write open firmware that can either coexist with
Delta's daemons or wholesale replace them while preserving binary
compatibility with the web CGI and SNMP MIB:

1. **Reserve the SysV key `0x153E` and segment size `0x40000`** in your
   `shmget()` call. Always use those constants.

2. **Maintain the `/dev/mtdblock4` mirror semantics**: at boot, read
   the 256 KB segment off mtd4, verify the BE-u32 checksum at
   `[0xFFFC..0xFFFF]` of each 128 KB half, and bail to a default if
   the checksum fails. On config change, write back through the same
   path with checksum-update — preserve the **two-half-with-redundant-
   checksum** layout to keep the stock bootloader behaviour
   compatible.

3. **Decide whether you replace `main` or live next to it.** If you
   replace it, you must:
   * Implement the network-bringup chain (wlan0 only — eth0 is
     untouched by stock `main`).
   * Implement the daemon-respawn-on-config-change pattern (it
     `killall`-then-`/root/X &` to restart things).
   * Implement the SHRMEM_UPDATE_IP_ADDRESS, _FIRMWARE, _HMI_FIRMWARE
     flag pollers — these are the only documented IPC contracts with
     the web UI.
   * Provide `StoreFlash()` semantics so other daemons (Charging_Standard,
     FlashLog, MeterIC_new) can request a flush via byte 0x1dd =
     "busy" handshake.

4. **For the trap_alarm bitmap**, define a `set_fault(bit_index)`
   helper that atomically OR-s into the right byte, and use the
   31-entry Pri_Comm alarm catalog as your authoritative fault
   namespace. Set byte +0x157 bit 0x40 ("Initial") at startup until
   all subsystems report healthy — that matches stock behaviour.

5. **Don't write to offsets 0xFFFC..0xFFFF at runtime** — they are
   the on-flash checksum. `StoreFlash()` writes them; nothing else
   should.

6. **For OCPP**, treat the `0x400..0x720` block as the
   write-through config store. DeltaOCPP reads it; `main`'s
   `GetConfig` writes it. If you replace `DeltaOCPP` (e.g. with
   `evcc` or `OpenOCPP`), wire those config strings into your
   OCPP client at startup, and re-read whenever byte 0x1bd
   (config-changed flag) is set.

7. **Drop in a hardware watchdog kicker**: stock uses busybox
   `watchdog`; you can keep that.

8. **Verify on a live unit** that the 4-byte values at 0x101..0x10c
   really are stored MSB-first (network order) and the same for
   0x158..0x15b (SNMP trap manager IP) and 0x190..0x193 (SNTP IP) —
   the disasm strongly implies BE but a one-byte read on a known
   IP would be conclusive.

---

## Raw extracts (for cross-reference)

* `02-main-symbols.txt` — full `nm -n main` + all `strings` from main.
* `02-shrmem-offsets-from-all-binaries.txt` — every offset hit in
  every daemon, kinds (ldrb/strb/ldr/str…) and example instruction
  addresses.
* `02-shrmem-rolemap.txt` — pivot table: rows = SHRMEM offsets, cols =
  daemons, cells = R / W / RW / `.`.

