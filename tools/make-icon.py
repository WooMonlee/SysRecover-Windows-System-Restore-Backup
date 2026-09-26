#!/usr/bin/env python3
"""make-icon.py — 生成 resources/SysRecover.ico（应用/窗口图标，多尺寸）。

**有源图时**（推荐）：`resources/icon-source.ico`（或 `.png`，≥256×256）——由产品维护者提供的
美术图，本脚本只负责**高质量降采样成标准多尺寸**（256/128/64/48/32/24/16）。
Windows 任务栏/资源管理器/Alt-Tab 会各取所需尺寸；单尺寸 ICO 会被 GDI 粗暴缩放，小尺寸发糊。
**无源图时**：按下面的内置造型生成（品牌蓝圆角方块 + 白色环形刷新箭头，配色见 AGENTS §15）。
用法：python tools/make-icon.py
"""
import math
import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "..", "resources")
OUT = os.path.join(RES, "SysRecover.ico")
SRC_CANDIDATES = [os.path.join(RES, "icon-source.ico"),
                  os.path.join(RES, "icon-source.png")]

# Windows 常用尺寸；256 由 PIL 以 PNG 压缩存入 ICO（体积小、保真）
SIZES = [(256, 256), (128, 128), (64, 64), (48, 48),
         (32, 32), (24, 24), (16, 16)]

BLUE = (43, 92, 224, 255)   # #2b5ce0
WHITE = (255, 255, 255, 255)
S = 1024                    # 内置造型先大尺寸画再缩小（自带抗锯齿）


def from_source(path):
    """把美术源图降采样成多尺寸 ICO。"""
    im = Image.open(path)
    if getattr(im, "n_frames", 1) > 1:      # 多尺寸 ICO：取最大的一帧
        im.size = max(im.info.get("sizes", [(im.width, im.height)]))
    im = im.convert("RGBA")
    if im.width < 256 or im.height < 256:
        raise SystemExit("源图太小（%dx%d），至少 256x256" % (im.size))
    if im.width != im.height:
        raise SystemExit("源图不是正方形（%dx%d）" % (im.size))
    im.save(OUT, sizes=SIZES)
    return im.width


def from_builtin():
    """内置造型：品牌蓝圆角方块 + 白色环形刷新箭头。"""
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * 0.22), fill=BLUE)

    cx = cy = S / 2.0
    R = S * 0.28
    w = int(S * 0.10)
    a1, a2 = -40.0, 250.0        # 画 290°（留 70° 缺口）
    d.arc([cx - R, cy - R, cx + R, cy + R], start=a1, end=a2, width=w, fill=WHITE)

    ar = w * 1.7
    tang = math.radians(a2 + 90.0)
    ax = cx + R * math.cos(math.radians(a2))
    ay = cy + R * math.sin(math.radians(a2))
    perp = tang + math.pi / 2.0
    tip = (ax + ar * 0.75 * math.cos(tang), ay + ar * 0.75 * math.sin(tang))
    b1 = (ax - ar * 0.35 * math.cos(tang) + ar * 0.85 * math.cos(perp),
          ay - ar * 0.35 * math.sin(tang) + ar * 0.85 * math.sin(perp))
    b2 = (ax - ar * 0.35 * math.cos(tang) - ar * 0.85 * math.cos(perp),
          ay - ar * 0.35 * math.sin(tang) - ar * 0.85 * math.sin(perp))
    d.polygon([tip, b1, b2], fill=WHITE)

    img.save(OUT, sizes=SIZES)
    return S


def main():
    src = next((p for p in SRC_CANDIDATES if os.path.exists(p)), None)
    if src:
        side = from_source(src)
        print("source: %s (%dx%d) -> multi-size ICO" % (os.path.basename(src), side, side))
    else:
        from_builtin()
        print("source: built-in design (no resources/icon-source.*)")
    print("wrote %s (%d bytes)" % (os.path.normpath(OUT), os.path.getsize(OUT)))


if __name__ == "__main__":
    main()
