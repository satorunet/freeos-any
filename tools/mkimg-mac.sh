#!/bin/bash
# macOS 用: build/esp/EFI を入れた 64MB の FAT32 イメージ build/freeos-any.img を作る (make image の代わり)
set -e
IMG=build/freeos-any.img
rm -f "$IMG"
MB=${IMG_MB:-64}
[ -n "$MODEL" ] && MB=${IMG_MB:-512}
mkfile -n ${MB}m "$IMG"
DEV=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$IMG" | awk 'NR==1{print $1}')
trap 'hdiutil detach "$DEV" >/dev/null 2>&1 || true' EXIT
newfs_msdos -F 32 -c 8 -v FREEOSANY "$DEV" >/dev/null
MNT=$(mktemp -d)
mount -t msdos "$DEV" "$MNT"
cp -R build/esp/EFI "$MNT/"
# 見本の HTML (ファイルアプリからブラウザで開ける)
if [ -d samples ]; then mkdir -p "$MNT/HOME"; cp samples/* "$MNT/HOME/"; fi
# AI チャットのモデル (MODEL=…fqw を指定したとき)
if [ -n "$MODEL" ]; then mkdir -p "$MNT/HOME/AI"; cp "$MODEL" "$MNT/HOME/AI/qwen3-0.6b.fqw"; fi
dot_clean -m "$MNT" 2>/dev/null || true
find "$MNT" -name '._*' -delete
umount "$MNT"; rmdir "$MNT"
echo "$IMG"
