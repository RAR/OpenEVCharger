#!/usr/bin/env python3
"""verify_dcofimage.py — sanity-check a built M12 DcoFImage.

Asserts the image meets the docs/22 spec:
  - DELTADCOF magic + BE-uint32 byte-sum trailer round-trips
  - JFFS2 payload extracts (requires `jefferson` on PATH)
  - /root/ contains: delta-bridge + 4 wrappers replacing stock leaf daemons
  - Wrappers exec /root/delta-bridge with the expected arguments
  - Trimmed binaries are absent (DeltaOCPP, ACFWMaker, FWMaker,
    ScenarioMaker, Pri_Comm_cqc, NTC_tmp, PowerCard-UltraLight,
    Charging_Standard)
  - Kept-stock binaries present (main, Pri_Comm, Charging_Standard_RFID,
    FlashLog, RTC, ErrorHandle, snmpd, snmptrap, mini_httpd, wpa_supplicant)
  - /etc/funs invokes first-boot.sh before any daemon
  - /etc/delta-bridge/{first-boot.sh, stk-manifest.txt} + /etc/delta-bridge.conf.default present

Usage:
    verify_dcofimage.py <DcoFImage> [--expected-sha256 <hex>]

Exits 0 on pass, 1 on any failed assertion. Prints a single summary line
on success; failed checks print to stderr.
"""
import argparse
import hashlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


MAGIC = b"DELTADCOF"

EXPECTED_WRAPPERS = {
    "RFID":        b"/root/delta-bridge -c /Storage/delta-bridge.conf",
    "Adc":         b"/root/delta-bridge --personality=adc",
    "MeterIC_new": b"/root/delta-bridge --personality=meter",
    "LED_control": b"/root/delta-bridge --personality=led",
}

TRIMMED = {
    "DeltaOCPP", "ACFWMaker", "FWMaker", "ScenarioMaker",
    "Pri_Comm_cqc", "NTC_tmp", "PowerCard-UltraLight",
    "Charging_Standard",
}

KEPT_STOCK = {
    "main", "Pri_Comm", "Charging_Standard_RFID",
    "FlashLog", "RTC", "ErrorHandle",
    "snmpd", "snmptrap", "mini_httpd",
    "wpa_supplicant", "iwconfig", "iwlist",
    "htpasswd", "simple.script",
}


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)


def verify_trailer(path):
    blob = Path(path).read_bytes()
    if len(blob) < 13 or blob[-13:-4] != MAGIC:
        fail(f"DELTADCOF magic missing (got {blob[-13:-4]!r})")
        return None
    stored = int.from_bytes(blob[-4:], "big")
    payload = blob[:-13]
    computed = sum(payload) & 0xFFFFFFFF
    if stored != computed:
        fail(f"checksum mismatch: stored 0x{stored:08x} computed 0x{computed:08x}")
        return None
    return payload


def extract_jffs2(payload, dest_dir):
    jffs2_path = dest_dir / "payload.jffs2"
    jffs2_path.write_bytes(payload)
    extract = dest_dir / "extracted"
    # Don't pre-mkdir — jefferson refuses if dest exists, and `-f`
    # makes it remove + recreate. Pass -f so reruns are clean.
    try:
        r = subprocess.run(
            ["jefferson", "-f", "-d", str(extract), str(jffs2_path)],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
    except FileNotFoundError:
        fail("`jefferson` not on PATH (pip install jefferson)")
        return None
    except subprocess.CalledProcessError as e:
        # jefferson prints its diagnostics to stdout, not stderr; surface
        # both so an operator can debug without re-running by hand.
        out = e.stdout.decode('utf-8', errors='replace')
        err = e.stderr.decode('utf-8', errors='replace')
        combined = (out + err).strip()[:400] or "(no jefferson output)"
        fail(f"jefferson failed (rc={e.returncode}): {combined}")
        return None
    return extract


def check_image(image_path, expected_sha256=None):
    payload = verify_trailer(image_path)
    if payload is None:
        return False

    with tempfile.TemporaryDirectory() as td:
        dest = Path(td)
        extract = extract_jffs2(payload, dest)
        if extract is None:
            return False

        root = extract / "root"
        if not root.is_dir():
            fail("/root missing in extracted image")
            return False

        ok = True

        # delta-bridge present
        bridge = root / "delta-bridge"
        if not bridge.is_file():
            fail("/root/delta-bridge missing")
            ok = False
        elif expected_sha256:
            got = hashlib.sha256(bridge.read_bytes()).hexdigest()
            if got != expected_sha256:
                fail(f"delta-bridge sha256 mismatch: expected {expected_sha256}, got {got}")
                ok = False

        # Wrappers replace the four leaf daemons
        for name, expected_exec in EXPECTED_WRAPPERS.items():
            p = root / name
            if not p.is_file():
                fail(f"/root/{name} wrapper missing")
                ok = False
                continue
            body = p.read_bytes()
            if not body.startswith(b"#!/bin/ash"):
                fail(f"/root/{name} not a shell wrapper (no #!/bin/ash header)")
                ok = False
            if expected_exec not in body:
                fail(f"/root/{name} doesn't exec the expected target ({expected_exec!r})")
                ok = False

        # Trimmed binaries truly gone
        for name in TRIMMED:
            if (root / name).exists():
                fail(f"/root/{name} should have been trimmed but is present")
                ok = False

        # Kept-stock binaries truly present
        for name in KEPT_STOCK:
            if not (root / name).exists():
                fail(f"/root/{name} expected to be kept stock but is missing")
                ok = False

        # /etc/funs invokes first-boot.sh before any daemon
        funs = extract / "etc" / "funs"
        if not funs.is_file():
            fail("/etc/funs missing")
            ok = False
        else:
            lines = funs.read_text().splitlines()
            try:
                fb_idx = next(i for i, line in enumerate(lines)
                              if "first-boot.sh" in line)
                main_idx = next(i for i, line in enumerate(lines)
                                if "/root/main" in line)
                if fb_idx >= main_idx:
                    fail(f"first-boot.sh runs at line {fb_idx} but /root/main starts at line {main_idx}; should be earlier")
                    ok = False
            except StopIteration:
                fail("/etc/funs doesn't mention first-boot.sh or /root/main")
                ok = False

        # Support files
        for sub in ("delta-bridge/first-boot.sh",
                    "delta-bridge/stk-manifest.txt",
                    "delta-bridge.conf.default"):
            p = extract / "etc" / sub
            if not p.is_file():
                fail(f"/etc/{sub} missing")
                ok = False

        # stk-manifest covers all four wrappers
        manifest = extract / "etc" / "delta-bridge" / "stk-manifest.txt"
        if manifest.is_file():
            listed = {line.strip() for line in manifest.read_text().splitlines()
                      if line.strip()}
            missing = set(EXPECTED_WRAPPERS) - listed
            if missing:
                fail(f"stk-manifest.txt missing entries: {sorted(missing)}")
                ok = False

        if ok:
            kept = sum(1 for n in KEPT_STOCK if (root / n).exists())
            print(f"OK: {image_path} — {len(EXPECTED_WRAPPERS)} wrappers, "
                  f"{kept} stock binaries kept, {len(TRIMMED)} trimmed; "
                  f"DELTADCOF + checksum verified.")
        return ok


def main(argv):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("image")
    p.add_argument("--expected-sha256",
                   help="if given, assert /root/delta-bridge's sha256 matches")
    a = p.parse_args(argv)
    return 0 if check_image(a.image, a.expected_sha256) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
