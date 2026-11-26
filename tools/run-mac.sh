#!/bin/bash
# 開発用: build/freeos-any.img を QEMU (hvf) で起動する。QEMU は $QEMU か PATH から
QEMU=${QEMU:-$(command -v qemu-system-x86_64)}
[ -x "$QEMU" ] || { echo "QEMU がありません (QEMU=... で指定)"; exit 1; }
SHARE="$(dirname "$QEMU")/../share/qemu"
mkdir -p build/vm
[ -f build/vm/vars.fd ] || cp "$SHARE/edk2-i386-vars.fd" build/vm/vars.fd
chmod u+w build/vm/vars.fd
exec "$QEMU" -name "FreeOS Any (開発)" -m 512 -machine q35 -accel hvf -cpu host,-pdpe1gb \
  -drive "if=pflash,format=raw,readonly=on,file=$SHARE/edk2-x86_64-code.fd" \
  -drive if=pflash,format=raw,file=build/vm/vars.fd \
  -drive if=none,id=disk,format=raw,file=build/freeos-any.img -device virtio-blk-pci,drive=disk \
  -nic user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:8081-:80 -device virtio-rng-pci -device virtio-tablet-pci "$@"
