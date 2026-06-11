#!/usr/bin/env python3
"""
天气图标 PNG 文件烧录脚本
将 PNG 图标打包成 SPIFFS 镜像并烧录到 ESP32

用法:
    python flash_icons.py --port COM3

要求:
    - 安装 mkspiffs 工具 (pip install mkspiffs 或从 https://github.com/igrr/mkspiffs 下载)
    - 安装 esptool (pip install esptool)
"""

import os
import sys
import subprocess
import argparse
import shutil

# 配置
ICONS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "main", "resources", "icons")
SPIFFS_SIZE = 0x80000  # 512KB
SPIFFS_BASE_ADDR = 0x380000  # 分区表中的偏移地址
SPIFFS_BLOCK_SIZE = 4096
SPIFFS_PAGE_SIZE = 256


def find_mkspiffs():
    """查找 mkspiffs 可执行文件"""
    # 先检查是否在 PATH 中
    mkspiffs = shutil.which("mkspiffs")
    if mkspiffs:
        return mkspiffs

    # 检查常见位置
    possible_paths = [
        r"C:\Users\%USERNAME%\.platformio\packages\tool-mkspiffs\mkspiffs.exe",
        r"C:\Espressif\tools\mkspiffs\mkspiffs.exe",
    ]
    for path in possible_paths:
        expanded = os.path.expandvars(path)
        if os.path.exists(expanded):
            return expanded

    return None


def create_spiffs_image(output_path, source_dir):
    """创建 SPIFFS 镜像文件"""
    mkspiffs = find_mkspiffs()
    if not mkspiffs:
        print("错误: 找不到 mkspiffs 工具")
        print("请从以下地址下载并添加到 PATH:")
        print("  https://github.com/igrr/mkspiffs/releases")
        print("  或使用: pip install mkspiffs")
        return False

    cmd = [
        mkspiffs,
        "-c", source_dir,
        "-b", str(SPIFFS_BLOCK_SIZE),
        "-p", str(SPIFFS_PAGE_SIZE),
        "-s", str(SPIFFS_SIZE),
        output_path
    ]

    print(f"创建 SPIFFS 镜像: {output_path}")
    print(f"  源目录: {source_dir}")
    print(f"  大小: {SPIFFS_SIZE // 1024}KB")

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"错误: mkspiffs 失败")
        print(result.stderr)
        return False

    # 检查镜像大小
    img_size = os.path.getsize(output_path)
    print(f"  镜像大小: {img_size // 1024}KB")

    return True


def flash_spiffs(port, image_path):
    """烧录 SPIFFS 镜像到 ESP32"""
    cmd = [
        sys.executable, "-m", "esptool",
        "--port", port,
        "--baud", "921600",
        "write_flash",
        "-z",
        hex(SPIFFS_BASE_ADDR), image_path
    ]

    print(f"\n烧录 SPIFFS 镜像到 ESP32...")
    print(f"  端口: {port}")
    print(f"  地址: {hex(SPIFFS_BASE_ADDR)}")

    result = subprocess.run(cmd)
    return result.returncode == 0


def main():
    parser = argparse.ArgumentParser(description="烧录天气图标到 ESP32 SPIFFS")
    parser.add_argument("--port", default="COM3", help="ESP32 串口 (默认: COM3)")
    parser.add_argument("--icons-dir", default=ICONS_DIR, help="图标目录")
    parser.add_argument("--only-pack", action="store_true", help="只打包，不烧录")
    args = parser.parse_args()

    # 检查图标目录
    if not os.path.exists(args.icons_dir):
        print(f"错误: 图标目录不存在: {args.icons_dir}")
        return 1

    # 列出所有 PNG 文件
    png_files = [f for f in os.listdir(args.icons_dir) if f.endswith('.png')]
    if not png_files:
        print(f"错误: 找不到 PNG 文件: {args.icons_dir}")
        return 1

    print(f"找到 {len(png_files)} 个 PNG 图标:")
    for f in sorted(png_files):
        size = os.path.getsize(os.path.join(args.icons_dir, f))
        print(f"  {f} ({size // 1024}KB)")

    # 创建临时镜像文件
    temp_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build")
    os.makedirs(temp_dir, exist_ok=True)
    image_path = os.path.join(temp_dir, "spiffs_icons.bin")

    # 创建 SPIFFS 镜像
    if not create_spiffs_image(image_path, args.icons_dir):
        return 1

    if args.only_pack:
        print(f"\n镜像已保存到: {image_path}")
        print("使用以下命令手动烧录:")
        print(f"  esptool.py --port {args.port} write_flash -z {hex(SPIFFS_BASE_ADDR)} {image_path}")
        return 0

    # 烧录到 ESP32
    if flash_spiffs(args.port, image_path):
        print("\n烧录成功!")
        return 0
    else:
        print("\n烧录失败!")
        return 1


if __name__ == "__main__":
    sys.exit(main())
