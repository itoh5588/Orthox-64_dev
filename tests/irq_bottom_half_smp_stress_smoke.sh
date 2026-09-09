#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
OVMF="${OVMF:-Web/wasabi/third_party/ovmf/RELEASEX64_OVMF.fd}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/irq-bottom-half-smp-stress-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/irq-bottom-half-smp-stress-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
BOOTCMD_BACKUP="$(mktemp)"
QEMU_PID=""

cleanup() {
    if [ -n "${QEMU_PID}" ] && kill -0 "${QEMU_PID}" 2>/dev/null; then
        kill "${QEMU_PID}" 2>/dev/null || true
        wait "${QEMU_PID}" 2>/dev/null || true
    fi
    if [ -f "${BOOTCMD_BACKUP}" ]; then
        if [ -s "${BOOTCMD_BACKUP}" ]; then
            cp "${BOOTCMD_BACKUP}" "${BOOTCMD_PATH}"
        else
            : > "${BOOTCMD_PATH}"
        fi
        rm -f "${BOOTCMD_BACKUP}"
    fi
}
trap cleanup EXIT

if [ ! -f "${OVMF}" ]; then
    echo "OVMF firmware not found: ${OVMF}" >&2
    exit 1
fi

if [ -f "${BOOTCMD_PATH}" ]; then
    cp "${BOOTCMD_PATH}" "${BOOTCMD_BACKUP}"
else
    : > "${BOOTCMD_BACKUP}"
fi

mkdir -p "$(dirname "${BOOTCMD_PATH}")"
cat > "${BOOTCMD_PATH}" <<'EOF'
echo irq-bottom-half-smp-stress-bootcmd
runqstat.elf
smpstress.elf 2
vblkstress
runqstat.elf
EOF

make orthos.iso >/tmp/irq-bottom-half-smp-stress-build.out

rm -f "${SERIAL_LOG}" "${QEMU_OUT}"

qemu-system-x86_64 \
    -machine q35 \
    -cpu qemu64 \
    -smp 2 \
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

echo "Waiting for SMP IRQ/bottom-half stress boot..."
for _ in $(seq 1 150); do
    if grep -q '\[smp\] started_cpus=2' "${SERIAL_LOG}" 2>/dev/null &&
       grep -q '\[net\] virtio-net ready.*msix=1' "${SERIAL_LOG}" 2>/dev/null &&
       grep -q '\[vblk\] virtio-blk ready.*msix=1' "${SERIAL_LOG}" 2>/dev/null &&
       grep -q '\[net\] virtio-net irq bottom half active' "${SERIAL_LOG}" 2>/dev/null &&
       grep -q 'runqstat: cpus=2' "${SERIAL_LOG}" 2>/dev/null &&
       grep -q 'smpstress: PASS rounds=2' "${SERIAL_LOG}" 2>/dev/null &&
       grep -q 'vblkstress: PASS' "${SERIAL_LOG}" 2>/dev/null &&
       grep -q '\[lwip\] dhcp bound ip=' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

python3 - <<'PY'
import socket
import time

deadline = time.time() + 30
received = 0
last_error = None

for i in range(32):
    payload = f"orthox-smp-irq-bh-{i:02d}".encode()
    while time.time() < deadline:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(1.0)
        try:
            sock.sendto(payload, ("127.0.0.1", 12345))
            data, _ = sock.recvfrom(256)
            if data == payload:
                received += 1
                break
            last_error = RuntimeError(f"unexpected echo payload: {data!r}")
        except Exception as exc:
            last_error = exc
            time.sleep(0.1)
        finally:
            sock.close()
    else:
        raise SystemExit(f"UDP burst stopped at {received}/32: {last_error}")

if received != 32:
    raise SystemExit(f"UDP burst only completed {received}/32")
PY

for _ in $(seq 1 20); do
    UDP_ECHO_COUNT="$(grep -c '\[lwip\] udp echo len=' "${SERIAL_LOG}" 2>/dev/null || true)"
    if [ "${UDP_ECHO_COUNT}" -ge 16 ]; then
        break
    fi
    sleep 1
done

cleanup
trap - EXIT

UDP_ECHO_COUNT="$(grep -c '\[lwip\] udp echo len=' "${SERIAL_LOG}" 2>/dev/null || true)"

grep -q '\[smp\] started_cpus=2' "${SERIAL_LOG}"
grep -q '\[net\] virtio-net ready.*msix=1' "${SERIAL_LOG}"
grep -q '\[vblk\] virtio-blk ready.*msix=1' "${SERIAL_LOG}"
grep -qE '\[boot\] mounted xv6fs root image on (vblk0|bootimg0)' "${SERIAL_LOG}"
grep -q '\[net\] virtio-net irq bottom half active' "${SERIAL_LOG}"
grep -q '\[lwip\] dhcp bound ip=' "${SERIAL_LOG}"
grep -q 'runqstat: cpus=2' "${SERIAL_LOG}"
grep -q 'smpstress: PASS rounds=2' "${SERIAL_LOG}"
grep -q 'vblkstress: PASS' "${SERIAL_LOG}"
if [ "${UDP_ECHO_COUNT}" -lt 16 ]; then
    echo "Expected at least 16 UDP echo log entries, got ${UDP_ECHO_COUNT}" >&2
    exit 1
fi

echo "SMP IRQ/bottom-half stress smoke PASS"
tail -n 260 "${SERIAL_LOG}"
