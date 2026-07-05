#!/bin/bash
set -euo pipefail

# ISO のパスを確認
ISO="out/orthox.iso"
if [ ! -f "$ISO" ]; then
    ISO="orthos.iso"
fi

exec qemu-system-x86_64 \
    -machine pc \
    -cpu max \
    -m 2G \
    -cdrom "$ISO" \
    -boot d \
    -display none \
    -serial stdio \
    "$@"
