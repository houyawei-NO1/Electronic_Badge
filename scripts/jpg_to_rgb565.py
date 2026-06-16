#!/usr/bin/env python3
"""
jpg_to_rgb565.py — Convert JPG background images to raw RGB565 binary format.

Output format matches LV_IMG_CF_TRUE_COLOR with LV_COLOR_16_SWAP=1:
- 2 bytes per pixel (RGB565)
- Bytes are stored in BIG-ENDIAN order so that LVGL's SWAP
  converts them back to correct little-endian for the display

Binary format (little-endian header, big-endian pixels):
    Bytes  0-1:   width  (uint16, little-endian)
    Bytes  2-3:   height (uint16, little-endian)
    Bytes  4+:    pixel data, w*h*2 bytes — for each pixel:
                     byte0 = (RGB565 >> 8) & 0xFF     (high byte first!)
                     byte1 = (RGB565) & 0xFF          (low byte second!)

Background images are resized to BG_WIDTH x BG_HEIGHT to save RAM.
LVGL zoom scales them to fill the 240x240 screen.

Usage:
    python jpg_to_rgb565.py <INPUT_DIR> <OUTPUT_DIR>
"""

import os
import sys
import struct


# Background image size (120x120, zoom=512 → 240x240 full screen)
# 120x120 RGB565 = ~28.8 KB. ESP32-C3 max free heap block is often ~28KB.
# If malloc fails at runtime, code falls back to 80x80.
BG_WIDTH = 120
BG_HEIGHT = 120


def jpg_to_rgb565(jpg_path: str, bin_path: str, width: int = BG_WIDTH, height: int = BG_HEIGHT) -> None:
    """Convert a single JPG file to RGB565 binary (TRUE_COLOR format, big-endian pixels)."""
    try:
        from PIL import Image
    except ImportError:
        print("ERROR: Pillow (PIL) is required. Install with: pip install Pillow",
              file=sys.stderr)
        sys.exit(1)

    img = Image.open(jpg_path).convert("RGB")

    # Resize to target size (high-quality downscale/upscale)
    if img.size != (width, height):
        img = img.resize((width, height), Image.LANCZOS)

    w, h = img.size
    pixels = img.tobytes("raw", "RGB")

    out = bytearray()
    out += struct.pack("<HH", w, h)  # header: width, height (uint16 LE)

    # RGB565: RRRRR GGGGGG BBBBB → uint16
    # IMPORTANT: PIL on Windows outputs BGR order, so we swap R and B.
    # Also store in BIG-ENDIAN because LVGL with LV_COLOR_16_SWAP=1
    # will swap the bytes when reading. So we pre-swap here.
    for i in range(0, len(pixels), 3):
        b = pixels[i]       # PIL outputs BGR, so pixel[0] is actually B
        g = pixels[i + 1]
        r = pixels[i + 2]   # pixel[2] is actually R
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        # Big-endian: high byte first, low byte second
        out += struct.pack(">H", rgb565)

    with open(bin_path, "wb") as f:
        f.write(out)

    print(f"  {os.path.basename(jpg_path)} → {os.path.basename(bin_path)} "
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
        if not fname.lower().endswith((".jpg", ".jpeg")):
            continue

        jpg_path = os.path.join(input_dir, fname)
        # Generate output filename: bg_1.bin, bg_2.bin, etc.
        base = os.path.splitext(fname)[0]
        bin_path = os.path.join(output_dir, f"bg_{base}.bin")
        jpg_to_rgb565(jpg_path, bin_path)
        total_bytes += os.path.getsize(bin_path)
        count += 1

    print(f"\nConverted {count} JPG file(s) from {input_dir} → {output_dir}")
    print(f"Skipped {skipped} file(s)")
    print(f"Total binary size: {total_bytes} bytes ({total_bytes/1024:.1f} KB)")
    print(f"Each image: {BG_WIDTH}x{BG_HEIGHT} RGB565 = ~{total_bytes//count//1024 if count else 0} KB")
    print(f"zoom = {240 * 256 // BG_WIDTH} (LVGL zoom to fill 240x240 screen)")


if __name__ == "__main__":
    main()
