.section .bss
.align 16
riscv64_boot_stacks:
    .skip (65536 * 8)
riscv64_boot_stacks_top:

.section .text.start
.globl _start
.globl riscv64_secondary_start
.extern riscv64_early_main
.extern riscv64_secondary_main
.extern riscv64_wait_forever

_start:
    li t0, 8
    bgeu a0, t0, 1f
    slli t1, a0, 16
    la sp, riscv64_boot_stacks
    add sp, sp, t1
    li t0, 65536
    add sp, sp, t0
    # ブートスタックは hart id で切り分けるが、カーネル実行中の tp は
    # cpu index (共有スケジューラの CPU 番号)。boot hart は必ず cpu 0
    mv tp, zero
    call riscv64_early_main
1:
    tail riscv64_wait_forever

# SBI HSM (hart_start) から S モードで呼ばれる副 hart のエントリ。
# a0 = hartid, a1 = opaque(= cpu index)。satp=0 (bare) だがカーネルは恒等マップ
riscv64_secondary_start:
    li t0, 8
    bgeu a0, t0, 2f
    bgeu a1, t0, 2f
    slli t1, a0, 16
    la sp, riscv64_boot_stacks
    add sp, sp, t1
    li t0, 65536
    add sp, sp, t0
    # tp = cpu index (per-hart 配列と共有スケジューラの CPU 番号を一致させる)
    mv tp, a1
    mv a0, a1
    call riscv64_secondary_main
2:
    tail riscv64_wait_forever
