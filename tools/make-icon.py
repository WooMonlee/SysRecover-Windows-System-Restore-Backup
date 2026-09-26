#!/usr/bin/env python3
"""make-icon.py — 生成 resources/SysRecover.ico（应用/窗口图标）。

造型：品牌蓝圆角方块（#2b5ce0，见 AGENTS §15 配色 token）+ 白色**环形刷新箭头**
（"还原/回滚"意象；小尺寸也看得清）。改配色/造型改本文件重跑即可。
用法：python tools/make-icon.py
"""
import math
import os

from PIL import Image, ImageDraw

BLUE = (43, 92, 224, 255)   # #2b5ce0
WHITE = (255, 255, 255, 255)
S = 1024                     # 先大尺寸画再缩小（自带抗锯齿）
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "resources", "SysRecover.ico")

img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

# 1) 圆角方块（圆角直径 ≈ 22% 边长）
d.rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * 0.22), fill=BLUE)

# 2) 白色环形箭头：圆心居中，环半径 R、线宽 w；缺口在右上，箭头指向顺时针
cx = cy = S / 2.0
R = S * 0.28
w = int(S * 0.10)
a1, a2 = -40.0, 250.0        # 画 290°（留 70° 缺口）
d.arc([cx - R, cy - R, cx + R, cy + R], start=a1, end=a2, width=w, fill=WHITE)

# 3) 箭头（在弧的终点 a2 处，沿切线方向）
ar = w * 1.7
tang = math.radians(a2 + 90.0)          # 切线方向
ax = cx + R * math.cos(math.radians(a2))
ay = cy + R * math.sin(math.radians(a2))
perp = tang + math.pi / 2.0
tip = (ax + ar * 0.75 * math.cos(tang), ay + ar * 0.75 * math.sin(tang))
b1 = (ax - ar * 0.35 * math.cos(tang) + ar * 0.85 * math.cos(perp),
      ay - ar * 0.35 * math.sin(tang) + ar * 0.85 * math.sin(perp))
b2 = (ax - ar * 0.35 * math.cos(tang) - ar * 0.85 * math.cos(perp),
      ay - ar * 0.35 * math.sin(tang) - ar * 0.85 * math.sin(perp))
d.polygon([tip, b1, b2], fill=WHITE)

img.save(OUT, sizes=[(256, 256), (128, 128), (64, 64), (48, 48),
                     (32, 32), (16, 16)])
print("wrote %s (%d bytes)" % (os.path.normpath(OUT), os.path.getsize(OUT)))
