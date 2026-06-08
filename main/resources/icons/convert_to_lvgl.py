#!/usr/bin/env python3
"""
Convert PNG weather icons to LVGL C arrays
Requires: pip install pillow

Usage: python convert_to_lvgl.py
Output: C array files in output/ directory
"""

import os
import sys

# Weather code to filename mapping
WEATHER_ICONS = {
    100: "100-fill",   # 晴
    101: "101-fill",   # 多云
    102: "102-fill",   # 少云
    103: "103-fill",   # 晴间多云
    104: "104-fill",   # 阴
    300: "300-fill",   # 阵雨
    302: "302-fill",   # 雷阵雨
    305: "305-fill",   # 小雨
    306: "306-fill",   # 中雨
    307: "307-fill",   # 大雨
    400: "400-fill",   # 小雪
    401: "401-fill",   # 中雪
    402: "402-fill",   # 大雪
    404: "404-fill",   # 雨夹雪
    501: "501-fill",   # 雾
    511: "511-fill",   # 霾
}

ICON_SIZE = 52  # Match PicoPixel design

def png_to_lvgl_c_array(png_path, output_path, var_name):
    """Convert PNG to LVGL C array (RGB565 format)"""
    try:
        from PIL import Image
        
        img = Image.open(png_path)
        
        # Resize to target size
        img = img.resize((ICON_SIZE, ICON_SIZE), Image.LANCZOS)
        
        # Convert to RGBA if needed
        if img.mode != 'RGBA':
            img = img.convert('RGBA')
        
        width, height = img.size
        pixels = list(img.getdata())
        
        # Generate C array content
        c_content = f'''// Generated from {os.path.basename(png_path)}
// Size: {width}x{height}

#include "lvgl.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMG_{var_name.upper()}
#define LV_ATTRIBUTE_IMG_{var_name.upper()}
#endif

static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_{var_name.upper()} uint8_t {var_name}_map[] = {{
'''
        
        # Convert pixels to RGB565
        pixel_data = []
        for r, g, b, a in pixels:
            if a < 128:
                # Transparent pixel
                pixel_data.append("0x00, 0x00")
            else:
                # RGB565 conversion (GC9A01 uses BGR)
                # BGR565: B[4:0] G[5:0] R[4:0]
                b5 = (b >> 3) & 0x1F
                g6 = (g >> 2) & 0x3F
                r5 = (r >> 3) & 0x1F
                color = (b5 << 11) | (g6 << 5) | r5
                pixel_data.append(f"0x{color & 0xFF:02X}, 0x{(color >> 8) & 0xFF:02X}")
        
        # Format output (16 pixels per line)
        for i in range(0, len(pixel_data), 16):
            line = pixel_data[i:i+16]
            c_content += "    " + ", ".join(line) + ",\n"
        
        c_content += f'''
}};

const lv_img_dsc_t {var_name} = {{
    .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
    .header.always_zero = 0,
    .header.reserved = 0,
    .header.w = {width},
    .header.h = {height},
    .data_size = {width * height * 2},
    .data = {var_name}_map,
}};
'''
        
        with open(output_path, 'w', encoding='utf-8') as f:
            f.write(c_content)
        
        return True
        
    except Exception as e:
        print(f"  Error: {e}")
        return False

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    output_dir = os.path.join(script_dir, "output")
    
    os.makedirs(output_dir, exist_ok=True)
    
    try:
        from PIL import Image
    except ImportError:
        print("Error: Pillow not installed")
        print("Install: pip install pillow")
        sys.exit(1)
    
    print("Converting weather icons to LVGL C arrays...")
    print(f"Output: {output_dir}")
    print()
    
    converted = 0
    for code, filename in WEATHER_ICONS.items():
        png_path = os.path.join(script_dir, f"{filename}.png")
        if not os.path.exists(png_path):
            print(f"  Skip: {filename}.png not found")
            continue
        
        var_name = f"weather_{code}"
        output_path = os.path.join(output_dir, f"{var_name}.c")
        
        print(f"  Converting {filename}.png -> {var_name}.c")
        if png_to_lvgl_c_array(png_path, output_path, var_name):
            converted += 1
    
    print()
    print(f"Done! Converted {converted}/{len(WEATHER_ICONS)} icons")
    print()
    print("Next steps:")
    print("1. Copy output/*.c to main/ui/src/ui/images/")
    print("2. Add #include for each file in main/ui/src/ui/images.c")
    print("3. Update display.c to use lv_img_set_src with weather_code_to_icon()")

if __name__ == "__main__":
    main()
