#!/usr/bin/env python3
"""Mac アプリ版の Info.plist とアイコンを作る。使い方: mkmacapp.py 'FreeOS Any.app' 版"""
import io, os, plistlib, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from logo import draw_logo
from PIL import Image
app, ver = sys.argv[1], sys.argv[2]
with open(os.path.join(app, 'Contents/Info.plist'), 'wb') as f:
    plistlib.dump({'CFBundleName': 'FreeOS Any', 'CFBundleDisplayName': 'FreeOS Any', 'CFBundleIdentifier': 'net.satoru.freeosany.native',
                   'CFBundleExecutable': 'FreeOSAny', 'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': ver,
                   'CFBundleVersion': ver, 'CFBundleIconFile': 'icon', 'LSMinimumSystemVersion': '11.0',
                   'NSHighResolutionCapable': True, 'NSPrincipalClass': 'NSApplication'}, f)
base = draw_logo(512)
chunks = b''
for kind, size in ((b'ic07', 128), (b'ic08', 256), (b'ic09', 512)):
    buf = io.BytesIO(); base.resize((size, size), Image.LANCZOS).save(buf, 'PNG')
    chunks += kind + struct.pack('>I', len(buf.getvalue()) + 8) + buf.getvalue()
open(os.path.join(app, 'Contents/Resources/icon.icns'), 'wb').write(b'icns' + struct.pack('>I', len(chunks) + 8) + chunks)
