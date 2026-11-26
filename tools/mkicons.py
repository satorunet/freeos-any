#!/usr/bin/env python3
"""アプリのアイコンを描いて C ヘッダにする (Pillow が必要)。
大きく (256px) 描いてから縮めるので、縁がなめらかになる。
使い方: mkicons.py 太字フォント.ttf 出力.h
並び順は os.h の APP_* と同じ。最後はスタートボタン用のロゴ。"""
import sys

from PIL import Image, ImageDraw, ImageFont

bold_ttf, out = sys.argv[1:3]
S = 256
SIZES = (32, 20, 16)  # タスクバー / スタートメニュー / タイトルバー


def base(top, bottom):
    """縦にグラデーションのかかった角丸の台"""
    grad = Image.new('RGBA', (S, S))
    g = ImageDraw.Draw(grad)
    for y in range(S):
        t = y / (S - 1)
        g.line([(0, y), (S, y)], fill=tuple(int(top[i] * (1 - t) + bottom[i] * t) for i in range(3)) + (255,))
    mask = Image.new('L', (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((8, 8, S - 9, S - 9), 58, fill=255)
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)
    # 上半分にうすい光
    shine = Image.new('RGBA', (S, S), (255, 255, 255, 0))
    sm = Image.new('L', (S, S), 0)
    ImageDraw.Draw(sm).rounded_rectangle((8, 8, S - 9, S // 2), 58, fill=34)
    shine.putalpha(sm)
    img.alpha_composite(shine)
    return img, ImageDraw.Draw(img)


def hexc(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


WHITE = (255, 255, 255, 255)
font = lambda size: ImageFont.truetype(bold_ttf, size)


def terminal():
    img, d = base(hexc(0x3A3A50), hexc(0x14141C))
    d.rounded_rectangle((40, 60, 216, 196), 16, outline=(110, 110, 140, 255), width=6)
    d.line([(70, 100), (110, 128), (70, 156)], fill=(74, 222, 128, 255), width=18, joint='curve')
    d.line([(124, 160), (180, 160)], fill=(74, 222, 128, 255), width=16)
    return img


def files():
    img, d = base(hexc(0xFFC857), hexc(0xF59E0B))
    d.rounded_rectangle((44, 70, 128, 110), 14, fill=(255, 240, 200, 255))
    d.rounded_rectangle((44, 92, 212, 196), 18, fill=WHITE)
    d.rounded_rectangle((44, 112, 212, 196), 18, fill=(255, 250, 235, 255))
    return img


def editor():
    img, d = base(hexc(0x60A5FA), hexc(0x2563EB))
    d.rounded_rectangle((62, 40, 182, 216), 14, fill=WHITE)
    for i, w in enumerate((88, 72, 88, 56)):
        d.line([(82, 80 + i * 30), (82 + w, 80 + i * 30)], fill=(147, 180, 240, 255), width=10)
    # 鉛筆
    d.polygon([(214, 118), (230, 134), (156, 208), (132, 216), (140, 192)], fill=(255, 200, 87, 255))
    d.polygon([(132, 216), (140, 192), (156, 208)], fill=(60, 60, 80, 255))
    return img


def browser():
    img, d = base(hexc(0x2DD4BF), hexc(0x0D9488))
    c, r = 128, 78
    d.ellipse((c - r, c - r, c + r, c + r), outline=WHITE, width=12)
    d.ellipse((c - 34, c - r, c + 34, c + r), outline=WHITE, width=10)
    d.line([(c - r, c), (c + r, c)], fill=WHITE, width=10)
    d.line([(c - 68, c - 38), (c + 68, c - 38)], fill=WHITE, width=8)
    d.line([(c - 68, c + 38), (c + 68, c + 38)], fill=WHITE, width=8)
    return img


def transfer():
    img, d = base(hexc(0xA78BFA), hexc(0x7C3AED))
    d.line([(96, 196), (96, 72)], fill=WHITE, width=20)
    d.polygon([(56, 104), (96, 56), (136, 104)], fill=WHITE)
    d.line([(160, 60), (160, 184)], fill=WHITE, width=20)
    d.polygon([(120, 152), (160, 200), (200, 152)], fill=WHITE)
    return img


def solitaire():
    img, d = base(hexc(0x34D399), hexc(0x059669))
    d.rounded_rectangle((62, 58, 158, 190), 12, fill=(230, 240, 235, 255), outline=(20, 90, 60, 255), width=4)
    d.rounded_rectangle((100, 70, 196, 202), 12, fill=WHITE, outline=(20, 90, 60, 255), width=4)
    # ハート
    hx, hy = 148, 136
    d.ellipse((hx - 30, hy - 26, hx + 2, hy + 6), fill=(239, 68, 68, 255))
    d.ellipse((hx - 2, hy - 26, hx + 30, hy + 6), fill=(239, 68, 68, 255))
    d.polygon([(hx - 29, hy - 4), (hx + 29, hy - 4), (hx, hy + 32)], fill=(239, 68, 68, 255))
    return img


def calc():
    img, d = base(hexc(0x4B4B66), hexc(0x24243A))
    d.rounded_rectangle((56, 44, 200, 92), 10, fill=(190, 240, 200, 255))
    for r in range(3):
        for c in range(3):
            x, y = 56 + c * 50, 106 + r * 38
            col = (255, 160, 70, 255) if c == 2 else (220, 220, 235, 255)
            d.rounded_rectangle((x, y, x + 42, y + 30), 8, fill=col)
    return img


def ai():
    """AI チャット: 夜空色に吹き出しと虹色の ∞"""
    import math
    img, d = base((44, 40, 74), (16, 15, 28))
    d.rounded_rectangle((40, 56, 216, 176), 40, fill=(255, 255, 255, 255))
    d.polygon([(70, 168), (60, 214), (112, 172)], fill=(255, 255, 255, 255))
    cols = [(150, 130, 255), (109, 93, 252), (236, 120, 220), (255, 190, 90), (150, 130, 255)]
    for i in range(900):
        t = i / 900 * 2 * math.pi
        s_, c_ = math.sin(t), math.cos(t); dd = 1 + s_ * s_
        x, y = 128 + 64 * c_ / dd, 116 + 64 * s_ * c_ / dd
        u = t / (2 * math.pi) * 4; k = min(int(u), 3); f = u - k
        col = tuple(int(cols[k][j] * (1 - f) + cols[k + 1][j] * f) for j in range(3))
        d.ellipse((x - 11, y - 11, x + 11, y + 11), fill=col + (255,))
    return img


def about():
    img, d = base(hexc(0x38BDF8), hexc(0x0284C7))
    d.ellipse((50, 50, 206, 206), outline=WHITE, width=14)
    d.ellipse((116, 76, 140, 100), fill=WHITE)
    d.rounded_rectangle((114, 114, 142, 182), 10, fill=WHITE)
    return img


def logo():
    """FreeOS Any のロゴ (tools/logo.py)。小さいので Any の文字は入れない"""
    sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
    from logo import draw_logo
    return draw_logo(S, text=False)


ICONS = [terminal, files, editor, browser, transfer, solitaire, calc, ai, about, logo]

with open(out, 'w') as f:
    f.write('/* 自動生成: tools/mkicons.py (アプリのアイコン、1点 ARGB) */\n')
    f.write(f'#define ICON_N {len(ICONS)}\n')
    for size in SIZES:
        f.write(f'static const u32 icon{size}[ICON_N][{size * size}] = {{\n')
        for fn in ICONS:
            im = fn().resize((size, size), Image.LANCZOS)
            px = list(im.getdata())
            f.write('{%s},\n' % ','.join(str(a << 24 | r << 16 | g << 8 | b) for r, g, b, a in px))
        f.write('};\n')
print(f'{len(ICONS)} icons')
