#!/bin/bash
# Orthox-64 Saba Browser Development Runner (VNC Enabled, Audio Disabled)
trap '' HUP
OVMF=Web/wasabi/third_party/ovmf/RELEASEX64_OVMF.fd
qemu-system-x86_64 -machine q35 -cpu qemu64 -m 2G -bios "$OVMF" -cdrom orthos.iso -boot d \
    -vga std \
    -vnc 0.0.0.0:5 \
    -audio none \
    -netdev user,id=net0,hostfwd=tcp::8080-:8080,hostfwd=udp::12345-:12345,hostfwd=udp::12346-:12346 \
    -device virtio-net-pci,netdev=net0 \
    -serial file:serial.log \
    -monitor telnet:127.0.0.1:4444,server,nowait
