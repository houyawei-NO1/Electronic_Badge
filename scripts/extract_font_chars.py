#!/usr/bin/env python3
"""
extract_font_chars.py — Extract CJK characters from lv_font_simsun_16_cjk.c

Usage:
    python extract_font_chars.py <FONT_C_FILE> <OUTPUT_FILE>

Outputs a plain text file with all Chinese characters found in the font.
"""

import re
import sys


def extract_cjk_chars(font_path: str) -> str:
    """Extract all CJK Unified Ideographs from LVGL font source."""
    with open(font_path, 'r', encoding='utf-8') as f:
        content = f.read()

    # Match: /* U+XXXX "char" */
    pattern = r'/\*\s*U\+([0-9A-F]{4})\s+"(.)"\s*\*/'
    matches = re.findall(pattern, content)

    chinese = []
    for codepoint, char in matches:
        cp = int(codepoint, 16)
        if 0x4E00 <= cp <= 0x9FFF:
            chinese.append(char)

    return ''.join(chinese)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)

    font_path = sys.argv[1]
    output_path = sys.argv[2]

    chars = extract_cjk_chars(font_path)
    print(f"Extracted {len(chars)} Chinese characters")

    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(chars)

    print(f"Saved to {output_path}")


if __name__ == "__main__":
    main()
