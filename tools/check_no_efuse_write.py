#!/usr/bin/env python3
"""Fail the build if eFuse write/burn code ends up in the firmware image.

Writing eFuse is irreversible, so the FlightMate firmware must only ever *read*
eFuse (MAC address, ADC calibration, chip revision). The ESP-IDF driver ships a
write path, but it is only kept when something actually references it; with
--gc-sections the linker drops it. This check inspects the GNU ld map so a future
change cannot silently pull the write path back in.

Only the "Linker script and memory map" region is inspected: that is the code
really placed in the image. The "Discarded input sections" block is ignored
(not present in the image) and the "Cross Reference Table" is ignored too (it
lists symbols of discarded sections).

Usage: check_no_efuse_write.py <build-dir>
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

WRITE_MARKERS = (
    "esp_efuse_write",
    "esp_efuse_burn",
    "efuse_utility_burn",
    "esp_efuse_set_rom_log_scheme",
    "ets_efuse_program",
    "ets_efuse_write_key",
    "ets_efuse_clear_program_registers",
)

PLACEMENT_START = "Linker script and memory map"
PLACEMENT_END = ("Discarded input sections", "Cross Reference Table")


def placed_region(text: str) -> str:
    start = text.find(PLACEMENT_START)
    if start < 0:
        raise ValueError("map has no 'Linker script and memory map' section")
    end = len(text)
    for marker in PLACEMENT_END:
        idx = text.find(marker, start + len(PLACEMENT_START))
        if idx >= 0:
            end = min(end, idx)
    return text[start:end]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    build = Path(sys.argv[1])
    maps = sorted(build.glob("*.map"))
    if not maps:
        print(f"ERROR: no .map file found in {build}", file=sys.stderr)
        return 1
    try:
        region = placed_region(maps[0].read_text(encoding="utf-8", errors="replace"))
    except ValueError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    hits = []
    for line in region.splitlines():
        if not any(marker in line for marker in WRITE_MARKERS):
            continue
        # Mask-ROM aliases (0x4000_0000-0x400F_FFFF) come from the linker script
        # and are not part of the firmware image; only flag flash/RAM placement.
        address = re.match(r"\s*(0x[0-9a-fA-F]+)", line)
        if address and 0x40000000 <= int(address.group(1), 16) < 0x40100000:
            continue
        hits.append(line.strip())
    if hits:
        print("ERROR: eFuse write/burn code is linked into the firmware image:", file=sys.stderr)
        for line in hits[:20]:
            print(f"  {line}", file=sys.stderr)
        return 1
    print("OK: firmware image contains eFuse read paths only (no write/burn code).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
