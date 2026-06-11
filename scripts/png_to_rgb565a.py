#!/usr/bin/env python3
"""
png_to_rgb565a.py — Convert PNG weather icons to raw RGB565+alpha binary format.

SPIFFS target size: 512 KB. All icons are resized to 48x48 to fit.

For each <code>-fill.png in INPUT_DIR, produces <code>-fill.bin in OUTPUT_DIR.
Binary format (little-endian, tightly packed):

    Bytes  0-1:   width  (uint16, little-endian)
    Bytes  2-3:   height (uint16, little-endian)
    Bytes  4+:    pixel data, w*h*3 bytes — for each pixel:
                     byte0 = (RGB565) & 0xFF          (low byte)
                     byte1 = (RGB565 >> 8) & 0xFF     (high byte)
                     byte2 = alpha (0 transparent, 255 opaque)

Usage:
    python png_to_rgb565a.py <INPUT_DIR> <OUTPUT_DIR>
"""

import os
import sys
import struct


# Icon size (must match what display.c expects)
ICON_SIZE = 48

# Night codes that map to day codes in weather_code_to_bin_path() in display.c
# We skip these PNGs because display.c will never open them — it uses the
# daytime variant instead (e.g. code 150 → path /spiffs/100-fill.bin).
NIGHT_CODES = {150, 151, 152, 153, 350, 351, 456, 457}

# SPIFFS partition size limit (see partitions.csv)
SPIFFS_MAX_BYTES = 512 * 1024  # 512 KB


def png_to_rgb565a(png_path: str, bin_path: str, size: int = ICON_SIZE) -> None:
    """Convert a single PNG file to RGB565+alpha binary."""
    try:
        from PIL import Image
    except ImportError:
        print("ERROR: Pillow (PIL) is required. Install with: pip install Pillow",
              file=sys.stderr)
        sys.exit(1)

    img = Image.open(png_path).convert("RGBA")

    # Resize to target size (high-quality downscale)
    if img.size != (size, size):
        img = img.resize((size, size), Image.LANCZOS)

    w, h = img.size
    pixels = img.tobytes("raw", "RGBA")

    out = bytearray()
    out += struct.pack("<HH", w, h)  # header: width, height (uint16 LE)

    # RGB565: RRRRR GGGGGG BBBBB → uint16, then alpha byte
    for i in range(0, len(pixels), 4):
        r = pixels[i]
        g = pixels[i + 1]
        b = pixels[i + 2]
        a = pixels[i + 3]
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out += struct.pack("<H", rgb565)
        out += bytes([a])

    with open(bin_path, "wb") as f:
        f.write(out)

    print(f"  {os.path.basename(png_path)} → {os.path.basename(bin_path)} "
          f"({w}x{h}, {len(out)} bytes)")


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)

    input_dir = sys.argv[1]
    output_dir = sys.argv[2]

    if not os.path.isdir(input_dir):
        print(f"ERROR: input directory not found: {input_dir}", file=sys.stderr)
        sys.exit(1)

    os.makedirs(output_dir, exist_ok=True)

    count = 0
    skipped = 0
    total_bytes = 0

    for fname in sorted(os.listdir(input_dir)):
        if not fname.lower().endswith(".png"):
            continue

        # Skip files that don't look like weather icons (e.g. peiwang.png)
        base = fname[:-4]  # strip .png
        try:
            code = int(base.split("-")[0])
        except ValueError:
            skipped += 1
            print(f"  SKIP {fname} (not a weather code)")
            continue

        # Skip night-code PNGs — display.c maps them to day codes
        if code in NIGHT_CODES:
            skipped += 1
            print(f"  SKIP {fname} (night code, maps to day variant)")
            continue

        png_path = os.path.join(input_dir, fname)
        bin_path = os.path.join(output_dir, base + ".bin")
        png_to_rgb565a(png_path, bin_path)
        total_bytes += os.path.getsize(bin_path)
        count += 1

    print(f"\nConverted {count} PNG file(s) from {input_dir} → {output_dir}")
    print(f"Skipped {skipped} file(s) (night codes / non-weather)")
    print(f"Total binary size: {total_bytes} bytes ({total_bytes/1024:.1f} KB)")
    print(f"SPIFFS limit:      {SPIFFS_MAX_BYTES} bytes ({SPIFFS_MAX_BYTES/1024:.0f} KB)")
    if total_bytes > SPIFFS_MAX_BYTES:
        print(f"WARNING: EXCEEDS SPIFFS by {total_bytes - SPIFFS_MAX_BYTES} bytes!")
        print("  → Reduce ICON_SIZE or remove unused codes.")
        sys.exit(2)
    else:
        print(f"OK: fits in SPIFFS ({SPIFFS_MAX_BYTES - total_bytes} bytes free)")


if __name__ == "__main__":
    main()
