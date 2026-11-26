#!/usr/bin/env python3
"""Windows の上で起動するための zip を作る。使い方: mkvmzip.py disk.img 版番号"""
import sys
import zipfile

img, ver = sys.argv[1], sys.argv[2]
FILES = {'windows': ['FreeOSAny.bat', 'launch.ps1', 'はじめに.txt']}
for name in sys.argv[3:] or ['windows']:
    files = FILES[name]
    top = f'FreeOSAny-{ver}-{name}/'
    entries = [(f'vm/{name}/{f}', f) for f in files]
    entries += [(img, 'freeos-any.img'), ('third_party/bearssl/LICENSE.txt', 'LICENSE-BearSSL.txt')]
    with zipfile.ZipFile(f'dist/freeos-any-{ver}-{name}.zip', 'w', zipfile.ZIP_DEFLATED) as z:
        for src, dst in entries:
            info = zipfile.ZipInfo.from_file(src, top + dst)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3  # Unix の実行権限を保つ
            mode = 0o755 if dst.endswith('.command') else 0o644
            info.external_attr = (0o100000 | mode) << 16
            with open(src, 'rb') as f:
                z.writestr(info, f.read())
