#!/usr/bin/env python3
"""概要とソースをまとめた参照用ページと zip を作る。使い方: mksrc.py 版番号"""
import glob
import html
import os
import sys
import zipfile

ver = sys.argv[1]
PATTERNS = ['OVERVIEW.md', 'README.md', 'Makefile', 'index.html', 'src/**/*', 'tools/*.py', 'vm/**/*', 'tests/**/*']
files = []
for p in PATTERNS:
    files += sorted(f for f in glob.glob(p, recursive=True) if os.path.isfile(f) and '__pycache__' not in f)
third = sorted(f for f in glob.glob('third_party/**/*', recursive=True) if os.path.isfile(f))

os.makedirs('dist', exist_ok=True)
archive = f'dist/freeos-any-{ver}-src.zip'
with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
    for f in files + third:
        z.write(f, f'freeos-any-{ver}/{f}')


def read(f):
    with open(f, encoding='utf-8-sig') as h:
        return h.read()


out = [f'''<!doctype html><html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><meta name="robots" content="noindex">
<title>FreeOS Any {ver} 概要とソース</title><style>
body{{margin:0 auto;max-width:1000px;padding:20px;background:#181a35;color:#eee;font:15px/1.6 system-ui,sans-serif}}
a{{color:#b9b0ff}}pre{{background:#0d0e1c;padding:12px;border-radius:8px;overflow:auto;font:13px/1.45 ui-monospace,monospace;tab-size:4}}
.doc{{white-space:pre-wrap}}summary{{cursor:pointer;padding:4px 0;font-family:ui-monospace,monospace}}</style></head><body>
<h1>FreeOS Any {ver} 概要とソース</h1>
<p><a href="{archive}" download>ソース一式 (zip)</a> ／ <a href="./">紹介ページ</a></p>
<pre class="doc">{html.escape(read("OVERVIEW.md"))}</pre>
<h2>ソース</h2><p>ファイル名を押すと中身が開きます。BearSSL と ルート証明書 (third_party/) は zip に入っています。</p>''']
for f in files:
    if f == 'OVERVIEW.md':
        continue
    text = read(f)
    out.append(f'<details id="{html.escape(f)}"><summary>{html.escape(f)} ({len(text.splitlines())} 行)</summary>'
               f'<pre>{html.escape(text)}</pre></details>')
out.append('</body></html>\n')
page = f'source-{ver}.html'
with open(page, 'w', encoding='utf-8') as h:
    h.write('\n'.join(out))
print(page, len(files), 'files;', archive, len(files) + len(third), 'files')
