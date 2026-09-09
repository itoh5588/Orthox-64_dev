#!/bin/bash
set -euo pipefail

ISO="${ISO:-orthos.iso}"

run_step() {
    local name="$1"
    shift
    echo "== final-smoke: ${name}: start =="
    "$@"
    echo "== final-smoke: ${name}: PASS =="
}

run_step "kernel"              make kernel.elf
run_step "busybox"             bash tests/musl_busybox_smoke.sh "${ISO}"
run_step "toolchain"           bash tests/musl_toolchain_smoke.sh
run_step "dynlink-realapp"     bash tests/dynlink_realapp_smoke.sh "${ISO}"
run_step "native-toolchain"    bash tests/native_toolchain_work_smoke.sh "${ISO}"
run_step "vblk"                bash tests/virtio_blk_inflight_smoke.sh "${ISO}"
run_step "net"                 bash tests/virtio_net_irq_smoke.sh "${ISO}"
run_step "q35-smp-irq-bottom-half" bash tests/irq_bottom_half_smp_stress_smoke.sh "${ISO}"

if [ "${RUN_NATIVE_KERNEL_BOOT_SMOKE:-0}" = "1" ]; then
    run_step "native-kernel-boot" bash tests/native_kernel_boot_smoke.sh "${ISO}"
else
    echo "== final-smoke: native-kernel-boot: SKIP =="
    echo "   set RUN_NATIVE_KERNEL_BOOT_SMOKE=1 to run the long native build+boot smoke"
fi

echo "final smoke suite PASS"
