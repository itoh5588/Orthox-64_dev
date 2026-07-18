.section .bss
.align 16
riscv64_boot_stacks:
    .skip (65536 * 8)
riscv64_boot_stacks_top:

.section .text.start
.globl _start
.extern riscv64_early_main
.extern riscv64_wait_forever

_start:
    li t0, 8
    bgeu a0, t0, 1f
    slli t1, a0, 16
    la sp, riscv64_boot_stacks
    add sp, sp, t1
    li t0, 65536
    add sp, sp, t0
    call riscv64_early_main
1:
    tail riscv64_wait_forever
