#!/usr/bin/env python3
"""
gen_certs_nvs.py — Build the dedicated `certs` NVS partition image and flash it.

This writes the three X.509 PEMs (Amazon Root CA, device client certificate,
device private key) into the `certs` NVS partition so the gateway can connect
to AWS IoT Core with mutual-TLS. Certificates live in their OWN partition, so
you can rotate them without reflashing the firmware.

USAGE
-----
1. Copy your AWS certs into tools/certs/ (see docs/AWS_IOT_SETUP.md):
     tools/certs/AmazonRootCA1.pem
     tools/certs/device_cert.pem
     tools/certs/private_key.pem

2. From an ESP-IDF terminal, run:
     python tools/gen_certs_nvs.py --port COM10

   Optional flags:
     --no-flash         Only generate certs.bin, do not flash.
     --ca FILE          Override Amazon Root CA path.
     --cert FILE        Override client certificate path.
     --key FILE         Override private key path.
     --offset 0xD000    Partition offset of the `certs` partition.
     --bin   certs.bin  Output image path.

The script auto-detects the ESP-IDF python virtualenv (so it can use the
esp_idf_nvs_partition_gen module) and esptool.py on PATH.
"""

import argparse
import glob
import os
import subprocess
import sys

# NVS keys used by certs.c (namespace "mqtt_certs")
CERTS_NAMESPACE = "mqtt_certs"
KEY_CA = "ca_cert"
KEY_CLIENT = "client_cert"
KEY_KEY = "client_key"

DEFAULT_OFFSET = "0xD000"   # recomputed from partitions.csv if present


def find_idf():
    idf = os.environ.get("IDF_PATH")
    if not idf:
        sys.exit("ERROR: $IDF_PATH is not set. Run this from an ESP-IDF terminal "
                 "(or `export.sh` / `Export.bat`).")
    return idf


def _venv_python_of(pat):
    """Return the python.exe inside a venv dir, or None."""
    for c in (os.path.join(pat, "Scripts", "python.exe"),
              os.path.join(pat, "bin", "python")):
        if os.path.isfile(c):
            return os.path.abspath(c)
    return None


def _venv_has_esptool(pat):
    """True if this venv contains an esptool (exe or importable package)."""
    if os.path.isfile(os.path.join(pat, "Scripts", "esptool.exe")):
        return True
    if os.path.isfile(os.path.join(pat, "bin", "esptool")):
        return True
    # package dir inside site-packages
    for sp in glob.glob(os.path.join(pat, "Lib", "site-packages", "esptool*")):
        return True
    return False


def find_venv_python(idf):
    """Locate the ESP-IDF python virtualenv interpreter (esp_idf_nvs_partition_gen)."""
    # 1) Prefer the path exported by the ESP-IDF export script (it has both
    #    esp_idf_nvs_partition_gen AND esptool).
    env = os.environ.get("IDF_PYTHON_ENV_PATH")
    if env:
        py = _venv_python_of(env)
        if py:
            return py

    # 2) Match venvs whose version follows the IDF version, preferring one that
    #    also has esptool (so generate + flash both work from the same venv).
    import re
    base = os.path.normpath(os.path.join(idf, "..", "..", "python_env"))
    m = re.search(r"esp-idf-v(\d+)\.(\d+)", idf)
    want = f"idf{m.group(1)}.{m.group(2)}" if m else None

    matches = [p for p in glob.glob(os.path.join(base, "idf*_py3*_env"))
               if (want is None or want in os.path.basename(p))]

    # prefer venv with esptool, but keep deterministic order
    for pat in sorted(matches, key=lambda p: (not _venv_has_esptool(p), p)):
        py = _venv_python_of(pat)
        if py:
            return py

    # 3) Fallback: any idf* venv.
    for pat in sorted(glob.glob(os.path.join(base, "idf*_py3*_env"))):
        py = _venv_python_of(pat)
        if py:
            return py

    sys.exit("ERROR: could not find the ESP-IDF python venv. Make sure the "
             "ESP-IDF environment is activated (idf.py works).")


def _parse_size(text):
    """Parse a partition size like '0x6000', '24K', '1M' into an int (bytes)."""
    text = text.strip().upper()
    if text.endswith("K"):
        return int(text[:-1]) * 1024
    if text.endswith("M"):
        return int(text[:-1]) * 1024 * 1024
    return int(text, 0)


def resolve_offset(project_dir, idf, venv_py, override):
    if override:
        return override
    # Compute offsets sequentially exactly like gen_esp32part.py does: each
    # partition with a blank offset starts right after the previous one ends.
    # The first partition's default offset is right after the partition table
    # (0x8000 + 0x1000 = 0x9000).
    csv_path = os.path.join(project_dir, "partitions.csv")
    if not os.path.isfile(csv_path):
        return DEFAULT_OFFSET

    rows = []
    for line in open(csv_path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = [c.strip() for c in line.split(",")]
        if len(parts) < 5:
            continue
        name, ptype, subtype, offset, size = parts[0], parts[1], parts[2], parts[3], parts[4]
        rows.append((name, offset, size))

    cur = 0x9000  # first partition starts right after the 4KB partition table
    result = None
    for name, offset, size in rows:
        if offset:
            cur = int(offset, 0)
        if name == "certs":
            result = cur
        cur += _parse_size(size)

    if result is None:
        return DEFAULT_OFFSET
    return "0x%X" % result


def read_pem(path, label):
    if not os.path.isfile(path):
        sys.exit(f"ERROR: {label} not found: {path}")
    with open(path) as f:
        data = f.read()
    if "BEGIN" not in data:
        sys.exit(f"ERROR: {label} does not look like a PEM file: {path}")
    return data


def esptool_cmd(venv_py):
    """Return the esptool command, invoked through the venv python (python -m esptool).
    This is the most reliable way on Windows — esptool is a python package that
    ships inside the ESP-IDF venv, so we never depend on an exe shim or PATH."""
    return [venv_py, "-m", "esptool"]


def write_csv(entries_csv, ca, cert, key):
    # nvs_partition_gen CSV format:
    #   key,type,encoding,value
    # namespace is itself a row. Binary PEMs use type=file, encoding=string.
    rows = [
        "key,type,encoding,value",
        f"{CERTS_NAMESPACE},namespace,,",
        f"{KEY_CA},file,binary,{ca}",
        f"{KEY_CLIENT},file,binary,{cert}",
        f"{KEY_KEY},file,binary,{key}",
    ]
    with open(entries_csv, "w") as f:
        f.write("\n".join(rows) + "\n")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    project_dir = os.path.dirname(here)

    ap = argparse.ArgumentParser(description="Generate & flash the AWS certs NVS partition.")
    ap.add_argument("--port", default="COM10", help="Serial port for flashing (default COM10)")
    ap.add_argument("--no-flash", action="store_true", help="Only generate certs.bin")
    ap.add_argument("--ca", default=os.path.join(here, "certs", "AmazonRootCA1.pem"))
    ap.add_argument("--cert", default=os.path.join(here, "certs", "device_cert.pem"))
    ap.add_argument("--key", default=os.path.join(here, "certs", "private_key.pem"))
    ap.add_argument("--offset", default=None, help="certs partition offset (default: from partitions.csv)")
    ap.add_argument("--bin", default=os.path.join(here, "certs.bin"), help="Output image path")
    args = ap.parse_args()

    idf = find_idf()
    venv_py = find_venv_python(idf)
    offset = resolve_offset(project_dir, idf, venv_py, args.offset)

    ca = read_pem(args.ca, "Amazon Root CA")
    cert = read_pem(args.cert, "client certificate")
    key = read_pem(args.key, "private key")

    entries_csv = os.path.join(here, "certs_entries.csv")
    write_csv(entries_csv, args.ca, args.cert, args.key)

    # Generate the binary partition image (size 0x4000 = 16KB, matches partitions.csv).
    # Default multipage-blob version (2) allows blobs > 4KB, fine for PEMs.
    cmd = [
        venv_py, "-m", "esp_idf_nvs_partition_gen", "generate",
        entries_csv, args.bin, "0x4000",
    ]
    print("Generating certs partition image...")
    print("  ", " ".join(cmd))
    subprocess.check_call(cmd)
    print(f"  -> wrote {args.bin}")

    if args.no_flash:
        print("Skipping flash (--no-flash). Flash manually with:")
        print(f"  esptool.py --port {args.port} write_flash {offset} {args.bin}")
        return

    flash_cmd = esptool_cmd(venv_py) + ["--port", args.port, "write_flash", offset, args.bin]
    print(f"Flashing certs partition to offset {offset} on {args.port}...")
    print("  ", " ".join(flash_cmd))
    try:
        subprocess.check_call(flash_cmd)
    except subprocess.CalledProcessError as e:
        print(f"\nERROR: flashing failed (exit {e.returncode}).")
        print(f"  - If you see 'port is busy'/'Access denied': close the serial monitor")
        print(f"    (idf.py monitor) or any other program using {args.port}, then retry.")
        print(f"  - If you see 'port does not exist': check the USB cable / COM port number.")
        print(f"\nThe image is ready at {args.bin}. You can flash it manually with:")
        print(f"  esptool.py --port {args.port} write_flash {offset} {args.bin}")
        sys.exit(1)
    print("Done. Certificates flashed. Reboot the gateway to connect to AWS.")


if __name__ == "__main__":
    main()
