.section .text
.globl riscv64_trap_entry
.extern riscv64_trap_dispatch

.p2align 2
riscv64_trap_entry:
    csrrw t0, sscratch, t0
    beqz t0, .Ltrap_fallback

    sd t1,   8(t0)
    sd t2,  16(t0)
    sd sp,  24(t0)
    ld sp,   0(t0)
    addi sp, sp, -288

    sd ra,   0(sp)
    ld t1,  24(t0)
    sd t1,   8(sp)
    sd gp,  16(sp)
    sd tp,  24(sp)
    csrr t1, sscratch
    sd t1,  32(sp)
    ld t1,   8(t0)
    sd t1,  40(sp)
    ld t1,  16(t0)
    sd t1,  48(sp)
    j .Ltrap_common

.Ltrap_fallback:
    csrr t0, sscratch
    addi sp, sp, -288

    sd ra,   0(sp)
    addi t1, sp, 288
    sd t1,   8(sp)
    sd gp,  16(sp)
    sd tp,  24(sp)
    sd t0,  32(sp)
    sd zero, 40(sp)
    sd t2,  48(sp)

.Ltrap_common:
    sd s0,  56(sp)
    sd s1,  64(sp)
    sd a0,  72(sp)
    sd a1,  80(sp)
    sd a2,  88(sp)
    sd a3,  96(sp)
    sd a4, 104(sp)
    sd a5, 112(sp)
    sd a6, 120(sp)
    sd a7, 128(sp)
    sd s2, 136(sp)
    sd s3, 144(sp)
    sd s4, 152(sp)
    sd s5, 160(sp)
    sd s6, 168(sp)
    sd s7, 176(sp)
    sd s8, 184(sp)
    sd s9, 192(sp)
    sd s10, 200(sp)
    sd s11, 208(sp)
    sd t3, 216(sp)
    sd t4, 224(sp)
    sd t5, 232(sp)
    sd t6, 240(sp)

    csrr t0, sepc
    sd t0, 248(sp)
    csrr t0, sstatus
    sd t0, 256(sp)
    csrr t0, scause
    sd t0, 264(sp)
    csrr t0, stval
    sd t0, 272(sp)
    sd zero, 280(sp)
    mv a0, sp
    call riscv64_trap_dispatch

    ld t0, 248(sp)
    csrw sepc, t0
    ld t0, 256(sp)
    csrw sstatus, t0

    ld ra,   0(sp)
    ld gp,  16(sp)
    ld tp,  24(sp)
    ld t0,  32(sp)
    ld t1,  40(sp)
    ld t2,  48(sp)
    ld s0,  56(sp)
    ld s1,  64(sp)
    ld a0,  72(sp)
    ld a1,  80(sp)
    ld a2,  88(sp)
    ld a3,  96(sp)
    ld a4, 104(sp)
    ld a5, 112(sp)
    ld a6, 120(sp)
    ld a7, 128(sp)
    ld s2, 136(sp)
    ld s3, 144(sp)
    ld s4, 152(sp)
    ld s5, 160(sp)
    ld s6, 168(sp)
    ld s7, 176(sp)
    ld s8, 184(sp)
    ld s9, 192(sp)
    ld s10, 200(sp)
    ld s11, 208(sp)
    ld t3, 216(sp)
    ld t4, 224(sp)
    ld t5, 232(sp)
    ld t6, 240(sp)
    ld sp,   8(sp)
    sret
