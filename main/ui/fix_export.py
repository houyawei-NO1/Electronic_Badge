#!/usr/bin/env python3
"""
PicoPixel UI Export Fix Script
==============================

每次从 PicoPixel 导出 UI 后运行此脚本，自动修复：
1. 头文件路径: <lvgl/lvgl.h> -> "lvgl.h"
2. 颜色格式: 32位 ARGB -> 16位 RGB565

使用方法:
    cd main/ui
    python fix_export.py

作者: AI Assistant
日期: 2026-06-06
"""

import os
import re
import sys

# 配置
UI_DIR = os.path.dirname(os.path.abspath(__file__))
SRC_DIR = os.path.join(UI_DIR, "src", "ui")

# 颜色映射表: 32位 ARGB -> 16位 RGB565
# 格式: 0xAARRGGBB -> 0xRGB565
COLOR_MAP = {
    # 背景/深色系
    "0xff000000": "0x0000",  # 纯黑
    "0xff1a1a1a": "0x1082",  # 近黑
    "0xff2a2a2a": "0x18e3",  # 深灰
    "0xff333333": "0x1ce7",  # 深灰
    "0xff444444": "0x2228",  # 中深灰
    "0xff555555": "0x2a69",  # 中灰
    "0xff666666": "0x318c",  # 中灰
    "0xff808080": "0x4208",  # 灰色
    "0xff999999": "0x4a49",  # 浅灰
    "0xffaaaaaa": "0x52aa",  # 浅灰
    "0xffcccccc": "0x6739",  # 很浅灰
    "0xffdddddd": "0x6f7b",  # 很浅灰
    "0xffeeeeee": "0x7bde",  # 近白
    "0xffffffff": "0xffff",  # 纯白

    # 红色系
    "0xffff0000": "0xf800",  # 纯红
    "0xffcc0000": "0xc000",  # 深红
    "0xffaa0000": "0xa000",  # 暗红
    "0xffff3333": "0xf8a0",  # 亮红
    "0xffff6666": "0xfcc6",  # 浅红
    "0xffff9999": "0xfe73",  # 粉红
    "0xffffcccc": "0xff9c",  # 很浅红

    # 绿色系
    "0xff00ff00": "0x07e0",  # 纯绿
    "0xff00cc00": "0x0600",  # 深绿
    "0xff00aa00": "0x0540",  # 暗绿
    "0xff33ff33": "0x07e6",  # 亮绿
    "0xff66ff66": "0x07ef",  # 浅绿
    "0xff99ff99": "0x0775",  # 粉绿
    "0xffccffcc": "0x07fc",  # 很浅绿

    # 蓝色系
    "0xff0000ff": "0x001f",  # 纯蓝
    "0xff0000cc": "0x0018",  # 深蓝
    "0xff0000aa": "0x0015",  # 暗蓝
    "0xff3333ff": "0x041f",  # 亮蓝
    "0xff6666ff": "0x0c5f",  # 浅蓝
    "0xff9999ff": "0x147f",  # 粉蓝
    "0xffccccff": "0x1c9f",  # 很浅蓝

    # 黄色系
    "0xffffff00": "0xffe0",  # 纯黄
    "0xffcccc00": "0xce00",  # 深黄
    "0xffaaaa00": "0xa540",  # 暗黄
    "0xffffff33": "0xffe6",  # 亮黄
    "0xffffff66": "0xfff6",  # 浅黄
    "0xffffff99": "0xfffb",  # 粉黄
    "0xffffffcc": "0xfffe",  # 很浅黄

    # 青色系
    "0xff00ffff": "0x07ff",  # 纯青
    "0xff00cccc": "0x0666",  # 深青
    "0xff00aaaa": "0x0555",  # 暗青
    "0xff33ffff": "0x07ff",  # 亮青
    "0xff66ffff": "0x0fff",  # 浅青
    "0xff99ffff": "0x17ff",  # 粉青
    "0xffccffff": "0x1fff",  # 很浅青

    # 品红/紫色系
    "0xffff00ff": "0xf81f",  # 纯品红
    "0xffcc00cc": "0xc018",  # 深品红
    "0xffaa00aa": "0xa015",  # 暗品红
    "0xffff33ff": "0xf81f",  # 亮品红
    "0xffff66ff": "0xfc5f",  # 浅品红
    "0xffff99ff": "0xfe7f",  # 粉品红
    "0xffffccff": "0xff9f",  # 很浅品红

    # 橙色/棕色系
    "0xffff6600": "0xfc60",  # 橙
    "0xffff8800": "0xfd20",  # 亮橙
    "0xffffaa00": "0xfe80",  # 金黄
    "0xffffcc00": "0xff00",  # 金黄
    "0xffcc6600": "0xcc20",  # 棕橙
    "0xff996600": "0x9b20",  # 棕色
    "0xff663300": "0x6180",  # 深棕
    "0xff48252a": "0x4924",  # 深红棕 (badge_loading 背景弧)
    "0xffea3756": "0xe73a",  # 橙红 (badge_loading 指示器)

    # 特殊颜色
    "0xff4CAF50": "0x27e4",  # Material 绿色
    "0xff2196F3": "0x21d7",  # Material 蓝色
    "0xffFF9800": "0xfc80",  # Material 橙色
    "0xffF44336": "0xf8e4",  # Material 红色
    "0xff9C27B0": "0x911a",  # Material 紫色
    "0xff00BCD4": "0x04d9",  # Material 青色
    "0xffFFEB3B": "0xff75",  # Material 黄色
    "0xff795548": "0x4aa5",  # Material 棕色
    "0xff607D8B": "0x3bcd",  # Material 蓝灰
    "0xffE91E63": "0xe8e4",  # Material 粉红
    "0xff3F51B5": "0x3a75",  # Material 靛蓝
    "0xff009688": "0x04b5",  # Material 蓝绿
    "0xffCDDC39": "0xced3",  # Material 黄绿
    "0xffFF5722": "0xfbc4",  # Material 深橙
    "0xff673AB7": "0x69d7",  # Material 深紫
    "0xff03A9F4": "0x051f",  # Material 浅蓝
    "0xff8BC34A": "0x8de4",  # Material 浅绿
    "0xffFFC107": "0xff03",  # Material 琥珀
    "0xff9E9E9E": "0x9cd3",  # Material 灰色
    "0xff90CAF9": "0x967c",  # Material 浅蓝 (天气文字)
}


def fix_headers():
    """修复头文件引用: <lvgl/lvgl.h> -> lvgl.h"""
    fixed_count = 0
    header_files = []

    # 查找所有 .h 文件
    for root, dirs, files in os.walk(SRC_DIR):
        for file in files:
            if file.endswith('.h'):
                header_files.append(os.path.join(root, file))

    print(f"找到 {len(header_files)} 个头文件")

    for filepath in header_files:
        with open(filepath, 'r', encoding='utf-8') as f:
            content = f.read()

        if '<lvgl/lvgl.h>' in content:
            new_content = content.replace('<lvgl/lvgl.h>', '"lvgl.h"')
            with open(filepath, 'w', encoding='utf-8') as f:
                f.write(new_content)
            print(f"  ✓ 修复: {os.path.relpath(filepath, UI_DIR)}")
            fixed_count += 1

    print(f"\n头文件修复完成: {fixed_count} 个文件已修改")
    return fixed_count


def convert_color_32_to_16(color_32_str):
    """
    将 32位 ARGB 颜色转换为 16位 RGB565
    输入: "0xffRRGGBB"
    输出: "0xRGB565"
    """
    # 去掉 0x 前缀和 alpha 通道
    hex_str = color_32_str.lower().replace('0x', '')
    if len(hex_str) == 8:
        hex_str = hex_str[2:]  # 去掉 alpha (ff)

    # 提取 RGB
    r = int(hex_str[0:2], 16)
    g = int(hex_str[2:4], 16)
    b = int(hex_str[4:6], 16)

    # 转换为 RGB565
    r5 = (r >> 3) & 0x1F
    g6 = (g >> 2) & 0x3F
    b5 = (b >> 3) & 0x1F

    rgb565 = (r5 << 11) | (g6 << 5) | b5

    return f"0x{rgb565:04x}"


def fix_colors():
    """修复颜色格式: 32位 ARGB -> 16位 RGB565"""
    fixed_count = 0
    source_files = []

    # 查找所有 .c 文件
    for root, dirs, files in os.walk(SRC_DIR):
        for file in files:
            if file.endswith('.c'):
                source_files.append(os.path.join(root, file))

    print(f"\n找到 {len(source_files)} 个源文件")

    # 匹配 lv_color_hex(0xffxxxxxx) 或 lv_color_hex(0xFFxxxxxx)
    color_pattern = re.compile(r'lv_color_hex\((0x[0-9a-fA-F]{8})\)')

    for filepath in source_files:
        with open(filepath, 'r', encoding='utf-8') as f:
            content = f.read()

        original_content = content
        matches = color_pattern.findall(content)

        if matches:
            for color_32 in matches:
                color_32_lower = color_32.lower()

                # 先查映射表
                if color_32_lower in COLOR_MAP:
                    color_16 = COLOR_MAP[color_32_lower]
                else:
                    # 自动转换
                    color_16 = convert_color_32_to_16(color_32)
                    print(f"  ! 未映射颜色 {color_32} -> {color_16} (自动转换)")

                content = content.replace(f'lv_color_hex({color_32})',
                                          f'lv_color_hex({color_16})')

            if content != original_content:
                with open(filepath, 'w', encoding='utf-8') as f:
                    f.write(content)
                print(f"  ✓ 修复: {os.path.relpath(filepath, UI_DIR)} ({len(matches)} 个颜色)")
                fixed_count += 1

    print(f"\n颜色修复完成: {fixed_count} 个文件已修改")
    return fixed_count


def main():
    """主函数"""
    print("=" * 60)
    print("PicoPixel UI Export Fix Tool")
    print("=" * 60)
    print(f"UI 目录: {UI_DIR}")
    print()

    # 检查目录是否存在
    if not os.path.exists(SRC_DIR):
        print(f"错误: 找不到源文件目录 {SRC_DIR}")
        print("请确保在 main/ui 目录下运行此脚本")
        sys.exit(1)

    # 修复头文件
    print("【步骤 1/2】修复头文件引用...")
    header_fixed = fix_headers()

    # 修复颜色
    print("\n【步骤 2/2】修复颜色格式...")
    color_fixed = fix_colors()

    # 总结
    print("\n" + "=" * 60)
    print("修复完成!")
    print("=" * 60)
    print(f"  头文件修复: {header_fixed} 个")
    print(f"  颜色修复:   {color_fixed} 个")
    print()
    print("现在可以编译了:")
    print("  idf.py build")
    print()


if __name__ == '__main__':
    main()
