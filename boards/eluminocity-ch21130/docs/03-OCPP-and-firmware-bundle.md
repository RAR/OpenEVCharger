# Delta OCPP + Firmware Bundle Analysis

Reverse-engineering target: Delta Electronics EVMU30 AC Mini Level-2 EVSE (rebadged Eluminocity CH-21130). Static analysis of `/home/rar/device-configs/esphome/testcharger/delta/{DeltaOCPP, ACFWMaker, FWMaker, mini_httpd, main}` plus the unpacked rootfs and CGI source.

## TL;DR

- **OCPP version is 1.5 SOAP/XML over HTTP(S)**, implemented with **gSOAP/2.8** on top of OpenSSL 1.0.0. There is no JSON, no WebSocket, no OCPP 1.6-J transport in the binary. The full 1.5 message catalog (CP→CS and CS→CP) is implemented, including local-list, reservations, and DataTransfer.
- The CP listens locally on **TCP/8080** (for CS→CP calls) and dials the CSMS at `http[s]://<host>:<port>/ChargeBox/OCPP15`. Endpoint host/port/security are read from a shared-memory block populated by `/root/main` from `/Storage/DownloadConfiguration`.
- **UpdateFirmware** is wired end-to-end: the CSMS passes a URL like `ftp://user:pass@host/path/DcoFImage`, the daemon parses it, runs busybox `ftpget`, and the file is staged at `/mnt/DcoFImage`. After a reboot, `/root/main`'s `UpdateCSU()` finds the magic header, verifies the byte-sum checksum, and dd's the payload onto `/dev/mtdblock2` (kernel) or `/dev/mtdblock5` (rootfs).
- **FWMaker / ACFWMaker bundle format is trivial**: `<raw_payload><9-byte ASCII magic><big-endian uint32 byte-sum>`. Magic is `DELTADCOK` for kernel, `DELTADCOF` for rootfs and AC-mini-primary. ACFWMaker additionally **pads the input to a 1024-byte boundary with 0xFF** before the trailer. No CRC, no MD5, no signature. Custom firmware is straightforward to forge.
- The web-UI CGI path (`KernelUp.c`, `RootfsUp.c`, `HmiUp.c`) drops the **raw** payload (no magic, no checksum) to `/mnt/{uImage, rootfs_nor.img, HMI_FW}` and sets a SHM flag. The bundled-magic verification only kicks in on the **FTP/USB** paths. Web uploads bypass it.

## OCPP layer (DeltaOCPP)

### Version + transport

OCPP 1.5 SOAP, by every available signal:

- Namespace literals embedded in the binary (`strings -t x DeltaOCPP`):
  - file off `0x848e0`: `urn://Ocpp/Cp/2012/06/`  — CP namespace (used as `xmlns:ns2:`)
  - file off `0x84908`: `urn://Ocpp/Cs/2012/06/`  — CS namespace (`xmlns:ns1:`)
  - `2012/06` is the publication date of OCPP 1.5; 1.6 used `2015/10`.
- Endpoint URL template (file off `0x8a988` / `0x8a9a8`):
  ```
  https://%s:%d/ChargeBox/OCPP15
  http://%s:%d/ChargeBox/OCPP15
  ```
  The `/ChargeBox/OCPP15` path is the canonical Delta 1.5 SOAP endpoint.
- gSOAP runtime fingerprints: `gSOAP/2.8`, `gSOAP Web Service`, `stdsoap2.c`, plus all the standard `soap_*` codecs in `nm` output (1700+ symbols).
- No JSON encoder/decoder, no `ws://`, `wss://`, `Sec-WebSocket-Key`, or `Origin:` strings. Not a JSON-OCPP build.
- WS-Addressing 1.3 is wired (`soap_wsa_*` symbols) — uses `<wsa:To>`, `<wsa:Action>`, `<wsa:MessageID>` in headers, set up by the in-tree `makeHeader` helper (sym `0xaea8`).

The two transports plain-HTTP and HTTPS are both compiled in. Which one is used is gated by **one byte in shared memory at `MeterSMPtr+0x202` bit 0** (visible in `main()` at `0xc6cc`–`0xc6dc` and again at `0xc784`–`0xc7ec` in DeltaOCPP). If set, `soap_ssl_client_context()` is called and the `https://...` template is sprintf'd; otherwise the `http://...` one.

### Endpoint config

DeltaOCPP does **not** read `/Storage/DownloadConfiguration` directly. It reads from a SysV shared-memory segment created by `/root/main` (key visible at `main+0xb4` in `shmget`). The segment is also persisted to `/mnt/ShareMemory.bin` (see `/root/main` strings) and laid out roughly as:

| Offset (`MeterSMPtr +`) | Field | Source line in `DownloadConfiguration` |
|---|---|---|
| `+0x100..+0x103` | FW version (BCD, 4 bytes) | (system, not user-set) |
| `+0x108..+0x10B` | CSMS server IPv4 (4 bytes, when stored numeric) | `OCPP Server URL` |
| `+0x1bc..+0x1bf` | UpdateFirmware retry counters | `ResetRetries` |
| `+0x200..+0x202` | bitfield flags (incl. SSL enable at bit 0 of +0x202) | `OCPP Security`, `Backend System`, etc. |
| `+0x400..` | CSMS hostname / endpoint pointer area | `OCPP Server URL` |
| `+0x460..` | CSMS port string | (parsed from URL) |
| `+0x600` | "OCPP ready" handshake byte (waited on by DeltaOCPP main loop at `c5e8`) | set by `/root/main` after config load |
| `+0x740..` | Charge Point Model | `OCPP Charge Point Model` |
| `+0x760..` | Charge Box ID | `OCPP Charge Box ID` |
| `+0xa70` | RecvThread "OCPP enabled" gate | set by `/root/main` when `Backend System=1` |
| `+0xa90..` | OCPP User ID | `OCPP User ID` |
| `+0xca0` (+7..+11) | "transactionId valid" cache | runtime |
| `+0x19c0+0x25` | Pending UpdateFirmware: 4-byte timestamp | UpdateFirmware req |
| `+0x19c0+0x29` (500 B) | Pending UpdateFirmware: target URL (sans `://`) | UpdateFirmware req |
| `+0x1bc0+0x1d` | Pending UpdateFirmware: retrieveDate | UpdateFirmware req |
| `+0x1bc0+0x21` | Pending UpdateFirmware: numRetries+retryInterval | UpdateFirmware req |
| `+0x1c0` (bit 6 = `0x40`) | Pending UpdateFirmware flag — main loop polls this | UpdateFirmware req sets it |

(Offsets recovered by cross-referencing `main+0x308`..`main+0x460` setup with `soap_serve___ns1__UpdateFirmware+0x70`..`+0x250` writes.)

`/root/main` is the system supervisor. It owns the shared memory, the watchdog, the GPIO/charging state machine, and gates which background daemons run. It conditionally launches DeltaOCPP only when `Backend System` ≠ 0 in `DownloadConfiguration` (string evidence: `killall DeltaOCPP` and `/root/DeltaOCPP &` both appear in `/tmp/main-strings.txt`, with no entry in `/etc/funs`). The Charging-Standard daemon is launched in the same code path; `Charging_Standard_RFID` is the variant when `Authentication Mode > 0`.

The local SOAP-server bind is at `ReceiveThread+0x68` (`b990`): hard-coded **port 0x1f90 = 8080**, bound on `0.0.0.0` after `inet_ntoa` of an SHM-supplied IP. SSL accept path is the same with `soap_ssl_server_context` first.

### Message catalog

#### CP → CSMS (this charger initiates)

All present and implemented. The `cmd*` symbols are the high-level wrappers that DeltaOCPP's main loop calls.

| Message | Implemented | Wrapper symbol | Trigger | Evidence |
|---|---|---|---|---|
| BootNotification | yes | `cmdBootNtf` (`0xb608`) | First action in DeltaOCPP `main`, retried every 5 min on failure | `c904: bl cmdBootNtf`; on retry success `sleep(600)` else `sleep(300)` |
| Heartbeat | yes | `cmdHeartbeat` (`0xb3f8`) | Periodic, gated by `HeartBeatInterval` config | symbol; `INCOMING: Heartbeat` log |
| Authorize | yes | `cmdAuthorize` (`0xaf20`) | RFID swipe → IdTag verify upstream | log marker present |
| StartTransaction | yes | `cmdStartTransaction` (`0xb058`) | Plug-in / EV start | log marker present |
| StopTransaction | yes | `cmdStopTransaction` (`0xb234`) | Unplug / fault | log marker present |
| MeterValues | yes | `cmdMeterValues` (`0xb490`) | `MeterValueSampleInterval` + `ClockAlignedDataInterval` | symbol, config keys present |
| StatusNotification | yes | `cmdStatusNtf` (`0xb70c`) | EVSE state change | log marker present |
| FirmwareStatusNotification | yes | `cmdFirmwareStatusNtf` (`0xb818`) | Called after each phase of an UpdateFirmware flow (`Downloaded` → `Installed` etc.) | `main+0x44b4: bl cmdFirmwareStatusNtf` |
| DiagnosticsStatusNotification | yes | `cmdDiagnosticsStatusNtf` (`0xb8a4`) | After `GetDiagnostics` triggered `ftpput` | log marker present |
| DataTransfer | yes (codec only) | (no `cmd*` wrapper found) | Vendor extension hook; codec available via `soap_call___ns2__DataTransfer` | symbol present but never called from non-soap code path |

#### CSMS → CP (this charger responds)

Served on TCP/8080 via gSOAP `soap_serve` dispatch. Each request hits the auto-generated `soap_serve___ns1__<Action>` which decodes args then calls the corresponding `Fun__ns1__*Response` body.

| Action | Implemented | Behavior | Evidence |
|---|---|---|---|
| Reset | yes | sets a flag; reboot path goes through `/root/main` SHM write | `Fun__ns1__ResetResponse` `0x8a2c0`, `ResetFailure` string |
| UnlockConnector | yes | `Fun__ns1__UnlockConnectorResponse` (`0x8a21c`); flips connector-lock SHM byte | `ConnectorLock` symbol `0xbf64`, `ConnectorLockFailure` string |
| ChangeAvailability | yes | `Fun__ns1__ChangeAvailabilityResponse` (`0x8a408`); writes Operative/Inoperative to SHM | `Inoperative`, `Operative` strings |
| GetDiagnostics | yes | runs `ftpput [-u u -p p] <host> /Storage/EncodeLogMessage`, then DiagnosticsStatusNotification | `Fun__ns1__GetDiagnosticsResponse` `0x8a4ac`, ftpput format strings |
| GetConfiguration | yes | enumerates keys, returns values from SHM | `Fun__ns1__GetConfigurationResponse` `0x8a760` |
| ChangeConfiguration | yes | writes new value to SHM; some keys reboot-required | `Fun__ns1__ChangeConfigurationResponse` `0x8ac6c`, `ns1:ConfigurationStatus` enum (`Accepted`, `Rejected`, `NotSupported`, `RebootRequired`) |
| ClearCache | yes | runs `rm -f /Storage/OCPPLocalList`, plus clears in-memory list | `Fun__ns1__ClearCacheResponse` `0x8a610`, rm command string |
| UpdateFirmware | yes | parses location URL, FTP-fetches `/mnt/DcoFImage`, sets pending flag | `soap_serve___ns1__UpdateFirmware` `0x8ccb8`, ftpget format strings |
| RemoteStartTransaction | yes | injects a virtual IdTag into the auth pipeline | `Fun__ns1__RemoteStartTransactionResponse` `0x8ad14` |
| RemoteStopTransaction | yes | flips StopTxn requested flag | `Fun__ns1__RemoteStopTransactionResponse` `0x8adbc` |
| ReserveNow | yes | populates reservation slot (time, IdTag, connector) | `Fun__ns1__ReserveNowResponse` `0x8ae60`, "Charging Point Reservation Time Out", "Charging Point Reservation Canceled" |
| CancelReservation | yes | clears reservation slot | `Fun__ns1__CancelReservationResponse` `0x8a170` |
| SendLocalList | yes | replaces or appends in `/Storage/OCPPLocalList`, calls `UpdateLocalList` (`0x89b4c`) | `Fun__ns1__SendLocalListResponse` `0x8a364` |
| GetLocalListVersion | yes | reads version from local-list file | `Fun__ns1__GetLocalListVersionResponse` `0x8a54c` |
| DataTransfer | yes (codec) | inbound codec exists; handler returns `UnknownVendorId` for unrecognized | `UnknownVendorId`, `UnknownMessageId` strings |

This is **all 15 of the OCPP 1.5 CS→CP actions** and **all 10 CP→CS actions**, plus DataTransfer extension. No core 1.5 message is missing.

### Configuration keys

The following keys are surfaced as literal strings in the binary and are read/written from SHM by the GetConfiguration/ChangeConfiguration handlers. These are the **standard OCPP 1.5 keys** — none of them are Delta-vendor-specific.

```
HeartBeatInterval           # interval in seconds
MeterValueSampleInterval    # interval in seconds
ClockAlignedDataInterval    # interval in seconds (0 = disabled)
MeterValuesSampledData      # CSV of OCPP MeasurandSampled enum values
MeterValuesAlignedData      # CSV of OCPP MeasurandSampled enum values
StopTxnSampledData
StopTxnAlignedData
ResetRetries                # number of OCPP-Reset retries
SystemFirmwareVersion       # read-only
SystemSN                    # read-only (Charge Box serial)
SystemTime                  # current time (read-only; settable via DataTransfer?)
```

No `AuthorizationCacheEnabled`, `AllowOfflineTxForUnknownId`, `LocalAuthListEnabled`, `LocalAuthorizeOffline`, etc. — but the **behavior** for those is wired (offline-charging and local-list are both controlled instead from `/Storage/DownloadConfiguration` and re-read from SHM at runtime), they're just not exposed over OCPP. A modern CSMS that tries to ChangeConfiguration them will get `NotSupported`.

### TLS + auth

Linked against `libssl.so.1.0.0` + `libcrypto.so.1.0.0` (visible in `readelf -d`). The certificate-handling code paths are:

- `soap_ssl_server_context` (`0x19a2c`) — called from `ReceiveThread+0x130` only when the SSL-enable SHM bit (`MeterSMPtr+0x202` bit 0) is set.
- `soap_ssl_client_context` (`0x19bb0`) — called from `main+0x758` under the same gate.

The third/fourth `soap_ssl_*_context` arguments (cert chain file, key file, password) are all passed `NULL` from the caller (visible in `main+0x794..0x7a0` and `ReceiveThread+0x118..0x130`: four `mov r2/r3,#0` stores onto the stack before the call). That means **the daemon configures SSL with no client cert, no server cert, no key, and no CA bundle** — the default OpenSSL "anonymous client / no peer verification" mode. In `DownloadConfiguration` this corresponds to `OCPP Security: 1` (toggle SSL on/off; no granular flag for cert-pinning).

So a custom CSMS that wants encrypted OCPP-S 1.5 from this charger needs to:
1. Run an HTTPS server speaking SOAP at `/ChargeBox/OCPP15`.
2. Present *any* certificate — DeltaOCPP will not verify it.
3. Set `OCPP Security: 1` in the unit's `DownloadConfiguration`.

No HTTP Basic / Digest auth is wired; the only credential is the `OCPP User ID` field which is sent in the SOAP body as an `IdTag`-like identifier (not as an HTTP header).

### Firmware update via OCPP

Implemented. Full flow:

1. CSMS calls `<UpdateFirmware>` with `<location>ftp://USER:PASS@HOST/PATH/DcoFImage</location>`, `<retrieveDate>`, `<retries>`, `<retryInterval>`.
2. `soap_serve___ns1__UpdateFirmware` (`0x8ccb8`) decodes the URL, copies it to SHM at `MeterSMPtr+0x19E9`, copies retries/retryInterval to `+0x1BDD/0x1BE1`, sets the pending-bit at `+0x1C0 | 0x40`, and immediately replies with an empty `UpdateFirmwareResponse`.
3. The main loop in DeltaOCPP polls that flag (`main+0x4250..` is the FW-handler block) and, when set, calls `cmdFirmwareStatusNtf("Downloading")`.
4. It parses the URL: `strchr(':')` → user, `strchr('@')` → password and host boundary, `strchr('/')` → path. Then `sprintf` into the ftpget format string (`main+0x4488..0x44a4`):
   ```
   ftpget -u <user> -p <pass> <host> /mnt/DcoFImage <path>
   ```
   or (anonymous):
   ```
   ftpget <host> /mnt/DcoFImage <path>
   ```
   It `system()`s the command (busybox `ftpget` is the runner).
5. On success it sends `Downloaded`, then runs the "kill and replace" sequence from `/tmp/deltaocpp-strings.txt`:
   ```
   mkdir /backup/root;cp -fp /root/* /backup/root
   cd /backup/root;rm -f main;rm -f LED_control;rm -f DeltaOCPP;
   cd /backup/root;killall *
   ```
   This stages a copy of the current `/root/*` into a tmpfs, kills `main`/`LED_control`/`DeltaOCPP` so they don't hold filesystem inodes, and then `/root/main`'s `UpdateCSU()` flow takes over from the tmpfs copy.
6. Finally `cmdFirmwareStatusNtf("Installed")` or `InstallationFailed`.

**Important**: the OCPP-delivered file *must* be in **DELTADCOF** bundle format (rootfs). The kernel/HMI cannot be updated via OCPP — only the userland rootfs. This is hard-coded: the target filename is `/mnt/DcoFImage`, which `UpdateCSU()` only treats as a rootfs candidate (writes to `/dev/mtdblock5`).

## Firmware bundle format (ACFWMaker / FWMaker)

### Container structure (header + payload + footer)

There is **no header**. The container is `<payload><9-byte magic><4-byte BE checksum>`:

```
+--------------------------------------+---------------+------------------+
| 0 .. N-1 : raw image bytes           | N .. N+8 :    | N+9 .. N+12 :    |
| (uImage / rootfs.img / ACmini.bin)   | ASCII magic   | sum BE-uint32    |
+--------------------------------------+---------------+------------------+
```

Total size = `N + 13`. The verifier in `/root/main` (`UpdateCSU` and `GetPrimaryFW`) scans the buffer for the first `'D'` followed by the matching 9-byte magic, treats everything before that offset as the payload, sums those bytes, and compares against the trailing 4 BE bytes. The scan terminates at `read_size` so an over-sized 0xFF-padded buffer still resolves correctly.

### Magic bytes / file signature

```
DELTADCOK   44 45 4C 54 41 44 43 4F 4B    -> kernel  (DcoKImage)  -> /dev/mtdblock2
DELTADCOF   44 45 4C 54 41 44 43 4F 46    -> rootfs  (DcoFImage)  -> /dev/mtdblock5
DELTADCOF   44 45 4C 54 41 44 43 4F 46    -> ACmini  (ACmini_Primary, primary HMI fw)
```

Both rootfs and ACmini-primary use the same magic. The differentiator is the **filename** the unit looks for (`/UsbFlash/DcoFImage` vs `/UsbFlash/ACmini_Primary.bin`) and the destination (mtdblock5 vs serial-uplink to the HMI MCU via `Pri_Comm`).

### Validation algorithm

Trivial unsigned-32-bit byte-sum:

```c
uint32_t sum = 0;
for (int i = 0; i < payload_size; i++) sum += buf[i];
// stored at file_end-4 as MSB-first 4 bytes
```

**No CRC, no MD5, no HMAC, no signature.** Easily forged — the only "tamper detection" is that a corrupted byte must be balanced by an equal-magnitude opposite-sign perturbation elsewhere to keep the sum.

### Example layout (annotated hex dump)

Take a stub 16-byte rootfs payload `00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F` (byte sum = `0x78`):

```
00000000  00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F  payload (N=16)
00000010  44 45 4C 54 41 44 43 4F  46                       "DELTADCOF" magic
00000019                              00 00 00 78           sum = 120 (BE u32)
0000001D  <eof>
```

A real `DcoFImage` would be ~10 MB of compressed JFFS2 followed by these 13 bytes.

For an ACmini_Primary built from a 30000-byte source the format is:

```
00000000..00007530   30000 bytes of payload
00007531..00007fff   0xFF * 720      <- pad to 32768 (next 1024 boundary)
00008000..00008008   "DELTADCOF"
00008009..0000800C   BE-uint32 sum (over the 32768 padded bytes)
```

### How to craft a custom-firmware bundle

Trivial one-liner in Python. Self-contained crafter (also in `03-ACFWMaker-disasm.txt`):

```python
import struct, sys, pathlib

def pack(payload: bytes, magic: bytes, pad_to: int = 1) -> bytes:
    assert magic in (b"DELTADCOK", b"DELTADCOF") and len(magic) == 9
    if pad_to > 1 and len(payload) % pad_to:
        payload = payload + b"\xFF" * (pad_to - len(payload) % pad_to)
    return payload + magic + struct.pack(">I", sum(payload) & 0xFFFFFFFF)

# Rootfs over OCPP UpdateFirmware:
pathlib.Path("DcoFImage").write_bytes(
    pack(pathlib.Path("my_rootfs_nor.img").read_bytes(), b"DELTADCOF"))

# Kernel via USB (only via /UsbFlash, no OCPP path):
pathlib.Path("DcoKImage").write_bytes(
    pack(pathlib.Path("my_uImage").read_bytes(), b"DELTADCOK"))

# HMI primary firmware via USB:
pathlib.Path("ACmini_Primary.bin").write_bytes(
    pack(pathlib.Path("my_hmi.bin").read_bytes(), b"DELTADCOF", pad_to=1024))
```

Three injection vectors (in increasing intrusiveness):

1. **Web UI** (port 80, `/cgi-bin/{KernelUp,RootfsUp,HmiUp}.cgi`, htpasswd-protected with `vern:ja3K3ABtAFcQw`) — **takes raw uImage / raw rootfs / raw HMI gzipped fw, no magic and no checksum**. The CGI just strips the MIME boundary and writes to `/mnt/{uImage, rootfs_nor.img, HMI_FW}`, then sets a SHM byte `SHRMEM_UPDATE_FIRMWARE` or `SHRMEM_UPDATE_HMI_FIRMWARE`. `/root/main` reboots and applies. This is the easiest path — no checksum to satisfy.
2. **USB stick** at `/UsbFlash/{DcoKImage, DcoFImage, ACmini_Primary.bin, DeltaEVSEConfig}` — requires the magic+checksum bundle. `/root/main` polls for these files at boot/during runtime.
3. **OCPP UpdateFirmware** — requires the magic+checksum bundle and a reachable FTP server. Only `DcoFImage` (rootfs) supported via this path.

The CGI path is the weakest link: a single POST with `Content-Type: application/octet-stream` and the raw `uImage` body, against the local web server, will overwrite the kernel partition on the next reboot. The htpasswd is the only gate, and it's a single hard-coded user (`vern`).

## mini_httpd (stripped binary, limited analysis)

It's stock `mini_httpd/1.19 19dec2003` from `http://www.acme.com/software/mini_httpd/` (the `Server:` header literal and copyright URL are still in the binary). The build is config-driven by `/root/www/mini_httpd.conf`:

```
port=80
dir=/root/www
cgipat=cgi-bin/*
user=root          # *not* nobody — runs as root, no privilege-drop on CGI
charset=big5
pidfile=/root/www/mini_httpd.pid
logfile=/root/www/mini_httpd.log
```

`nochroot` (the default when no `chroot` option is present) — no chroot tree. There are no extra environment variables passed to CGIs beyond the standard CGI/1.1 set (`SERVER_SOFTWARE`, `SERVER_NAME`, `GATEWAY_INTERFACE`, `SERVER_PROTOCOL`, `SERVER_PORT`, `REMOTE_ADDR`, `HTTP_REFERER`, `HTTP_USER_AGENT`, `HTTP_COOKIE`, `HTTP_HOST`, `CONTENT_TYPE`, `CONTENT_LENGTH`, `REMOTE_USER`). All visible in strings.

The CGIs need to run as root because they write to `/mnt/` and `shmget(0777)` against `MeterSMKey` (defined in `www/define.h` but not present in dump; the value matches the one used by `/root/main` at `main+0xb4`).

## Unknown / unresolved

- **Exact SHM key value**: `shmget(MeterSMKey, MeterSMSize, ...)` is used everywhere but `MeterSMKey` is a CPP macro from `define.h` which isn't in the dump. Recoverable by reading the immediate constant at `main+0xb4` in `/root/main` (left for a runtime probe).
- **Which SHM byte is `SHRMEM_UPDATE_FIRMWARE`** vs `SHRMEM_UPDATE_HMI_FIRMWARE`: also CPP macros not in the dump. The CGI sources set them but the literal offsets are linked at build time. Easy to recover by diff-dumping `/mnt/ShareMemory.bin` before/after a CGI POST.
- **HMI firmware container format** (`/mnt/HMI_FW`): the CGI strips MIME and writes the body raw, then `/root/main` hands it off to the `Pri_Comm` daemon over an internal serial protocol to flash the STM32 HMI. The exact wire format of that serial upload was not analyzed here — it lives in `/root/Pri_Comm` (see existing `01-Pri_Comm-symbols.txt` in this directory).
- **DataTransfer vendor extensions**: codec exists, but no `cmdDataTransfer` wrapper and no business logic visible. CP will reply `UnknownVendorId` to any incoming DataTransfer. No outgoing DataTransfer is ever issued.
- **Whether `OCPP local list` config actually allows offline auth**: strings + symbols (`UpdateLocalList`, `VerifyIdTag`, `/Storage/OCPPLocalList`, `Charging_Standard_RFID`) suggest yes, but the gate that decides offline-allow vs reject lives in `/root/main`'s `VerifyIdTag` at `0x11514` — not exhaustively traced here.

## Recommended next steps

### For OCPP integration

1. **Stand up a minimal SOAP CSMS** that listens at `http://<our-ip>:9000/ChargeBox/OCPP15`. Use Python with `pysimplesoap` or write a Flask app that handles the eight CP→CS actions (`BootNotification`, `Heartbeat`, `StatusNotification`, `MeterValues`, `StartTransaction`, `StopTransaction`, `Authorize`, `FirmwareStatusNotification`, `DiagnosticsStatusNotification`, `DataTransfer`). Replies for all of them are simple — `BootNotificationResponse` just needs `currentTime`, `heartbeatInterval`, `status=Accepted`.
2. **Pre-populate `/Storage/DownloadConfiguration`** (or use the on-device CGI Management page) with:
   ```
   Backend System: 1
   OCPP Server URL: http://<our-ip>:9000
   OCPP Charge Box ID: <something memorable>
   OCPP Security: 0
   ```
   Reboot. Watch `/Storage/EncodeLogMessage` and the inbound traffic on port 9000 — the unit will start `BootNotification`s every 5 min until accepted.
3. **For OCPP 1.6/2.0 integration**, the only realistic path is a **shim/proxy** that translates 1.5-SOAP-on-the-wire to 1.6-JSON-WebSocket. There are open-source bridges (e.g. SteVe used to ship a 1.5-SOAP backend; or a custom 100-LoC translator in Python). The binary cannot be patched to speak JSON-WS without a full rewrite.
4. **For "freeing" the OCPP layer** to use it with arbitrary CSMSes, the recommended move is to leave the daemon stock and configure the URL to point at a local proxy; modifying the binary risks the watchdog/main supervisor killing/restarting it (`killall DeltaOCPP` + `/root/DeltaOCPP &` is the supervisor's automatic respawn).

### For custom firmware injection

1. **Easiest path — CGI rootfs upload**:
   - Build a custom `rootfs_nor.img` (JFFS2, same partition geometry as the original at `/dump/mtd5-rootfs.bin`).
   - `curl -u vern:<password> -F file=@my_rootfs.img http://<charger>/cgi-bin/RootfsUp.cgi`
   - No magic, no checksum needed — the CGI accepts raw octet-stream.
   - Reboot — `/root/main` on next boot sees `SHRMEM_UPDATE_FIRMWARE=1` and runs `UpdateCSU()` which **does** verify the magic+sum trailer — so **this CGI path is actually mis-coded and won't work end-to-end without also writing the magic trailer.** To make CGI uploads work, append `DELTADCOF + <BE sum>` to the rootfs image before POSTing. Or skip CGI and POST a pre-bundled file (which is the safer, well-defined path).
   - Actually re-reading the CGI source: it writes to `/mnt/rootfs_nor.img`. `UpdateCSU()` reads from `/mnt/DcoFImage`. **Different file**. So the CGI path uses a *different* code branch in `/root/main` that hasn't been fully traced yet — likely a raw-no-magic flasher gated by `SHRMEM_UPDATE_FIRMWARE`. Confirm by tracing the consumer of that flag in `main`.
2. **Cleanest path — OCPP UpdateFirmware**:
   - Build `DcoFImage` with the Python crafter above (`DELTADCOF` magic, BE sum).
   - Host it on an FTP server.
   - Send `UpdateFirmware` SOAP request from the CSMS shim with `<location>ftp://anon@<host>/DcoFImage</location>`.
   - Watch FirmwareStatusNotifications, then power-cycle.
3. **For HMI/STM32 firmware** (the front-panel MCU): can only be done via the web CGI (`HmiUp.cgi`, takes a gzip-compressed body) or via USB (`ACmini_Primary.bin` with `DELTADCOF` magic + 1KB pad). No OCPP path. The serial-uplink protocol from `/root/Pri_Comm` to the STM32 is the next thing to reverse if you want to e.g. brick-recover from a bad HMI flash.
4. **Before flashing anything**, capture the existing `/dev/mtdblock0..5` (we already have them under `dump/mtd*-*.bin`) and verify a round-trip: pack the captured rootfs through the Python crafter and binary-diff against a stock `DcoFImage` from any official Delta release if you can get one. If they match byte-for-byte, the format is fully understood.
5. **htpasswd cracking**: the line `vern:ja3K3ABtAFcQw` is a 13-char DES `crypt(3)` hash. Salt is `ja`, expected hash `3K3ABtAFcQw`. Brute-force or `hashcat -m 1500` will recover the password in seconds for any password ≤7 chars (DES truncates). That unlocks all three CGI upload endpoints over the local network.
