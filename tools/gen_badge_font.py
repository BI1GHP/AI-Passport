#!/usr/bin/env python3
"""Generate the badge UI's LVGL CJK font subsets.

The built-in Source Han Sans subsets shipped with LVGL only cover a small
Chinese character set, so the badge UI needs its own subset.  This tool scans
the C sources for every non-ASCII character used by the UI, rasterises the
glyphs with FreeType and writes two LVGL "fmt_txt" font files (14 px and 16 px).

The generated files are committed so a normal firmware build needs neither
FreeType nor the source font.  Run ``--check`` to verify that the committed
character list still covers the current sources.

Usage:
    python tools/gen_badge_font.py            # regenerate fonts
    python tools/gen_badge_font.py --check    # verify coverage only
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC_DIRS = (ROOT / "main",)
OUT_DIR = ROOT / "main" / "fonts"
CHARS_FILE = OUT_DIR / "badge_font_chars.txt"
COMMON_ZH_FILE = ROOT / "tools" / "_common_zh.txt"   # GB2312 常用字,用于运行时 SSID 等
ASCII_START = 0x20
ASCII_END = 0x7E

DEFAULT_FONT = os.environ.get("BADGE_FONT_SOURCE", r"C:\Windows\Fonts\msyh.ttc")


def source_chars() -> set[str]:
    """Collect ASCII printable plus all non-ASCII characters in main/,
    plus GB2312 common Chinese chars (for runtime SSID etc.)."""
    chars = {chr(c) for c in range(ASCII_START, ASCII_END + 1)}
    for directory in SRC_DIRS:
        for path in sorted(directory.rglob("*")):
            if path.suffix not in (".c", ".h") or OUT_DIR in path.parents:
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError) as exc:
                print(f"warning: cannot read {path}: {exc}", file=sys.stderr)
                continue
            for ch in text:
                cp = ord(ch)
                if cp > ASCII_END and cp <= 0xFFFF and ch not in "\r\n\t":
                    chars.add(ch)
    # GB2312 common Chinese (runtime SSID, etc.)
    if COMMON_ZH_FILE.is_file():
        try:
            zh = COMMON_ZH_FILE.read_text(encoding="utf-8")
            for ch in zh:
                if 0x4E00 <= ord(ch) <= 0x9FFF:
                    chars.add(ch)
        except (OSError, UnicodeDecodeError):
            pass
    return chars


def write_chars(chars: set[str]) -> None:
    ordered = [chr(c) for c in range(ASCII_START, ASCII_END + 1)]
    ordered += sorted((c for c in chars if ord(c) > ASCII_END), key=ord)
    CHARS_FILE.parent.mkdir(parents=True, exist_ok=True)
    CHARS_FILE.write_text(
        "# 电子工牌 UI 字符清单(由 tools/gen_badge_font.py 生成,勿手改)\n"
        + "".join(ordered)
        + "\n",
        encoding="utf-8",
    )


def generated_codepoints(path: Path) -> set[int]:
    text = path.read_text(encoding="utf-8")
    return {int(m, 16) for m in re.findall(r"/\* U\+([0-9A-Fa-f]{4,6}) ", text)}


def check() -> int:
    chars = source_chars()
    want = {ord(c) for c in chars}
    problems: list[str] = []
    if not CHARS_FILE.is_file():
        problems.append(f"missing {CHARS_FILE.relative_to(ROOT)}")
    else:
        listed = {ord(c) for c in CHARS_FILE.read_text(encoding="utf-8") if ord(c) > ASCII_END}
        missing = {chr(c) for c in want - listed if c > ASCII_END}
        if missing:
            problems.append(
                "character list is stale, missing: " + "".join(sorted(missing, key=ord))
            )
    for size in (14, 16):
        path = OUT_DIR / f"badge_font_{size}.c"
        if not path.is_file():
            problems.append(f"missing {path.relative_to(ROOT)}")
            continue
        have = generated_codepoints(path)
        lost = {chr(c) for c in want - have if c > ASCII_END}
        if lost:
            problems.append(
                f"{path.name} lacks: " + "".join(sorted(lost, key=ord))
            )
    if problems:
        for item in problems:
            print(f"ERROR: {item}", file=sys.stderr)
        print("run: python tools/gen_badge_font.py", file=sys.stderr)
        return 1
    print("Badge font coverage: PASS")
    return 0


def rasterise(font_path: Path, size: int, chars: set[str]) -> str:
    import freetype

    face = freetype.Face(str(font_path))
    face.set_char_size(0, size * 64, 0, 72)
    ascender = face.size.ascender >> 6
    descender = -(face.size.descender >> 6)
    line_height = ascender + descender
    base_line = descender

    ordered = [chr(c) for c in range(ASCII_START, ASCII_END + 1)]
    ordered += sorted((c for c in chars if ord(c) > ASCII_END), key=ord)

    bitmaps = bytearray()
    glyphs: list[tuple[int, int, int, int, int, int]] = []
    for ch in ordered:
        face.load_char(ch, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL)
        glyph = face.glyph
        bitmap = glyph.bitmap
        width, rows = bitmap.width, bitmap.rows
        top = glyph.bitmap_top
        left = glyph.bitmap_left
        adv = round(glyph.advance.x / 4)  # 26.6 px -> 8.4 fixed point
        index = len(bitmaps)
        if width and rows:
            pitch = bitmap.pitch
            buf = bitmap.buffer
            nibble = 0
            for y in range(rows):
                base = y * pitch
                for x in range(width):
                    value = min(15, (buf[base + x] + 8) >> 4)
                    if nibble == 0:
                        bitmaps.append(value << 4)
                        nibble = 1
                    else:
                        bitmaps[-1] |= value
                        nibble = 0
        glyphs.append((index, adv, width, rows, left, top - rows))

    # 0x20..0x7E is dense; everything above is sparse.
    ascii_count = ASCII_END - ASCII_START + 1
    extras = ordered[ascii_count:]
    ascii_cmap = (
        "    {\n"
        f"        .range_start = {ASCII_START}, .range_length = {ascii_count}, "
        ".glyph_id_start = 1,\n"
        "        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, "
        ".type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY\n"
        "    }"
    )
    cmap_parts = [ascii_cmap]
    extra_list = ""
    cmap_num = 1
    if extras:
        first = ord(extras[0])
        last = ord(extras[-1])
        rcp = [ord(c) - first for c in extras]
        extra_list = "static const uint16_t unicode_list_1[] = {\n"
        for i in range(0, len(rcp), 8):
            extra_list += "    " + ", ".join(f"0x{v:02x}" for v in rcp[i : i + 8]) + ",\n"
        extra_list += "};\n\n"
        cmap_parts.append(
            "    {\n"
            f"        .range_start = {first}, .range_length = {last - first + 1}, "
            f".glyph_id_start = {ascii_count + 1},\n"
            "        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL, "
            f".list_length = {len(extras)}, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n"
            "    }"
        )
        cmap_num = 2

    out = []
    out.append(
        f"/*\n * Auto-generated by tools/gen_badge_font.py -- do not edit.\n"
        f" * Size: {size} px  Bpp: 4  Source: {font_path.name}\n"
        f" * Characters: {len(ordered)}\n */\n\n"
        '#include "lvgl.h"\n\n'
        f"#ifndef BADGE_FONT_CJK_{size}\n#define BADGE_FONT_CJK_{size} 1\n#endif\n\n"
        f"#if BADGE_FONT_CJK_{size}\n\n"
        "static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n"
    )
    for i, (ch, glyph) in enumerate(zip(ordered, glyphs)):
        index, adv, w, h, left, ofs_y = glyph
        cp = ord(ch)
        name = ch if ch.isprintable() else "?"
        out.append(f'    /* U+{cp:04X} "{name}" */\n')
        if w and h:
            next_index = glyphs[i + 1][0] if i + 1 < len(glyphs) else len(bitmaps)
            data = bitmaps[index:next_index]
            for j in range(0, len(data), 16):
                out.append("    " + ", ".join(f"0x{b:02x}" for b in data[j : j + 16]) + ",\n")
    out.append("};\n\n")

    out.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n")
    out.append("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},\n")
    for index, adv, w, h, left, ofs_y in glyphs:
        out.append(
            f"    {{.bitmap_index = {index}, .adv_w = {adv}, .box_w = {w}, "
            f".box_h = {h}, .ofs_x = {left}, .ofs_y = {ofs_y}}},\n"
        )
    out.append("};\n\n")

    out.append(extra_list)
    out.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {\n")
    out.append(",\n".join(cmap_parts))
    out.append("\n};\n\n")

    out.append(
        "static const lv_font_fmt_txt_dsc_t font_dsc = {\n"
        "    .glyph_bitmap = glyph_bitmap,\n"
        "    .glyph_dsc = glyph_dsc,\n"
        "    .cmaps = cmaps,\n"
        "    .kern_dsc = NULL,\n"
        f"    .kern_scale = 16,\n"
        f"    .cmap_num = {cmap_num},\n"
        "    .bpp = 4,\n"
        "    .kern_classes = 0,\n"
        "    .bitmap_format = 0,\n"
        "};\n\n"
        f"const lv_font_t badge_font_cjk_{size} = {{\n"
        "    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n"
        "    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n"
        f"    .line_height = {line_height},\n"
        f"    .base_line = {base_line},\n"
        f"    .cap_height = {max(1, ascender)},\n"
        f"    .x_height = {max(1, ascender * 2 // 3)},\n"
        "    .subpx = LV_FONT_SUBPX_NONE,\n"
        "    .underline_position = -2,\n"
        "    .underline_thickness = 1,\n"
        "    .static_bitmap = 1,\n"
        "    .dsc = &font_dsc,\n"
        "    .fallback = NULL,\n"
        "    .user_data = NULL,\n"
        "};\n\n"
        f"#endif /* BADGE_FONT_CJK_{size} */\n"
    )
    return "".join(out)


def generate(font_path: Path) -> int:
    if not font_path.is_file():
        print(
            f"ERROR: source font not found: {font_path}\n"
            "hint: pass --font or set BADGE_FONT_SOURCE",
            file=sys.stderr,
        )
        return 1
    try:
        import freetype  # noqa: F401
    except ImportError:
        print("ERROR: freetype-py is required: pip install freetype-py", file=sys.stderr)
        return 1

    chars = source_chars()
    write_chars(chars)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for size in (14, 16):
        text = rasterise(font_path, size, chars)
        (OUT_DIR / f"badge_font_{size}.c").write_text(text, encoding="utf-8")
        print(f"wrote main/fonts/badge_font_{size}.c")
    print(f"characters: {len(chars)} (list: {CHARS_FILE.relative_to(ROOT)})")
    return check()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify coverage only")
    parser.add_argument("--font", type=Path, default=Path(DEFAULT_FONT))
    args = parser.parse_args()
    return check() if args.check else generate(args.font)


if __name__ == "__main__":
    sys.exit(main())
