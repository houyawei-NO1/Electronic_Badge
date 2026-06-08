#!/usr/bin/env python3
"""
Convert peiwang.png to LVGL C array (RGB565) — replaces hpb3vuwbytl44uh
- Preserves variable name 'hpb3vuwbytl44uh' (UI already references it)
- Flattens alpha onto white background (255,255,255) so image is fully opaque
- Outputs RGB565 format at 400x400 for GC9A01 display
Usage: python convert_peiwang.py
"""
import os
import sys

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow not installed. Run: pip install pillow")
    sys.exit(1)

INPUT_PATH  = r"E:\HouYawei\01.ESP32Project\Electronic_Badge\main\resources\icons\peiwang.png"
OUTPUT_PATH = r"E:\HouYawei\01.ESP32Project\Electronic_Badge\main\ui\src\ui\images\hpb3vuwbytl44uh.c"
VAR_NAME    = "hpb3vuwbytl44uh"
TARGET_W    = 400
TARGET_H    = 400
BG_COLOR    = (255, 255, 255)   # White background for transparent areas

print(f"Opening: {INPUT_PATH}")
img = Image.open(INPUT_PATH)
print(f"  Original: {img.size}, mode={img.mode}")

# Resize to target if needed
if img.size != (TARGET_W, TARGET_H):
    img = img.resize((TARGET_W, TARGET_H), Image.LANCZOS)
    print(f"  Resized to: {img.size}")

# Ensure RGBA
if img.mode != 'RGBA':
    img = img.convert('RGBA')

# Flatten onto white background — produce fully opaque RGB image
bg = Image.new('RGB', (TARGET_W, TARGET_H), BG_COLOR)
bg.paste(img, mask=img.split()[3])   # use alpha channel as mask
img_rgb = bg

pixels = list(img_rgb.getdata())
width, height = img_rgb.size
total  = width * height
print(f"  Flattened to RGB, opaque. Pixels={total}")

# ---- Build C file header ----
c_header = f'''/**
 * @file {VAR_NAME}.c
 * @brief LVGL image asset — RGB565 16-bit color (true color)
 *
 * Converted from peiwang.png to replace PicoPixel placeholder.
 * Alpha channel has been flattened onto a white background so the
 * image is fully opaque and rendered with LV_IMG_CF_TRUE_COLOR.
 *
 * Image: {VAR_NAME}
 * Size:  {width} x {height} pixels
 * CF:    LV_IMG_CF_TRUE_COLOR (RGB565, 16-bit)
 */

#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(__has_include) && __has_include(<lvgl.h>)
    #include <lvgl.h>
#else
    #include "../../lvgl.h"
#endif

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMG_{VAR_NAME.upper()}
#define LV_ATTRIBUTE_IMG_{VAR_NAME.upper()}
#endif

/*
 * Each pixel is stored as two bytes: little-endian RGB565.
 *   byte0 = bits 7..0 of the 16-bit RGB565 value
 *   byte1 = bits 15..8 of the 16-bit RGB565 value
 * LVGL endianness for 16-bit TRUE_COLOR is controlled at compile time
 * by LV_COLOR_16_SWAP; this file uses the default (not swapped) order
 * matching the ESP32 / GC9A01 lvgl configuration.
 */

static const LV_ATTRIBUTE_MEM_ALIGN
       LV_ATTRIBUTE_LARGE_CONST
       LV_ATTRIBUTE_IMG_{VAR_NAME.upper()}
       uint8_t {VAR_NAME}_map[{width * height * 2}] = {{
'''

# ---- Convert pixels to RGB565 little-endian ----
byte_entries = []
for r, g, b in pixels:
    r5 = (r >> 3) & 0x1F
    g6 = (g >> 2) & 0x3F
    b5 = (b >> 3) & 0x1F
    color16 = (r5 << 11) | (g6 << 5) | b5
    lo = color16 & 0xFF
    hi = (color16 >> 8) & 0xFF
    byte_entries.append(f"0x{lo:02X},0x{hi:02X}")

# Format: 16 pixels (32 hex bytes) per source line
lines = []
for i in range(0, len(byte_entries), 16):
    chunk = byte_entries[i:i+16]
    lines.append("    " + ",".join(chunk) + ",")

c_body = "\n".join(lines)

c_footer = f'''
}};

const lv_img_dsc_t {VAR_NAME} = {{
    .header.always_zero = 0,
    .header.w = {width},
    .header.h = {height},
    .data_size = {width * height * 2},
    .header.cf = LV_IMG_CF_TRUE_COLOR,
    .data = {VAR_NAME}_map,
}};
'''

full_content = c_header + c_body + c_footer

print(f"Writing output: {OUTPUT_PATH}")
print(f"  Data size: {width * height * 2} bytes ({(width*height*2)/1024:.1f} KB)")

with open(OUTPUT_PATH, 'w', encoding='utf-8') as f:
    f.write(full_content)

# Quick sanity check — print a snippet of the middle of the file so the user
# can visually confirm non-zero data is present
mid_offset = (total // 2)
r, g, b = pixels[mid_offset]
print(f"  Sample mid-pixel (r,g,b)=({r},{g},{b})")
print("Done!")
