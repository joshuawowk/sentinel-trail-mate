#!/usr/bin/env python3
"""Package a Sentinel build of Trail Mate for the M5Stack Tab5.

Writes, from a build directory configured with -DTRAIL_MATE_SENTINEL=ON:

  <dist>/trail-mate-tab5-sentinel[-vVERSION].bin       merged image: install it with Sentinel
  <dist>/trail-mate-tab5-sentinel-app[-vVERSION].bin   the bare app image (for reference)

The merged image carries the partition table at 0x8000, so Sentinel Bootloader gives
Trail Mate the rows it mounts by label: `nvs`, `ffat` (resource packs, 1 MiB, formatted on
first start) and `coredump`. A bare image would get only an NVS. Install it with

  python3 sentinel-bootloader/tools/sentinelctl.py install <merged image> "Trail Mate"

or copy it to the SD card and use Add -> SD card on the Tab5.

Usage: package_tab5.py [--build build.tab5_sentinel] [--dist dist] [--version X.Y.Z]
Needs esptool (python -m esptool, e.g. from an ESP-IDF environment) and the partition
table tool from $IDF_PATH.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys

REQUIRED_ROWS = {"nvs": (0x01, 0x02), "ffat": (0x01, 0x81), "coredump": (0x01, 0x03)}
APP_TYPE = 0x00
SENTINEL_RESERVED_PROJECTS = {"sentinel-bootloader"}


def read_flash_args(build):
    path = os.path.join(build, "flash_args")
    with open(path) as f:
        lines = [ln.split() for ln in f.read().splitlines() if ln.strip()]
    opts = lines[0]
    files = {os.path.normpath(p): int(off, 16) for off, p in lines[1:]}
    return opts, files


def parse_partition_table(blob):
    """(label, type, subtype, offset, size) rows of an ESP-IDF binary partition table."""
    rows = []
    for i in range(0, len(blob), 32):
        e = blob[i:i + 32]
        if e[:2] != b"\xaa\x50":
            break
        ptype, subtype = e[2], e[3]
        offset = int.from_bytes(e[4:8], "little")
        size = int.from_bytes(e[8:12], "little")
        label = e[12:28].split(b"\0", 1)[0].decode("ascii", "replace")
        rows.append((label, ptype, subtype, offset, size))
    return rows


def app_descriptor(app):
    """project name and version from esp_app_desc_t (first segment, after the headers)."""
    # image header 24 bytes + first segment header 8 bytes, then esp_app_desc_t:
    # magic u32, secure_version u32, reserv1[2] u32, version[32], project_name[32], ...
    d = app[24 + 8:]
    if int.from_bytes(d[:4], "little") != 0xABCD5432:
        sys.exit("trail-mate.bin: no app descriptor in the first segment")
    version = d[16:48].split(b"\0", 1)[0].decode()
    project = d[48:80].split(b"\0", 1)[0].decode()
    return project, version


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default="build.tab5_sentinel")
    ap.add_argument("--dist", default="dist")
    ap.add_argument("--version", help="version suffix for the file names (default: none)")
    args = ap.parse_args()

    sdk = os.path.join(args.build, "sdkconfig.tab5_sentinel")
    if not os.path.exists(sdk):
        sys.exit(f"{args.build} is not a Sentinel build (no sdkconfig.tab5_sentinel); configure it with "
                 "-DTRAIL_MATE_IDF_TARGET=tab5 -DTRAIL_MATE_SENTINEL=ON")

    opts, files = read_flash_args(args.build)
    app_path = os.path.join(args.build, "trail-mate.bin")
    table_path = os.path.join(args.build, "partition_table", "partition-table.bin")
    with open(app_path, "rb") as f:
        app = f.read()
    with open(table_path, "rb") as f:
        table = f.read()

    if app[:1] != b"\xe9":
        sys.exit("trail-mate.bin does not start with an ESP image header (0xE9)")
    project, version = app_descriptor(app)
    # Sentinel's app information (third_party/sentinel_app): magic "SNAP", format 1, 256 bytes.
    magic, fmt, size = struct.unpack_from("<IHH", app, 0x120)
    if (magic, fmt, size) != (0x50414E53, 1, 256):
        sys.exit("no Sentinel app information at 0x120 (SENTINEL_APP_INFO missing or not kept)")
    repo = app[0x128:0x128 + 120].split(b"\0", 1)[0].decode()
    asset = app[0x1A0:0x1A0 + 64].split(b"\0", 1)[0].decode()
    if project in SENTINEL_RESERVED_PROJECTS:
        sys.exit(f"project name {project!r} is reserved by Sentinel")

    rows = parse_partition_table(table)
    labels = {r[0]: r for r in rows}
    for label, (ptype, subtype) in REQUIRED_ROWS.items():
        r = labels.get(label)
        if r is None or (r[1], r[2]) != (ptype, subtype):
            sys.exit(f"partition table lacks the {label} row Trail Mate mounts")
    apps = [r for r in rows if r[1] == APP_TYPE]
    if len(apps) != 1:
        sys.exit(f"expected one app row (Sentinel installs it as ota_0), found {len(apps)}")
    app_row = apps[0]
    if len(app) > app_row[4]:
        sys.exit(f"image ({len(app)} B) is larger than its app row ({app_row[4]} B)")

    os.makedirs(args.dist, exist_ok=True)
    suffix = f"-v{args.version}" if args.version else ""
    merged = os.path.join(args.dist, f"trail-mate-tab5-sentinel{suffix}.bin")
    bare = os.path.join(args.dist, f"trail-mate-tab5-sentinel-app{suffix}.bin")

    # Bootloader, table and app at the offsets the build used (the otadata image is left
    # out: Sentinel writes its own, and a blank one selects ota_0 when flashed alone).
    parts = []
    for rel, off in sorted(files.items(), key=lambda kv: kv[1]):
        if os.path.basename(rel) == "ota_data_initial.bin":
            continue
        parts += [hex(off), os.path.join(args.build, rel)]
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32p4", "merge_bin", "-o", merged, *opts, *parts]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    with open(bare, "wb") as f:
        f.write(app)

    with open(merged, "rb") as f:
        m = f.read()
    if m[0x8000:0x8002] != b"\xaa\x50":
        sys.exit("merged image has no partition table at 0x8000")
    for path in (merged, bare):
        with open(path, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        print(f"{digest}  {path}  ({os.path.getsize(path)} bytes)")
    print(f"updates from {repo} asset {asset or '(any)'}")
    print(f"project {project} version {version}; rows: " +
          ", ".join(f"{r[0]} {r[4] // 1024} KiB" for r in rows))


if __name__ == "__main__":
    main()
