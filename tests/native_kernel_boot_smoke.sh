#!/bin/bash
# native_kernel_boot_smoke.sh
# Two-phase test:
#   Phase 1 – Build kernel natively inside Orthox-64; /kernel.elf is written to xv6fs (/kbuild).
#   Phase 2 – Extract /kernel.elf from rootfs.img, build a boot ISO, verify the kernel boots.
set -euo pipefail

ISO="${1:-orthos.iso}"
ROOTFS_IMG="${ROOTFS_IMG:-rootfs.img}"
SERIAL_LOG_BUILD="${SERIAL_LOG_BUILD:-LOGs/native-kernel-boot-build.log}"
SERIAL_LOG_BOOT="${SERIAL_LOG_BOOT:-LOGs/native-kernel-boot-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/native-kernel-boot-qemu.out}"
BOOT_ISO="${BOOT_ISO:-native-kernel-boot.iso}"
NATIVE_KERNEL="native-kernel.elf"

BOOTCMD_BACKUP="$(mktemp)"
SCRIPT_BACKUP="$(mktemp)"
BUILD_BOOTCMD="$(mktemp)"
BUILD_SCRIPT="$(mktemp)"
BOOT_TESTCMD="$(mktemp)"
QEMU_PID=""
IMAGE_PATCHED=0

cleanup() {
    if [ -n "${QEMU_PID}" ] && kill -0 "${QEMU_PID}" 2>/dev/null; then
        kill "${QEMU_PID}" 2>/dev/null || true
        wait "${QEMU_PID}" 2>/dev/null || true
    fi
    if [ "${IMAGE_PATCHED}" = "1" ]; then
        python3 scripts/build_rootfs_xv6fs.py --replace /etc/bootcmd "${BOOTCMD_BACKUP}" "${ROOTFS_IMG}" >/dev/null || true
        python3 scripts/build_rootfs_xv6fs.py --replace /etc/native_kernel_build_smoke.sh "${SCRIPT_BACKUP}" "${ROOTFS_IMG}" >/dev/null || true
    fi
    rm -f "${BOOTCMD_BACKUP}" "${SCRIPT_BACKUP}" "${BUILD_BOOTCMD}" "${BUILD_SCRIPT}" "${BOOT_TESTCMD}"
    rm -rf native_boot_iso_root
}
trap cleanup EXIT

python3 scripts/build_rootfs_xv6fs.py --extract /etc/bootcmd "${BOOTCMD_BACKUP}" "${ROOTFS_IMG}" >/dev/null
python3 scripts/build_rootfs_xv6fs.py --extract /etc/native_kernel_build_smoke.sh "${SCRIPT_BACKUP}" "${ROOTFS_IMG}" >/dev/null

# ─────────────────────────────────────────────────────────────────────────────
# Phase 1: build kernel natively; /kernel.elf is saved to xv6fs (/kbuild)
# ─────────────────────────────────────────────────────────────────────────────
echo "=== Phase 1: native build ==="

cat > "${BUILD_SCRIPT}" <<'EOF'
export PATH=/bin:/usr/bin:/
echo native-kernel-build-start
mkdir -p /kbuild/kernel /kbuild/lwip/core/ipv4 /kbuild/lwip/netif
rm -f /kernel.elf
make -f /src/kernel-build/Makefile BUILD=/kbuild OUTPUT=/kernel.elf
status=$?
if [ "${status}" != "0" ]; then
    echo native-kernel-build-fail:${status}
    exit "${status}"
fi
test -s /kernel.elf || {
    echo native-kernel-build-fail:no-kernel-elf
    exit 1
}
echo native-kernel-build-pass
echo native-kernel-build-end
EOF

cat > "${BUILD_BOOTCMD}" <<'EOF'
/bin/ash /etc/native_kernel_build_smoke.sh
EOF

echo "Patching ${ROOTFS_IMG} boot script without rebuilding xv6fs..."
python3 scripts/build_rootfs_xv6fs.py --replace /etc/native_kernel_build_smoke.sh "${BUILD_SCRIPT}" "${ROOTFS_IMG}"
python3 scripts/build_rootfs_xv6fs.py --replace /etc/bootcmd "${BUILD_BOOTCMD}" "${ROOTFS_IMG}"
IMAGE_PATCHED=1

echo "Building ${ISO} (ROOTFS_REBUILD=0: preserving cached /kbuild .o files)..."
ROOTFS_REBUILD=0 make "${ISO}" > /tmp/native-kernel-boot-iso-build.out 2>&1

rm -f "${SERIAL_LOG_BUILD}" "${QEMU_OUT}"

QEMU_ARGS=(
    -M q35
    -cpu max
    -m 2G
    -cdrom "${ISO}"
    -boot d
    -display none
    -serial "file:${SERIAL_LOG_BUILD}"
    -no-reboot
    -drive if=none,id=rootfs,file="${ROOTFS_IMG}",format=raw
    -device virtio-blk-pci,drive=rootfs
)

echo "Running QEMU for native build (timeout 3600s)..."
qemu-system-x86_64 "${QEMU_ARGS[@]}" > "${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

for _ in $(seq 1 3600); do
    if grep -q "native-kernel-build-end" "${SERIAL_LOG_BUILD}" 2>/dev/null; then
        break
    fi
    if grep -q "EXCEPTION OCCURRED\|#PF(User):\|#UD" "${SERIAL_LOG_BUILD}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if ! grep -q "native-kernel-build-pass" "${SERIAL_LOG_BUILD}" 2>/dev/null; then
    echo "Phase 1 failed (build did not complete). Build log tail:"
    tail -n 30 "${SERIAL_LOG_BUILD}"
    exit 1
fi
echo "Phase 1 passed: native kernel built and saved to xv6fs."

# ─────────────────────────────────────────────────────────────────────────────
# Phase 2: extract /kernel.elf from rootfs.img, build boot ISO, verify boot
# ─────────────────────────────────────────────────────────────────────────────
echo "=== Phase 2: extract and boot ==="

python3 scripts/build_rootfs_xv6fs.py --extract /kernel.elf "${NATIVE_KERNEL}" "${ROOTFS_IMG}"

# Simple bootcmd for boot test. Patch it before copying rootfs.img into the
# boot ISO, and also keep the external virtio-blk image consistent.
cat > "${BOOT_TESTCMD}" <<'EOF'
echo native-kernel-boot-ok
EOF
python3 scripts/build_rootfs_xv6fs.py --replace /etc/bootcmd "${BOOT_TESTCMD}" "${ROOTFS_IMG}"

# Build boot ISO with native kernel (don't call make to avoid rootfs.img rebuild)
rm -rf native_boot_iso_root
mkdir -p native_boot_iso_root/boot/limine native_boot_iso_root/EFI/BOOT

cp "${NATIVE_KERNEL}"              native_boot_iso_root/boot/kernel.elf
cp user/sh.elf                     native_boot_iso_root/boot/sh.elf
cp "${ROOTFS_IMG}"                 native_boot_iso_root/boot/rootfs.img
cp iso/limine.conf                 native_boot_iso_root/boot/limine/limine.conf
cp Limine/limine-bios.sys \
   Limine/limine-bios-cd.bin \
   Limine/limine-uefi-cd.bin       native_boot_iso_root/boot/limine/
cp Limine/BOOTX64.EFI              native_boot_iso_root/EFI/BOOT/
cp Limine/BOOTIA32.EFI             native_boot_iso_root/EFI/BOOT/

xorriso -as mkisofs -v -R -r -J \
    -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    native_boot_iso_root -o "${BOOT_ISO}" 2>/dev/null
Limine/limine bios-install "${BOOT_ISO}"

echo "Built ${BOOT_ISO} ($(du -h "${BOOT_ISO}" | cut -f1))"

rm -f "${SERIAL_LOG_BOOT}"

QEMU_BOOT_ARGS=(
    -M q35
    -cpu max
    -m 2G
    -cdrom "${BOOT_ISO}"
    -boot d
    -display none
    -serial "file:${SERIAL_LOG_BOOT}"
    -no-reboot
    -drive if=none,id=rootfs,file="${ROOTFS_IMG}",format=raw
    -device virtio-blk-pci,drive=rootfs
)

echo "Booting native kernel (timeout 120s)..."
qemu-system-x86_64 "${QEMU_BOOT_ARGS[@]}" > "${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

for _ in $(seq 1 120); do
    if grep -q "native-kernel-boot-ok\|muslcheck: PASS" "${SERIAL_LOG_BOOT}" 2>/dev/null; then
        break
    fi
    if grep -q "EXCEPTION OCCURRED\|#PF(User):\|#UD" "${SERIAL_LOG_BOOT}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if grep -q "native-kernel-boot-ok\|muslcheck: PASS" "${SERIAL_LOG_BOOT}" 2>/dev/null; then
    echo "Test passed: native kernel boots successfully on Orthox-64."
    exit 0
fi

echo "Boot test failed. Serial log (tail):"
tail -n 50 "${SERIAL_LOG_BOOT}"
exit 1
