#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
OVMF="${OVMF:-Web/wasabi/third_party/ovmf/RELEASEX64_OVMF.fd}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/virtio-q35-msix-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/virtio-q35-msix-qemu.out}"

if [ ! -f "${OVMF}" ]; then
    echo "OVMF firmware not found: ${OVMF}" >&2
    exit 1
fi

make orthos.iso >/tmp/virtio-q35-msix-build.out

rm -f "${SERIAL_LOG}" "${QEMU_OUT}"

qemu-system-x86_64 \
    -machine q35 \
    -cpu qemu64 \
    -m 2G \
    -bios "${OVMF}" \
    -cdrom "${ISO}" \
    -boot d \
    -display none \
    -audio none \
    -drive if=none,id=rootfs,file=rootfs.img,format=raw \
    -device virtio-blk-pci,drive=rootfs \
    -netdev user,id=net0,hostfwd=udp::12345-:12345 \
    -device virtio-net-pci,netdev=net0 \
    -serial "file:${SERIAL_LOG}" \
    -k en-us >"${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

cleanup() {
    if kill -0 "${QEMU_PID}" 2>/dev/null; then
        kill "${QEMU_PID}" 2>/dev/null || true
        wait "${QEMU_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "Waiting for q35 VirtIO MSI-X boot..."
for _ in $(seq 1 90); do
    if grep -q '\[net\] virtio-net ready.*msix=1' "${SERIAL_LOG}" 2>/dev/null &&
       grep -q '\[vblk\] virtio-blk ready.*msix=1' "${SERIAL_LOG}" 2>/dev/null &&
       grep -q '\[lwip\] dhcp bound ip=' "${SERIAL_LOG}" 2>/dev/null &&
       grep -q '\[boot\] mounted xv6fs root image on vblk0' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

python3 - <<'PY'
import socket
import sys
import time

payload = b"orthox-q35-msix"
deadline = time.time() + 20
last_error = None

while time.time() < deadline:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    try:
        sock.sendto(payload, ("127.0.0.1", 12345))
        data, _ = sock.recvfrom(256)
        if data == payload:
            sys.exit(0)
        last_error = RuntimeError(f"unexpected echo payload: {data!r}")
    except Exception as exc:
        last_error = exc
        time.sleep(0.5)
    finally:
        sock.close()

raise SystemExit(f"UDP echo did not complete: {last_error}")
PY

for _ in $(seq 1 20); do
    if grep -q '\[lwip\] udp echo len=' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

cleanup
trap - EXIT

grep -q '\[net\] virtio-net ready.*msix=1' "${SERIAL_LOG}"
grep -q '\[vblk\] virtio-blk ready.*msix=1' "${SERIAL_LOG}"
grep -q '\[boot\] mounted xv6fs root image on vblk0' "${SERIAL_LOG}"
grep -q '\[net\] virtio-net irq bottom half active' "${SERIAL_LOG}"
grep -q '\[lwip\] dhcp bound ip=' "${SERIAL_LOG}"
grep -q '\[lwip\] udp echo len=' "${SERIAL_LOG}"

echo "VirtIO q35 MSI-X smoke PASS"
tail -n 180 "${SERIAL_LOG}"
