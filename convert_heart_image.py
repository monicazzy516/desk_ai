#!/usr/bin/env python3
"""
将 heart.png 转换为 LVGL 9.x 兼容的 C 数组（ARGB8888 格式，支持透明度）
"""

from PIL import Image
import sys

def convert_image_to_lvgl_c(input_file, output_file, var_name, target_size=80):
    """
    将 PNG 图片转换为 LVGL 9.x 的 C 数组格式
    
    Args:
        input_file: 输入的 PNG 文件路径
        output_file: 输出的 C 文件路径
        var_name: C 数组的变量名
        target_size: 目标尺寸（宽高相同）
    """
    # 打开图片
    img = Image.open(input_file)
    
    # 转换为 RGBA 模式（确保有 alpha 通道）
    img = img.convert("RGBA")
    
    # 调整大小（保持纵横比）
    img.thumbnail((target_size, target_size), Image.Resampling.LANCZOS)
    
    # 创建目标尺寸的透明背景图片
    result = Image.new("RGBA", (target_size, target_size), (0, 0, 0, 0))
    
    # 将调整后的图片居中粘贴
    offset = ((target_size - img.width) // 2, (target_size - img.height) // 2)
    result.paste(img, offset)
    
    width, height = result.size
    pixels = result.load()
    
    # 生成 C 代码
    with open(output_file, 'w') as f:
        # 写入头部注释
        f.write(f"/* Generated from {input_file} */\n")
        f.write(f"/* Size: {width}x{height}, Format: ARGB8888 */\n\n")
        
        f.write("#include \"lvgl.h\"\n\n")
        
        # 写入像素数据（ARGB8888 格式：每个像素 4 字节）
        total_pixels = width * height
        f.write(f"static const uint32_t {var_name}_data[{total_pixels}] = {{\n")
        
        for y in range(height):
            f.write("    ")
            for x in range(width):
                r, g, b, a = pixels[x, y]
                # ARGB8888 格式：A|R|G|B（大端序）
                argb = (a << 24) | (r << 16) | (g << 8) | b
                f.write(f"0x{argb:08x}")
                
                if y < height - 1 or x < width - 1:
                    f.write(", ")
                
                if (x + 1) % 8 == 0 and x < width - 1:
                    f.write("\n    ")
            
            if y < height - 1:
                f.write("\n")
        
        f.write("\n};\n\n")
        
        # 写入 LVGL 9.x 图像描述符
        f.write(f"const lv_image_dsc_t {var_name} = {{\n")
        f.write(f"    .header = {{\n")
        f.write(f"        .magic = LV_IMAGE_HEADER_MAGIC,\n")
        f.write(f"        .cf = LV_COLOR_FORMAT_ARGB8888,\n")
        f.write(f"        .flags = 0,\n")
        f.write(f"        .w = {width},\n")
        f.write(f"        .h = {height},\n")
        f.write(f"        .stride = {width * 4},\n")
        f.write(f"    }},\n")
        f.write(f"    .data_size = sizeof({var_name}_data),\n")
        f.write(f"    .data = (const uint8_t *){var_name}_data,\n")
        f.write(f"}};\n")
    
    print(f"✓ 转换完成: {output_file}")
    print(f"  尺寸: {width}x{height}")
    print(f"  格式: ARGB8888")
    print(f"  大小: ~{total_pixels * 4 / 1024:.1f} KB")

if __name__ == "__main__":
    input_file = "main/ui/heart.png"
    output_file = "main/ui/heart_img.c"
    convert_image_to_lvgl_c(input_file, output_file, "heart_img", target_size=80)
