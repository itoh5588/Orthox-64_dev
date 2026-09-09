.section .text
.globl riscv64_trap_entry
.extern riscv64_trap_dispatch

.p2align 2
# ユーザーから来たかカーネルから来たかは sstatus.SPP で判定する。
#
# 以前は「sscratch が 0 ならカーネル」という規約にしていたが、
# riscv64_trap_set_kernel_stack() がカーネル実行中に sscratch へ非ゼロを書く
# 窓があり、そこで割り込みが入るとユーザー経路が選ばれて scratch->kernel_sp から
# sp を張り直す = 今まさに走っているカーネルスタックを踏み潰していた。
# SPP はトラップ前の特権モードをハードウェアが記録したもので、ソフト側の
# 書き込み順に依存しない。sscratch は常に per-hart scratch を指したままにする。
riscv64_trap_entry:
    csrrw t0, sscratch, t0     # t0 = &scratch, sscratch = 退避した元の t0
    sd t1,   8(t0)             # scratch.saved_t1
    sd sp,  24(t0)             # scratch.saved_sp (ユーザー/カーネルどちらでも)

    csrr t1, sstatus
    andi t1, t1, 0x100         # SPP: 1 = カーネル(S)から
    bnez t1, .Ltrap_have_sp
    ld sp,   0(t0)             # ユーザーから: このタスクのカーネルスタックへ
.Ltrap_have_sp:
    # カーネルから来た場合は現在の sp をそのまま使う (ネストして深くなるだけ)
    addi sp, sp, -288

    sd ra,   0(sp)
    ld t1,  24(t0)             # 元の sp
    sd t1,   8(sp)
    sd gp,  16(sp)
    sd tp,  24(sp)
    csrr t1, sscratch          # 元の t0
    sd t1,  32(sp)
    ld t1,   8(t0)             # 元の t1
    sd t1,  40(sp)
    sd t2,  48(sp)             # t2 はまだ潰していない
    # カーネル実行中の tp を hart index にする (SMP 用の CPU 識別)
    ld tp,  32(t0)
    # sscratch を戻す。以後は常に &scratch なのでネスト trap も同じ入口を通れる。
    # ここまでの入口処理中は SIE=0 (ハードウェアがトラップ時に落とす) なので、
    # scratch のフィールドが生きている間に割り込みがネストすることはない。
    csrw sscratch, t0

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
    mv a0, sp
    call riscv64_trap_dispatch

    ld t0, 248(sp)
    csrw sepc, t0
    ld t0, 256(sp)
    csrw sstatus, t0

    # sscratch は入口で &scratch に戻してあり、ユーザー/カーネルどちらへ帰るときも
    # そのままでよい (SPP 判定にしたので 0 に落とす必要が無い)
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
    ld t0,  32(sp)
    ld sp,   8(sp)
    sret
