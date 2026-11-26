#!/usr/bin/env python3
"""GRUB の unicode.pf2 (GNU Unifont) から必要な範囲を C ヘッダにする。
使い方: mkfont.py unicode.pf2 font.h"""
import struct
import sys

RANGES = [(0x20, 0x7E), (0xA0, 0x17F), (0x370, 0x3FF), (0x400, 0x45F), (0x2000, 0x206F),
          (0x2070, 0x20CF), (0x2100, 0x214F), (0x2190, 0x21FF), (0x2200, 0x22FF), (0x2300, 0x23FF),
          (0x2460, 0x24FF), (0x2500, 0x257F), (0x25A0, 0x25FF), (0x2600, 0x26FF), (0x2700, 0x27BF),
          (0x3000, 0x30FF), (0x3200, 0x33FF), (0x4E00, 0x9FFF), (0xFF00, 0xFFEF)]

data = open(sys.argv[1], 'rb').read()
sections, pos = {}, 0
while pos + 8 <= len(data):
    name, length = data[pos:pos + 4], struct.unpack_from('>I', data, pos + 4)[0]
    if name == b'DATA':
        break
    sections[name] = data[pos + 8:pos + 8 + length]
    pos += 8 + length
ascent = struct.unpack('>H', sections[b'ASCE'])[0]

glyphs = []
index = sections[b'CHIX']
for i in range(0, len(index), 9):
    code, _, off = struct.unpack_from('>IBI', index, i)
    if not any(a <= code <= b for a, b in RANGES):
        continue
    w, h, xo, yo, advance = struct.unpack_from('>HHhhh', data, off)
    bits = int.from_bytes(data[off + 10:off + 10 + (w * h + 7) // 8], 'big')
    total = ((w * h + 7) // 8) * 8
    rows = [0] * 16
    for y in range(h):
        for x in range(w):
            if bits >> (total - 1 - (y * w + x)) & 1:
                gx, gy = x + xo, ascent - yo - h + y
                if 0 <= gx < 16 and 0 <= gy < 16:
                    rows[gy] |= 0x8000 >> gx
    glyphs.append((code, min(advance, 16), rows))
glyphs.sort()

with open(sys.argv[2], 'w') as f:
    f.write('/* 自動生成: GNU Unifont (GRUB unicode.pf2) より */\n')
    f.write(f'#define FONT_COUNT {len(glyphs)}\n')
    f.write('static const u16 font_code[] = {%s};\n' % ','.join(str(g[0]) for g in glyphs))
    f.write('static const u8 font_width[] = {%s};\n' % ','.join(str(g[1]) for g in glyphs))
    f.write('static const u16 font_bits[][16] = {\n')
    for g in glyphs:
        f.write('{%s},\n' % ','.join(map(str, g[2])))
    f.write('};\n')
print(f'{len(glyphs)} glyphs, ascent={ascent}')
