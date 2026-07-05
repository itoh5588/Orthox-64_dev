#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/virtio-net-irq-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/virtio-net-irq-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
BOOTCMD_BACKUP="$(mktemp)"
QEMU_PID=""

cleanup() {
    if [ -n "${QEMU_PID}" ] && kill -0 "${QEMU_PID}" 2>/dev/null; then
        kill "${QEMU_PID}" 2>/dev/null || true
        wait "${QEMU_PID}" 2>/dev/null || true
    fi
    if [ -f "${BOOTCMD_BACKUP}" ]; then
        cp "${BOOTCMD_BACKUP}" "${BOOTCMD_PATH}"
        rm -f "${BOOTCMD_BACKUP}"
    fi
}
trap cleanup EXIT

cp "${BOOTCMD_PATH}" "${BOOTCMD_BACKUP}"

cat > "${BOOTCMD_PATH}" <<'EOF'
echo virtio-net-irq-smoke-bootcmd
EOF

make orthos.iso >/tmp/virtio-net-irq-build.out

rm -f "${SERIAL_LOG}" "${QEMU_OUT}"

qemu-system-x86_64 \
    -machine pc \
    -cpu qemu64 \
    -m 2G \
    -cdrom "${ISO}" \
    -boot d \
    -display none \
    -audio none \
    -netdev user,id=net0,hostfwd=udp::12345-:12345 \
    -device virtio-net-pci,netdev=net0 \
    -serial "file:${SERIAL_LOG}" \
    -k en-us >"${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

echo "Waiting for VirtIO Net DHCP and IRQ bottom half..."
for _ in $(seq 1 90); do
    if grep -q '\[lwip\] dhcp bound ip=' "${SERIAL_LOG}" 2>/dev/null &&
       grep -q '\[net\] virtio-net irq bottom half active' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

python3 - <<'PY'
import socket
import sys
import time

payload = b"orthox-net-irq-smoke"
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

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

grep -q '\[net\] virtio-net ready' "${SERIAL_LOG}"
grep -q 'irq=0x' "${SERIAL_LOG}"
grep -q '\[net\] virtio-net irq bottom half active' "${SERIAL_LOG}"
grep -q '\[lwip\] dhcp bound ip=' "${SERIAL_LOG}"
grep -q '\[lwip\] udp echo len=' "${SERIAL_LOG}"

echo "VirtIO Net IRQ smoke PASS"
tail -n 160 "${SERIAL_LOG}"
