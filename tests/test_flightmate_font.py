#!/usr/bin/env python3
"""Verify that every non-ASCII character used by FlightMate exists in its font."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
FONT_SOURCES = [ROOT / "assets/fonts/flightmate_ui_16.c",
                ROOT / "assets/fonts/flightmate_ui_14.c",
                ROOT / "assets/fonts/flightmate_ui_12.c"]
SOURCE_FILES = [
    ROOT / "main/flightmate_logic.c",
    ROOT / "main/flightmate_ui.c",
    ROOT / "main/flightmate_wifi.c",
    ROOT / "main/flightmate_net.c",
    ROOT / "main/main.c",
    ROOT / "main/airband_stations.c",
]
STRING_PATTERN = re.compile(r'"(?:\\.|[^"\\])*"')


def main() -> int:
    covered = set()
    for font_source in FONT_SOURCES:
        covered |= {int(value, 16)
                    for value in re.findall(r"U\+([0-9A-Fa-f]{4,6})",
                                            font_source.read_text(encoding="utf-8"))}
    missing = {}
    used = set()

    for source in SOURCE_FILES:
        text = source.read_text(encoding="utf-8")
        for literal in STRING_PATTERN.findall(text):
            for char in literal[1:-1]:
                codepoint = ord(char)
                if codepoint < 0x80:
                    continue
                used.add(codepoint)
                if codepoint not in covered:
                    missing.setdefault(source.relative_to(ROOT), set()).add(f"U+{codepoint:04X}")

    if missing:
        for source, points in sorted(missing.items()):
            print(f"{source}: missing {' '.join(sorted(points))}", file=sys.stderr)
        return 1

    print(f"FlightMate font coverage: PASS ({len(used)} non-ASCII code points)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())