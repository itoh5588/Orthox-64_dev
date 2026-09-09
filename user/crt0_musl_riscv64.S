.section .text
.global _start
.type _start, @function
.extern __libc_start_main
.extern main
.extern _init
.extern _fini

_start:
    .option push
    .option norelax
    lla gp, __global_pointer$
    .option pop

    mv t0, sp
    ld a1, 0(t0)
    addi a2, t0, 8
    lla a0, main
    lla a3, _init
    lla a4, _fini
    li a5, 0
    andi sp, sp, -16
    call __libc_start_main

.loop:
    wfi
    j .loop
