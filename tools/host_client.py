#!/usr/bin/env python3
"""FC41D-side reference TLV client for OpenBHZD UART4 (PC12 TX / PD2 RX).

Connect a USB-UART adapter at 115200 8N1 to:
  PC12 (MCU TX) → adapter RX
  PD2  (MCU RX) → adapter TX
  GND            → adapter GND

Usage:
  ./host_client.py /dev/ttyUSB0           # full handshake demo
  ./host_client.py /dev/ttyUSB0 listen    # passive: print every frame
  ./host_client.py /dev/ttyUSB0 ping
  ./host_client.py /dev/ttyUSB0 state
  ./host_client.py /dev/ttyUSB0 buildinfo
  ./host_client.py /dev/ttyUSB0 amps 32   # SET_ADVERTISED_AMPS
  ./host_client.py /dev/ttyUSB0 clear 0   # CLEAR_FAULT (0 = all clearable)
  ./host_client.py /dev/ttyUSB0 faultlog 16
  ./host_client.py /dev/ttyUSB0 lifetime
  ./host_client.py /dev/ttyUSB0 stop      # REQUEST_STOP
  ./host_client.py /dev/ttyUSB0 resume    # REQUEST_START_RESUME
  ./host_client.py /dev/ttyUSB0 cal 1462 3540 459   # WRITE_CALIBRATION (anchor num den)
"""

import struct
import sys
import time
from typing import Optional, Tuple

try:
    import serial  # pyserial
except ImportError:
    sys.stderr.write("pyserial not installed. `pip install pyserial`\n")
    sys.exit(1)


SOF = b"\xa5\x5a"

# Commands FC41D → MCU
CMD_PING = 0x01
CMD_GET_STATE = 0x02
CMD_SET_ADVERTISED_AMPS = 0x03
CMD_REQUEST_STOP = 0x04
CMD_REQUEST_START_RESUME = 0x05
CMD_CLEAR_FAULT = 0x06
CMD_GET_FAULT_LOG = 0x07
CMD_GET_LIFETIME_KWH = 0x08
CMD_WRITE_CALIBRATION = 0x09
CMD_SET_LED_OVERRIDE = 0x0A
CMD_BUZZER_BEEP = 0x0B
CMD_GET_BUILD_INFO = 0x0C

# Events / responses MCU → FC41D
EVT_STATE_CHANGED = 0x80
EVT_PING_ACK = 0x81
EVT_STATE_REPORT = 0x82
EVT_FAULT_RAISED = 0x83
EVT_FAULT_CLEARED = 0x84
EVT_SESSION_BEGAN = 0x85
EVT_SESSION_ENDED = 0x86
EVT_BOOT_COMPLETE = 0x87
EVT_FAULT_LOG_ENTRY = 0x88
EVT_FAULT_LOG_END = 0x89
EVT_LIFETIME_KWH = 0x8A
EVT_BUILD_INFO = 0x8C

EVT_NAMES = {
    EVT_STATE_CHANGED: "STATE_CHANGED",
    EVT_PING_ACK: "PING_ACK",
    EVT_STATE_REPORT: "STATE_REPORT",
    EVT_FAULT_RAISED: "FAULT_RAISED",
    EVT_FAULT_CLEARED: "FAULT_CLEARED",
    EVT_SESSION_BEGAN: "SESSION_BEGAN",
    EVT_SESSION_ENDED: "SESSION_ENDED",
    EVT_BOOT_COMPLETE: "BOOT_COMPLETE",
    EVT_FAULT_LOG_ENTRY: "FAULT_LOG_ENTRY",
    EVT_FAULT_LOG_END: "FAULT_LOG_END",
    EVT_LIFETIME_KWH: "LIFETIME_KWH",
    EVT_BUILD_INFO: "BUILD_INFO",
}


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def build_frame(cmd: int, seq: int, payload: bytes = b"") -> bytes:
    if len(payload) > 56:
        raise ValueError("payload too long")
    body = struct.pack("<BB", cmd, seq) + payload      # CMD + SEQ + payload
    length = struct.pack("<H", len(body))               # u16 LE
    crc = crc16_ccitt(length + body)
    return SOF + length + body + struct.pack(">H", crc) # CRC big-endian


def parse_frame(buf: bytearray) -> Tuple[Optional[Tuple[int, int, bytes]], int]:
    """Returns ((cmd, seq, payload), consumed_bytes) or (None, drop_bytes)."""
    if len(buf) < 8:
        return None, 0
    if buf[0] != 0xA5 or buf[1] != 0x5A:
        return None, 1
    length = struct.unpack("<H", bytes(buf[2:4]))[0]
    if length < 2 or length > 58:
        return None, 1
    total = 4 + length + 2
    if len(buf) < total:
        return None, 0
    cmd, seq = buf[4], buf[5]
    payload = bytes(buf[6 : 4 + length])
    got = struct.unpack(">H", bytes(buf[4 + length : total]))[0]
    want = crc16_ccitt(bytes(buf[2 : 4 + length]))
    if got != want:
        return None, 1
    return (cmd, seq, payload), total


def receive_frame(ser: serial.Serial, timeout: float = 1.0) -> Optional[Tuple[int, int, bytes]]:
    deadline = time.monotonic() + timeout
    buf = bytearray()
    while time.monotonic() < deadline:
        chunk = ser.read(64)
        if chunk:
            buf.extend(chunk)
        while True:
            parsed, consumed = parse_frame(buf)
            if parsed is not None:
                del buf[:consumed]
                return parsed
            if consumed == 0:
                break
            del buf[:consumed]
        if not chunk:
            time.sleep(0.005)
    return None


def show_frame(cmd: int, seq: int, payload: bytes) -> str:
    name = EVT_NAMES.get(cmd, f"cmd=0x{cmd:02x}")
    extra = ""
    if cmd == EVT_BUILD_INFO:
        extra = f"  build={payload.rstrip(b'\\x00').decode('ascii', errors='replace')!r}"
    elif cmd == EVT_STATE_REPORT and len(payload) >= 28:
        j, e, amps, ccmd = struct.unpack_from("<BBBB", payload, 0)
        cp_hi = struct.unpack_from("<h", payload, 4)[0]
        fbits, ffid = struct.unpack_from("<II", payload, 16)
        extra = f"  j1772={j} evse={e} amps={amps} relay={ccmd} cp_hi={cp_hi}mV faults=0x{fbits:08x} first={ffid}"
    elif cmd == EVT_FAULT_RAISED and len(payload) >= 8:
        fid, j, e, mv = struct.unpack("<IBBh", payload[:8])
        extra = f"  fault_id={fid} j1772={j} evse={e} cp={mv}mV"
    elif cmd == EVT_BOOT_COMPLETE and len(payload) >= 8:
        passed, _, _, _, lf = struct.unpack("<BBBBI", payload[:8])
        extra = f"  self_test_pass={bool(passed)} last_fault={lf}"
    elif cmd == EVT_FAULT_CLEARED and len(payload) >= 4:
        (fid,) = struct.unpack("<I", payload[:4])
        extra = f"  fault_id={fid}"
    elif cmd == EVT_FAULT_LOG_ENTRY and len(payload) >= 32:
        ts, bc, fid, j, e, mv = struct.unpack_from("<IHHBBh", payload, 0)
        extra = f"  ts={ts} boot={bc} fid={fid} j1772={j} evse={e} cp={mv}mV"
    elif cmd == EVT_FAULT_LOG_END and len(payload) >= 2:
        emitted, requested = payload[0], payload[1]
        extra = f"  emitted={emitted}/{requested}"
    elif cmd == EVT_LIFETIME_KWH and len(payload) >= 4:
        (mwh,) = struct.unpack("<I", payload[:4])
        extra = f"  lifetime={mwh} mWh ({mwh/1_000_000:.3f} kWh)"
    return f"<{name} seq={seq} plen={len(payload)}>{extra}"


def cmd_ping(ser):
    ser.write(build_frame(CMD_PING, seq=1))
    f = receive_frame(ser)
    print(show_frame(*f) if f else "no response")


def cmd_state(ser):
    ser.write(build_frame(CMD_GET_STATE, seq=2))
    f = receive_frame(ser)
    print(show_frame(*f) if f else "no response")


def cmd_buildinfo(ser):
    ser.write(build_frame(CMD_GET_BUILD_INFO, seq=3))
    f = receive_frame(ser)
    print(show_frame(*f) if f else "no response")


def cmd_listen(ser):
    print("listening for unsolicited events; Ctrl-C to stop")
    while True:
        f = receive_frame(ser, timeout=10.0)
        if f:
            print(show_frame(*f))


def cmd_set_amps(ser, amps: int):
    ser.write(build_frame(CMD_SET_ADVERTISED_AMPS, seq=4,
                          payload=struct.pack("<B", amps & 0xFF)))
    print(f"sent SET_ADVERTISED_AMPS={amps} (no ack defined)")


def cmd_clear_fault(ser, fid: int):
    ser.write(build_frame(CMD_CLEAR_FAULT, seq=5,
                          payload=struct.pack("<I", fid & 0xFFFFFFFF)))
    f = receive_frame(ser, timeout=0.5)
    print(show_frame(*f) if f else "no EVT_FAULT_CLEARED (may be already clear)")


def cmd_fault_log(ser, count: int):
    ser.write(build_frame(CMD_GET_FAULT_LOG, seq=6,
                          payload=struct.pack("<B", count & 0xFF)))
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        f = receive_frame(ser, timeout=2.0)
        if not f:
            break
        print(show_frame(*f))
        if f[0] == EVT_FAULT_LOG_END:
            return
    print("(GET_FAULT_LOG: timed out before END terminator)")


def cmd_lifetime(ser):
    ser.write(build_frame(CMD_GET_LIFETIME_KWH, seq=7))
    f = receive_frame(ser, timeout=2.0)
    print(show_frame(*f) if f else "no response")


def cmd_stop(ser):
    ser.write(build_frame(CMD_REQUEST_STOP, seq=8, payload=b"\x00"))
    print("sent REQUEST_STOP — watch for STATE_CHANGED or new STATE_REPORT")


def cmd_resume(ser):
    ser.write(build_frame(CMD_REQUEST_START_RESUME, seq=9))
    print("sent REQUEST_START_RESUME")


def cmd_calibration(ser, anchor: int, num: int, den: int):
    payload = struct.pack("<hhh", anchor, num, den)
    ser.write(build_frame(CMD_WRITE_CALIBRATION, seq=10, payload=payload))
    print(f"sent WRITE_CALIBRATION anchor={anchor} num={num} den={den}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    port = sys.argv[1]
    cmd = sys.argv[2] if len(sys.argv) > 2 else "demo"

    ser = serial.Serial(port, 115200, timeout=0.05)
    if cmd == "ping":
        cmd_ping(ser)
    elif cmd == "state":
        cmd_state(ser)
    elif cmd == "buildinfo":
        cmd_buildinfo(ser)
    elif cmd == "listen":
        cmd_listen(ser)
    elif cmd == "amps":
        cmd_set_amps(ser, int(sys.argv[3]))
    elif cmd == "clear":
        cmd_clear_fault(ser, int(sys.argv[3]))
    elif cmd == "faultlog":
        cmd_fault_log(ser, int(sys.argv[3]) if len(sys.argv) > 3 else 16)
    elif cmd == "lifetime":
        cmd_lifetime(ser)
    elif cmd == "stop":
        cmd_stop(ser)
    elif cmd == "resume":
        cmd_resume(ser)
    elif cmd == "cal":
        cmd_calibration(ser, int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]))
    else:  # demo
        cmd_ping(ser)
        cmd_state(ser)
        cmd_buildinfo(ser)


if __name__ == "__main__":
    main()
