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
    csrci sstatus, 2           # SIE を閉じる (訳は riscv64_enter_user_from_frame)
    srli t0, a0, 12
    li t1, 8
    slli t1, t1, 60
    or t0, t0, t1
    csrw satp, t0
    sfence.vma zero, zero
    mv a0, a1
    j riscv64_enter_user_from_frame

riscv64_enter_user_from_frame:
    # **ユーザーのレジスタを積む前に割り込みを閉じる (2026-09-19)。**
    # カーネルは実行中も割り込みを受ける (riscv64_interrupts_enable)。ここを
    # 開けたまま sp をユーザーの値にした後で割り込みが入ると、入口は
    # 「カーネルから来た」(SPP=1) と見て今の sp = ユーザーのスタックに
    # trap frame を積む。fork の子や exec の最初の遷移はここを通る。
    # cowstress (4 hart) で、ユーザーのスタックがカーネルの番地で上書き
    # されて飛ぶ / CoW の読み取り専用のスタックに積もうとして入口ごと
    # フォルトし続ける、として出た。aarch64 は同じ穴を P3-4 で塞いである
    # (aarch64_task_fork_child_return の msr daifset)。
    # sret の直前に sstatus をフレームの値で書き戻すので、ユーザーには
    # 開いて降りる
    csrci sstatus, 2           # SIE
    ld ra,   0(a0)
    ld sp,   8(a0)
    ld gp,  16(a0)
    ld tp,  24(a0)
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
    ld t0,  32(a0)
    ld a0,  72(a0)
    sret
