#!/usr/bin/env python3
"""
将带透明度的 PNG 图片转换为 LVGL C 数组格式（LVGL 9.x 兼容）
使用 ARGB8888 格式以支持透明度
"""
from PIL import Image
import sys

def convert_image_to_lvgl_c(input_path, output_path, var_name="smile_img", target_size=200):
    # 打开图片
    img = Image.open(input_path)
    
    # 调整大小
    img = img.resize((target_size, target_size), Image.Resampling.LANCZOS)
    
    # 确保是 RGBA 模式
    if img.mode != 'RGBA':
        img = img.convert('RGBA')
    
    width, height = img.size
    
    # 转换为 ARGB8888 数据
    pixels = []
    for y in range(height):
        for x in range(width):
            r, g, b, a = img.getpixel((x, y))
            # ARGB8888: Alpha Red Green Blue (4 bytes per pixel)
            argb8888 = (a << 24) | (r << 16) | (g << 8) | b
            pixels.append(argb8888)
    
    # 生成 C 代码
    stride = width * 4  # ARGB8888 = 4 bytes per pixel
    with open(output_path, 'w') as f:
        f.write(f'// Auto-generated from {input_path}\n')
        f.write(f'// Size: {width}x{height}, Format: ARGB8888\n')
        f.write(f'// LVGL 9.x compatible format with transparency\n\n')
        f.write('#include "lvgl.h"\n\n')
        f.write(f'// Image data (ARGB8888 format)\n')
        f.write(f'static const uint32_t {var_name}_map[{len(pixels)}] = {{\n')
        
        # 写入像素数据，每行8个
        for i in range(0, len(pixels), 8):
            chunk = pixels[i:i+8]
            hex_vals = ', '.join(f'0x{p:08X}' for p in chunk)
            f.write(f'    {hex_vals},\n')
        
        f.write('};\n\n')
        
        # 生成 LVGL 9.x 图片描述符
        f.write(f'const lv_image_dsc_t {var_name} = {{\n')
        f.write(f'    .header.magic = LV_IMAGE_HEADER_MAGIC,\n')
        f.write(f'    .header.cf = LV_COLOR_FORMAT_ARGB8888,\n')
        f.write(f'    .header.flags = 0,\n')
        f.write(f'    .header.w = {width},\n')
        f.write(f'    .header.h = {height},\n')
        f.write(f'    .header.stride = {stride},\n')
        f.write(f'    .data_size = sizeof({var_name}_map),\n')
        f.write(f'    .data = (const uint8_t *){var_name}_map,\n')
        f.write(f'}};\n')
    
    print(f"✅ Converted {input_path} → {output_path}")
    print(f"   Size: {width}x{height}")
    print(f"   Data: {len(pixels) * 4} bytes ({len(pixels) * 4 / 1024:.1f} KB)")

if __name__ == "__main__":
    input_file = "main/ui/smile.png"
    output_file = "main/ui/smile_img.c"
    convert_image_to_lvgl_c(input_file, output_file, "smile_img", target_size=360)
