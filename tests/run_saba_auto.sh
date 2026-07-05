#!/bin/bash
# Orthox-64 Saba Auto Runner (Improved Stability)
OVMF=Web/wasabi/third_party/ovmf/RELEASEX64_OVMF.fd

# 1. Kill existing QEMU
pkill -f qemu-system-x86_64 || true
sleep 1

# 2. Start QEMU with nohup/setsid to prevent SIGHUP
echo "Starting QEMU in a detached session..."
setsid nohup qemu-system-x86_64 -machine q35 -cpu qemu64 -m 2G -bios "$OVMF" -cdrom orthos.iso -boot d \
    -vga std \
    -vnc 0.0.0.0:5 \
    -audio none \
    -netdev user,id=net0,hostfwd=tcp::8080-:8080,hostfwd=udp::12345-:12345,hostfwd=udp::12346-:12346 \
    -device virtio-net-pci,netdev=net0 \
    -serial file:serial.log \
    -monitor telnet:127.0.0.1:4444,server,nowait < /dev/null > qemu_saba.log 2>&1 &

# 3. Wait for OS to boot
echo "Waiting for Orthox-64 to boot (15 seconds)..."
sleep 15

# 4. Send command to start saba
echo "Sending /bin/saba command via monitor..."
python3 send_command.py "/bin/saba\n"

echo "Done. Please connect to 172.26.18.23:5905 via VNC."
# Check if it's still alive
ss -tlpn | grep 5905
