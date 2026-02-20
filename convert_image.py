#!/usr/bin/env python3
"""
将 PNG 图片转换为 LVGL C 数组格式（LVGL 9.x 兼容）
"""
from PIL import Image
import sys

def convert_image_to_lvgl_c(input_path, output_path, var_name="background_img"):
    # 打开图片
    img = Image.open(input_path)
    
    # 调整大小到 360x360
    img = img.resize((360, 360), Image.Resampling.LANCZOS)
    
    # 转换为 RGB565 格式
    img_rgb = img.convert('RGB')
    width, height = img_rgb.size
    
    # 转换为 RGB565 数据
    pixels = []
    for y in range(height):
        for x in range(width):
            r, g, b = img_rgb.getpixel((x, y))
            # RGB565: RRRRR GGGGGG BBBBB
            rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            pixels.append(rgb565)
    
    # 生成 C 代码
    stride = width * 2  # RGB565 = 2 bytes per pixel
    with open(output_path, 'w') as f:
        f.write(f'// Auto-generated from {input_path}\n')
        f.write(f'// Size: {width}x{height}, Format: RGB565\n')
        f.write(f'// LVGL 9.x compatible format\n\n')
        f.write('#include "lvgl.h"\n\n')
        f.write(f'// Image data (RGB565 format)\n')
        f.write(f'static const uint16_t {var_name}_map[{len(pixels)}] = {{\n')
        
        # 写入像素数据，每行16个
        for i in range(0, len(pixels), 16):
            chunk = pixels[i:i+16]
            hex_vals = ', '.join(f'0x{p:04X}' for p in chunk)
            f.write(f'    {hex_vals},\n')
        
        f.write('};\n\n')
        
        # 生成 LVGL 9.x 图片描述符
        f.write(f'const lv_image_dsc_t {var_name} = {{\n')
        f.write(f'    .header.magic = LV_IMAGE_HEADER_MAGIC,\n')
        f.write(f'    .header.cf = LV_COLOR_FORMAT_RGB565,\n')
        f.write(f'    .header.flags = 0,\n')
        f.write(f'    .header.w = {width},\n')
        f.write(f'    .header.h = {height},\n')
        f.write(f'    .header.stride = {stride},\n')
        f.write(f'    .data_size = sizeof({var_name}_map),\n')
        f.write(f'    .data = (const uint8_t *){var_name}_map,\n')
        f.write(f'}};\n')
    
    print(f"✅ Converted {input_path} → {output_path}")
    print(f"   Size: {width}x{height}")
    print(f"   Data: {len(pixels) * 2} bytes ({len(pixels) * 2 / 1024:.1f} KB)")

if __name__ == "__main__":
    input_file = "main/ui/bg.png"
    output_file = "main/ui/background_img.c"
    convert_image_to_lvgl_c(input_file, output_file, "background_img")
