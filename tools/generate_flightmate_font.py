#!/usr/bin/env python3
"""Generate the FlightMate LVGL font subset from a licensed CJK font.

Environment overrides:
  NODE_EXE        Path to node.exe
  LV_FONT_CONV    Path to lv_font_conv.js
  NOTO_CJK_FONT   Path to NotoSansCJKsc-Regular.otf
"""

from pathlib import Path
import os
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE_FILES = [
    ROOT / "main/flightmate_logic.c",
    ROOT / "main/flightmate_ui.c",
    ROOT / "main/flightmate_wifi.c",
    ROOT / "main/flightmate_net.c",
    ROOT / "main/airband_stations.c",
    ROOT / "main/main.c",
]
STRING_PATTERN = re.compile(r'"(?:\\.|[^"\\])*"')
# lv_font_conv reuses these internal names in every font; when two subsets are
# compiled into one translation unit they collide, so suffix them per size.
INTERNAL_SYMBOLS = (
    "glyph_bitmap", "glyph_dsc", "unicode_list_0", "unicode_list_1",
    "unicode_list_2", "unicode_list_3", "cmaps", "kern_left_class_mapping",
    "kern_right_class_mapping", "kern_class_values", "kern_classes", "cache",
    "font_dsc",
)
OUTPUTS = [(16, ROOT / "assets/fonts/flightmate_ui_16.c"),
           (14, ROOT / "assets/fonts/flightmate_ui_14.c"),
           (12, ROOT / "assets/fonts/flightmate_ui_12.c")]


def source_characters() -> str:
    characters = {chr(value) for value in range(0x20, 0x7F)}
    for source in SOURCE_FILES:
        for literal in STRING_PATTERN.findall(source.read_text(encoding="utf-8")):
            characters.update(literal[1:-1])
    return "".join(sorted(characters - set(chr(value) for value in range(0x20, 0x7F))))


def main() -> int:
    node = os.environ.get("NODE_EXE")
    converter = os.environ.get("LV_FONT_CONV")
    font = os.environ.get("NOTO_CJK_FONT")
    if not node or not converter or not font:
        print("Set NODE_EXE, LV_FONT_CONV, and NOTO_CJK_FONT.", file=sys.stderr)
        return 2

    for size, output in OUTPUTS:
        name = f"flightmate_ui_{size}"
        command = [
            node,
            converter,
            "--font", font,
            "--range", "0x20-0x7E",
            "--symbols", source_characters(),
            "--size", str(size),
            "--bpp", "4",
            "--format", "lvgl",
            "--lv-font-name", name,
            "--lv-include", "lvgl.h",
            "--force-fast-kern-format",
            "--no-compress",
            "--no-prefilter",
            "-o", str(output),
        ]
        subprocess.run(command, check=True)
        generated = output.read_text(encoding="utf-8")
        header = (
            "/*******************************************************************************\n"
            f" * FlightMate UI font subset ({size} px)\n"
            " * Source: Noto Sans CJK SC Regular (SIL Open Font License 1.1)\n"
            f" * Size: {size} px, 4 bpp, no compression\n"
            " * Regenerate with tools/generate_flightmate_font.py\n"
            " ******************************************************************************/"
        )
        generated = re.sub(r"/\*.*?\*/", header, generated, count=1, flags=re.DOTALL)
        for symbol in INTERNAL_SYMBOLS:
            # keep struct member designators (.glyph_bitmap) untouched
            generated = re.sub(r"(?<![.\w])%s\b" % re.escape(symbol),
                                f"{symbol}_{size}", generated)
        output.write_text(generated, encoding="utf-8")
        print(f"Generated {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())