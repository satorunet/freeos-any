# FreeOS Any: make でビルド (clang, lld-link, python3 が必要)
FONT ?= /usr/share/grub/unicode.pf2
CFLAGS = -Isrc/libc -Ithird_party/bearssl/inc -ffreestanding -fshort-wchar -mno-stack-arg-probe -fno-stack-protector \
         -fno-builtin -O2 -Wall -Wextra -Ibuild
BOOT = build/esp/EFI/BOOT
HDRS = src/os.h src/efi.h src/net.h build/font.h build/fontaa.h build/icons.h third_party/trust_anchors.h
PYTHON ?= python3
BIZUD = third_party/bizud

# Intel/AMD 用と ARM64 用を同じ EFI フォルダに作る。PC が自分に合う方を起動する
.DEFAULT_GOAL = all

build/font.h: tools/mkfont.py $(FONT)
	@mkdir -p build
	python3 tools/mkfont.py $(FONT) $@

# なめらかな文字 (BIZ UDゴシック)。Pillow と fontTools が必要
build/fontaa.h: tools/mkfontaa.py build/font.h $(BIZUD)/BIZUDGothic-Regular.ttf $(BIZUD)/BIZUDPGothic-Bold.ttf
	$(PYTHON) tools/mkfontaa.py build/font.h $(BIZUD)/BIZUDGothic-Regular.ttf $(BIZUD)/BIZUDPGothic-Bold.ttf $@

# アプリのアイコン (Pillow が必要)
build/icons.h: tools/mkicons.py $(BIZUD)/BIZUDPGothic-Bold.ttf
	@mkdir -p build
	$(PYTHON) tools/mkicons.py $(BIZUD)/BIZUDPGothic-Bold.ttf $@

APP = core apps net http browser transfer solitaire virtio sound hda boot calc
VER = 1.8
# BearSSL はOS標準の機能(乱数・時計・SIMD)に頼らない設定でビルドする
TLSFLAGS = -ffreestanding -mno-stack-arg-probe -fno-stack-protector -fno-builtin -O2 -w \
  -Isrc/libc -Ithird_party/bearssl/inc -Ithird_party/bearssl/src \
  -DBR_RDRAND=0 -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_POWER8=0 -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 \
  -DBR_USE_WIN32_RAND=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0 -DBR_INT128=0 -DBR_UMUL128=0
TLSSRC = $(shell find third_party/bearssl/src -name '*.c')

define ARCHRULES
build/$(1)/%.obj: src/%.c $$(HDRS)
	@mkdir -p $$(dir $$@)
	clang --target=$(2) $(3) $$(CFLAGS) -c $$< -o $$@
build/$(1)/tls/%.obj: third_party/bearssl/src/%.c
	@mkdir -p $$(dir $$@)
	@clang --target=$(2) $(3) $$(TLSFLAGS) -c $$< -o $$@
$(1)_OBJS = $$(APP:%=build/$(1)/%.obj) $$(TLSSRC:third_party/bearssl/src/%.c=build/$(1)/tls/%.obj)
endef
$(eval $(call ARCHRULES,x64,x86_64-unknown-windows-gnu,-mno-red-zone))
$(eval $(call ARCHRULES,arm64,aarch64-unknown-windows-gnu,))

$(BOOT)/BOOTX64.EFI: $(x64_OBJS)
	@mkdir -p $(dir $@)
	@lld-link /machine:x64 /subsystem:efi_application /entry:efi_main /nodefaultlib /out:$@ $^

$(BOOT)/BOOTAA64.EFI: $(arm64_OBJS)
	@mkdir -p $(dir $@)
	@lld-link /machine:arm64 /subsystem:efi_application /entry:efi_main /nodefaultlib /out:$@ $^

all: $(BOOT)/BOOTX64.EFI $(BOOT)/BOOTAA64.EFI

# USBメモリ/仮想マシン用のイメージ (mkfs.fat と mtools が必要)
image: all
	rm -f build/freeos-any.img
	truncate -s 64M build/freeos-any.img
	mkfs.fat -F 32 -n FREEOSANY build/freeos-any.img >/dev/null
	mcopy -s -i build/freeos-any.img build/esp/EFI ::/

# macOS で作る場合 (hdiutil と newfs_msdos を使う)
image-mac: all
	tools/mkimg-mac.sh

# このMacで起動して試す (QEMU、hvf)
run-mac: image-mac
	tools/run-mac.sh

# Windows の上で起動するための zip
vmzip: image
	python3 tools/mkvmzip.py build/freeos-any.img $(VER) windows

# macOS 用の .dmg (xorriso と python3-pillow が必要)
dmg: image
	python3 tools/mkdmg.py build/freeos-any.img build/esp $(VER)

# 概要とソースの参照ページ
srcdoc:
	python3 tools/mksrc.py $(VER)

clean:
	rm -rf build

.PHONY: all image image-mac run-mac vmzip dmg srcdoc clean
