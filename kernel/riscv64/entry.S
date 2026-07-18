.section .text
.globl arch_context_switch
.globl riscv64_enter_user_from_frame
.globl riscv64_run_on_stack
.globl riscv64_activate_address_space_and_enter_user

arch_context_switch:
    sd ra,  16(a1)
    sd sp,  24(a1)
    sd s0,  32(a1)
    sd s1,  40(a1)
    sd s2,  48(a1)
    sd s3,  56(a1)
    sd s4,  64(a1)
    sd s5,  72(a1)
    sd s6,  80(a1)
    sd s7,  88(a1)
    sd s8,  96(a1)
    sd s9, 104(a1)
    sd s10, 112(a1)
    sd s11, 120(a1)

    ld t0,   0(a0)
    beqz t0, .Lswitch_regs
    srli t0, t0, 12
    li t1, 8
    slli t1, t1, 60
    or t0, t0, t1
    csrw satp, t0
    sfence.vma zero, zero

.Lswitch_regs:
    ld ra,  16(a0)
    ld sp,  24(a0)
    ld s0,  32(a0)
    ld s1,  40(a0)
    ld s2,  48(a0)
    ld s3,  56(a0)
    ld s4,  64(a0)
    ld s5,  72(a0)
    ld s6,  80(a0)
    ld s7,  88(a0)
    ld s8,  96(a0)
    ld s9, 104(a0)
    ld s10, 112(a0)
    ld s11, 120(a0)
    ret

riscv64_run_on_stack:
    mv sp, a0
    jr a1

riscv64_activate_address_space_and_enter_user:
    srli t0, a0, 12
    li t1, 8
    slli t1, t1, 60
    or t0, t0, t1
    csrw satp, t0
    sfence.vma zero, zero
    mv a0, a1
    j riscv64_enter_user_from_frame

riscv64_enter_user_from_frame:
    ld ra,   0(a0)
    ld sp,   8(a0)
    ld gp,  16(a0)
    ld tp,  24(a0)
    ld t0,  32(a0)
    ld t1,  40(a0)
    ld t2,  48(a0)
    ld s0,  56(a0)
    ld s1,  64(a0)
    ld a1,  80(a0)
    ld a2,  88(a0)
    ld a3,  96(a0)
    ld a4, 104(a0)
    ld a5, 112(a0)
    ld a6, 120(a0)
    ld a7, 128(a0)
    ld s2, 136(a0)
    ld s3, 144(a0)
    ld s4, 152(a0)
    ld s5, 160(a0)
    ld s6, 168(a0)
    ld s7, 176(a0)
    ld s8, 184(a0)
    ld s9, 192(a0)
    ld s10, 200(a0)
    ld s11, 208(a0)
    ld t3, 216(a0)
    ld t4, 224(a0)
    ld t5, 232(a0)
    ld t6, 240(a0)
    ld t0, 248(a0)
    csrw sepc, t0
    ld t0, 256(a0)
    csrw sstatus, t0
    ld a0,  72(a0)
    sret
