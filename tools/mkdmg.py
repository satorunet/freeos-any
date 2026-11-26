#!/usr/bin/env python3
"""macOS 用のディスクイメージ(.dmg)を作る。使い方: mkdmg.py disk.img espフォルダ 版番号

中身は ISO 9660 (Rock Ridge) 形式。実行権限とリンクを保てて、xorriso だけで作れる。"""
import gzip
import io
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

img, esp, ver = sys.argv[1:4]
xorriso = os.environ.get('XORRISO', 'xorriso')


def icon():
    base = Image.new('RGBA', (512, 512))
    d = ImageDraw.Draw(base)
    d.rounded_rectangle((36, 36, 476, 476), 100, fill=(109, 93, 252, 255))
    d.rounded_rectangle((100, 130, 412, 382), 24, fill=(244, 244, 250, 255))
    d.rounded_rectangle((100, 130, 412, 186), 24, fill=(36, 36, 64, 255))
    d.rectangle((100, 162, 412, 186), fill=(36, 36, 64, 255))
    # 文字は図形で描く (フォントに依存しない): F と A
    ink = (109, 93, 252, 255)
    d.line([(160, 350), (160, 222), (232, 222)], fill=ink, width=28, joint='curve')
    d.line([(160, 280), (220, 280)], fill=ink, width=26)
    d.line([(270, 356), (316, 218), (362, 356)], fill=ink, width=28, joint='curve')
    d.line([(290, 304), (342, 304)], fill=ink, width=24)
    chunks = b''
    for kind, size in ((b'ic07', 128), (b'ic08', 256), (b'ic09', 512)):
        buf = io.BytesIO()
        base.resize((size, size), Image.LANCZOS).save(buf, 'PNG')
        chunks += kind + struct.pack('>I', len(buf.getvalue()) + 8) + buf.getvalue()
    return b'icns' + struct.pack('>I', len(chunks) + 8) + chunks


with tempfile.TemporaryDirectory() as tmp:
    os.chmod(tmp, 0o755)
    app = f'{tmp}/FreeOS Any.app/Contents'
    res = f'{app}/Resources'
    os.makedirs(f'{app}/MacOS')
    os.makedirs(f'{res}/EFI/BOOT')
    with open(f'{app}/Info.plist', 'wb') as f:
        plistlib.dump({
            'CFBundleName': 'FreeOS Any', 'CFBundleIdentifier': 'net.satoru.freeosany',
            'CFBundleExecutable': 'FreeOSAny', 'CFBundlePackageType': 'APPL',
            'CFBundleShortVersionString': ver, 'CFBundleVersion': ver, 'CFBundleIconFile': 'icon',
            'LSMinimumSystemVersion': '11.0', 'LSUIElement': True,
            'LSArchitecturePriority': ['arm64', 'x86_64'],  # Rosetta を要求させない
        }, f)
    shutil.copyfile('vm/macapp/FreeOSAny', f'{app}/MacOS/FreeOSAny')
    os.chmod(f'{app}/MacOS/FreeOSAny', 0o755)
    with open(img, 'rb') as src, open(f'{res}/freeos-any.img.gz', 'wb') as dst:
        with gzip.GzipFile(fileobj=dst, mode='wb', mtime=0) as z:
            shutil.copyfileobj(src, z)
    for name in ('BOOTX64.EFI', 'BOOTAA64.EFI'):
        shutil.copyfile(f'{esp}/EFI/BOOT/{name}', f'{res}/EFI/BOOT/{name}')
    with open(f'{res}/VERSION', 'w') as f:
        f.write(ver + '\n')
    with open(f'{res}/icon.icns', 'wb') as f:
        f.write(icon())
    shutil.copyfile('third_party/bearssl/LICENSE.txt', f'{res}/LICENSE-BearSSL.txt')
    shutil.copyfile('vm/macapp/README.txt', f'{tmp}/README.txt')
    os.symlink('/Applications', f'{tmp}/Applications')
    out = f'dist/FreeOSAny-{ver}.dmg'
    if os.path.exists(out):
        os.unlink(out)
    subprocess.run([xorriso, '-as', 'mkisofs', '-quiet', '-V', 'FreeOS Any', '-R', '-uid', '0', '-gid', '0',
                    '-input-charset', 'utf-8', '-o', out, tmp], check=True)
    os.chmod(out, 0o644)
    print(out, os.path.getsize(out))
