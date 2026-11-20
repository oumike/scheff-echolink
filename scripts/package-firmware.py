#!/usr/bin/env python3
"""Copy a board's images to dist/ under their release names.

- scheff-echolink-<env>-v<ver>.bin: the merged factory image (bootloader,
  partition table and app) that flashes at 0x0 with esptool.
- scheff-echolink-<env>-v<ver>-app.bin: the app alone, for 0x10000. This is
  the one bmorcelli's Launcher (LauncherHub) installs beside itself.

Both are checked: the bootloader and partition table must sit where the
board's sdkconfig says, and the app must be an ESP image that fits the
factory slot.

Usage: package-firmware.py <env> <pio build dir for env> <out dir>
"""

import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP_OFFSET = 0x10000
APP_SLOT = 6 * 1024 * 1024  # factory partition in partitions.csv


def sdkconfig_hex(env, key):
    text = (ROOT / f"sdkconfig.{env}").read_text()
    match = re.search(rf"^CONFIG_{key}=(0x[0-9a-fA-F]+)$", text, re.M)
    if not match:
        sys.exit(f"CONFIG_{key} not found in sdkconfig.{env}")
    return int(match.group(1), 16)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    env, build_dir, out_dir = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    version = (ROOT / "VERSION").read_text().split()[0]
    factory = build_dir / "firmware.factory.bin"
    app = build_dir / "firmware.bin"
    data = factory.read_bytes()
    app_data = app.read_bytes()

    bootloader = sdkconfig_hex(env, "BOOTLOADER_OFFSET_IN_FLASH")
    partitions = sdkconfig_hex(env, "PARTITION_TABLE_OFFSET")
    if data[bootloader:bootloader + 1] != b"\xe9":
        sys.exit(f"{factory}: no bootloader image at {bootloader:#x}")
    if data[partitions:partitions + 2] != b"\xaa\x50":
        sys.exit(f"{factory}: no partition table at {partitions:#x}")
    if app_data[:1] != b"\xe9" or len(app_data) > APP_SLOT:
        sys.exit(f"{app}: not an app image, or larger than the {APP_SLOT // 1024} KB slot")
    if data[APP_OFFSET:APP_OFFSET + len(app_data)] != app_data:
        sys.exit(f"{factory}: the app at {APP_OFFSET:#x} differs from {app}")

    out_dir.mkdir(parents=True, exist_ok=True)
    base = f"scheff-echolink-{env}-v{version}"
    for src, name in ((factory, f"{base}.bin"), (app, f"{base}-app.bin")):
        shutil.copyfile(src, out_dir / name)
        print(f"{out_dir / name} ({src.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
