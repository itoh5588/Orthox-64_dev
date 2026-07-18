.section .bss
.align 12
riscv64_boot_stacks:
    .skip (4096 * 8)
riscv64_boot_stacks_top:

.section .text.start
.globl _start
.extern riscv64_early_main
.extern riscv64_wait_forever

_start:
    li t0, 8
    bgeu a0, t0, 1f
    slli t1, a0, 12
    la sp, riscv64_boot_stacks
    add sp, sp, t1
    li t0, 4096
    add sp, sp, t0
    call riscv64_early_main
1:
    tail riscv64_wait_forever
