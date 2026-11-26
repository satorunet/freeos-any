#!/usr/bin/env python3
"""BIZ UDゴシックを縦16ドットの濃淡付き画像 (1点4ビット) にして C ヘッダにする。
文字の幅は Unifont と同じ (半角8 / 全角16) にそろえる。BIZ UDゴシックにない文字は Unifont で描く。
起動画面の名前用に、BIZ UDPゴシック Bold の大きな文字も作る。
使い方: mkfontaa.py font.h(Unifont) BIZUDGothic-Regular.ttf BIZUDPGothic-Bold.ttf 出力.h  (Pillow が必要)"""
import re
import sys

from PIL import Image, ImageDraw, ImageFont

unifont_h, mono_ttf, title_ttf, out = sys.argv[1:5]

# Unifont 側の文字と幅 (この並びに合わせる)
src = open(unifont_h).read()
codes = list(map(int, re.search(r'font_code\[\] = \{([^}]*)\}', src).group(1).split(',')))
widths = list(map(int, re.search(r'font_width\[\] = \{([^}]*)\}', src).group(1).split(',')))

SIZE, BASE = 16, 14  # 16px で描き、上から14ドット目を文字の基準線にする
mono = ImageFont.truetype(mono_ttf, SIZE)
try:
    from fontTools.ttLib import TTFont
    cmap = TTFont(mono_ttf).getBestCmap()
except ImportError:
    cmap = None


def has(ch):
    return ord(ch) in cmap if cmap is not None else mono.getmask(ch).getbbox() is not None


def render(ch, cell):
    adv = round(mono.getlength(ch))
    img = Image.new('L', (cell, 16), 0)
    d = ImageDraw.Draw(img)
    d.text(((cell - adv) // 2, BASE), ch, font=mono, fill=255, anchor='ls')
    px = img.load()
    nib = []
    for y in range(16):
        for x in range(cell):
            nib.append((px[x, y] * 15 + 127) // 255)
    return nib


offsets, data = [], bytearray()
aa = 0
for code, w in zip(codes, widths):
    ch = chr(code)
    if code == 0x20 or not has(ch) or 0x2500 <= code <= 0x257F:  # 罫線は Unifont の方がつながる
        offsets.append(0xFFFFFFFF)
        continue
    if round(mono.getlength(ch)) > w:  # 枠より広い (Unifont では半角、BIZ では全角の記号など)
        offsets.append(0xFFFFFFFF)
        continue
    nib = render(ch, w)
    if not any(nib):
        offsets.append(0xFFFFFFFF)
        continue
    offsets.append(len(data))
    for i in range(0, len(nib), 2):
        data.append(nib[i] << 4 | (nib[i + 1] if i + 1 < len(nib) else 0))
    aa += 1

# 起動画面の名前用 (8ビットの濃淡)
TITLE = 'FreeOS Any'
big = ImageFont.truetype(title_ttf, 34)
asc, desc = big.getmetrics()
th = asc + desc
tchars = sorted(set(TITLE))
toff, tdata, tinfo = [], bytearray(), []
for ch in tchars:
    w = max(1, round(big.getlength(ch)))
    img = Image.new('L', (w + 2, th), 0)
    ImageDraw.Draw(img).text((1, asc), ch, font=big, fill=255, anchor='ls')
    tinfo.append((ord(ch), w + 2, round(big.getlength(ch)), len(tdata)))
    tdata += img.tobytes()

with open(out, 'w') as f:
    f.write('/* 自動生成: BIZ UDゴシック / BIZ UDPゴシック (SIL Open Font License 1.1) より */\n')
    f.write('static const u32 fontaa_off[] = {%s};\n' % ','.join(map(str, offsets)))
    f.write('static const u8 fontaa_data[] = {%s};\n' % ','.join(map(str, data)))
    f.write(f'#define TITLE_H {th}\n#define TITLE_N {len(tinfo)}\n')
    f.write('static const struct { u16 code, w, adv; u32 off; } title_glyph[] = {%s};\n' %
            ','.join('{%d,%d,%d,%d}' % t for t in tinfo))
    f.write('static const u8 title_data[] = {%s};\n' % ','.join(map(str, tdata)))
print(f'{aa} / {len(codes)} glyphs from BIZ UDGothic, {len(data)} bytes; title {th}px')
