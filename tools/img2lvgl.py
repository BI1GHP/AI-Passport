#!/usr/bin/env python3
# tools/img2lvgl.py —— 把人物场景图转成 LVGL I8(8bit 索引色)图片资源。
# 用法: python tools/img2lvgl.py <输入图片> <输出.c> <宽> <高> <符号名> [裁剪框L,T,R,B]
# I8 = 256 色调色板(每项 4 字节 BGRA) + 每像素 1 字节索引,体积为 RGB565 的一半。
import os
import sys

from PIL import Image


def main() -> int:
    if len(sys.argv) < 6:
        print("usage: img2lvgl.py <in> <out.c> <w> <h> <symbol> [cropL,cropT,cropR,cropB]")
        return 1
    src, out, w, h, sym = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
    img = Image.open(src).convert("RGB")
    if len(sys.argv) > 6:
        box = tuple(int(v) for v in sys.argv[6].split(","))
        img = img.crop(box)
    else:
        img = img.copy()
    img = img.resize((w, h), Image.LANCZOS)
    # 自适应 256 色调色板 + Floyd-Steinberg 抖动,小尺寸屏幕上观感接近原图
    q = img.quantize(colors=256, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG)
    palette = q.getpalette()[: 256 * 3]  # RGBRGB...
    pixels = q.tobytes()

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write(f"// {os.path.basename(out)} —— 由 tools/img2lvgl.py 自动生成,勿手改。\n")
        f.write(f"// 源图: {os.path.basename(src)} → {w}x{h} I8(256 色索引,调色板 BGRA + 索引)\n")
        f.write('#include "xz_portrait.h"\n\n')
        f.write(f"static const uint8_t {sym}_map[] = {{\n")
        # 调色板:LVGL v9 索引色每项 4 字节,内存序 B,G,R,A;之后是每像素 1 字节索引
        data = bytearray()
        for i in range(256):
            r, g, b = palette[i * 3], palette[i * 3 + 1], palette[i * 3 + 2]
            data += bytes((b, g, r, 0xFF))
        data += pixels
        for off in range(0, len(data), 16):
            chunk = data[off : off + 16]
            f.write("".join(f"0x{v:02x}," for v in chunk) + "\n")
        f.write("};\n\n")
        f.write(f"const lv_image_dsc_t {sym} = {{\n")
        f.write("    .header.cf = LV_COLOR_FORMAT_I8,\n")
        f.write("    .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
        f.write(f"    .header.w = {w},\n")
        f.write(f"    .header.h = {h},\n")
        f.write(f"    .data_size = {len(data)},\n")
        f.write(f"    .data = {sym}_map,\n")
        f.write("};\n")
    print(f"{out}: {w}x{h} I8, {len(data)} 字节")
    return 0


if __name__ == "__main__":
    sys.exit(main())
