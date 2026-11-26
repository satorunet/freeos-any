#!/usr/bin/env python3
"""FreeOS Any のロゴ: 夜空色の角丸に虹色の ∞ と「Any」。
draw_logo(size) が RGBA 画像を返す (Pillow)。単体で実行すると logo.png を書き出す。
形と色は src/boot.c の起動アニメーションと同じ。"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
BOLD = os.path.join(HERE, '..', 'third_party', 'bizud', 'BIZUDPGothic-Bold.ttf')

# 設計図は 512 x 512
BG_TOP, BG_BOTTOM = (30, 28, 52), (16, 15, 28)
RAINBOW = [(150, 130, 255), (109, 93, 252), (236, 120, 220), (255, 190, 90), (150, 130, 255)]
CX, CY, A, WIDTH = 256, 214, 168, 44  # ∞ の中心・大きさ・線の太さ


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def rainbow(t):
    n = len(RAINBOW) - 1
    i = min(int(t * n), n - 1)
    return lerp(RAINBOW[i], RAINBOW[i + 1], t * n - i)


def lemniscate(t, k):
    s, c = math.sin(t), math.cos(t)
    d = 1 + s * s
    return (CX + A * c / d) * k, (CY + A * s * c / d) * k


def draw_logo(size, text=True):
    K = max(1, 2048 // 512)
    S = 512 * K
    grad = Image.new('RGBA', (S, S))
    g = ImageDraw.Draw(grad)
    for y in range(S):
        g.line([(0, y), (S, y)], fill=lerp(BG_TOP, BG_BOTTOM, y / (S - 1)) + (255,))
    mask = Image.new('L', (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((24 * K, 24 * K, S - 24 * K - 1, S - 24 * K - 1), 110 * K, fill=255)
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)

    def stroke(t0, t1, n=1600):
        d = ImageDraw.Draw(img)
        r = WIDTH * K / 2
        for i in range(n + 1):
            t = t0 + (t1 - t0) * i / n
            x, y = lemniscate(t, K)
            d.ellipse((x - r, y - r, x + r, y + r), fill=rainbow((t % (2 * math.pi)) / (2 * math.pi)) + (255,))

    # 右端 (t=0) から一筆で描く。交差点 (t=3π/2) の手前で影を落としてから、その先を描く
    stroke(0, 1.5 * math.pi - 0.5)
    shadow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    for i in range(200):
        t = 1.5 * math.pi - 0.25 + 0.5 * i / 200
        x, y = lemniscate(t, K)
        r = (WIDTH / 2 + 6) * K
        sd.ellipse((x - r, y - r, x + r, y + r), fill=(0, 0, 0, 120))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(5 * K)))
    stroke(1.5 * math.pi - 0.5, 2 * math.pi, 500)
    if text:
        ImageDraw.Draw(img).text((S / 2, 392 * K), 'Any', font=ImageFont.truetype(BOLD, 96 * K),
                                 fill=(255, 255, 255, 255), anchor='mm')
    return img.resize((size, size), Image.LANCZOS)


if __name__ == '__main__':
    draw_logo(int(sys.argv[2]) if len(sys.argv) > 2 else 512).save(sys.argv[1] if len(sys.argv) > 1 else 'logo.png')
