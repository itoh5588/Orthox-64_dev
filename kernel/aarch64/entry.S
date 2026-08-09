/* EL0 への出入り (M3a)。riscv64 の kernel/riscv64/entry.S に相当する。
 *
 * eret は「ELR_EL1 の場所へ SPSR_EL1 の状態で戻る」命令。EL1 から EL0 へ
 * 降りるのも「戻る」として表現する。riscv64 の sret と同じ考え方。
 *
 * **EL0 と EL1 で sp が別レジスタ** なのが riscv64 との一番の違い。
 * EL0 は SP_EL0、EL1 は SP_EL1。例外で EL1 に入ると CPU が自動で SP_EL1 へ
 * 切り替えるので、カーネルのスタックをユーザーに壊される心配が無い。
 * riscv64 で sscratch を使って手で入れ替えていた所が、ハードウェアで済む。
 */

.section .text

/* void aarch64_enter_el0(uint64_t entry, uint64_t user_sp, uint64_t arg0)
 *
 *   x0 = EL0 で走らせる場所
 *   x1 = ユーザーのスタック先頭 (SP_EL0 に入れる)
 *   x2 = ユーザーの x0 に渡す値
 *
 * EL0 から抜けるときは、例外ハンドラが戻り先を aarch64_user_abort_landing に
 * 差し替えて eret してくる。そこで下の epilogue に着地し、呼び出し元へ戻る。 */
.globl aarch64_enter_el0
aarch64_enter_el0:
    /* 呼び出し元へ戻れるように callee-saved を積む。**EL0 から帰ってくる
     * のは eret 経由の別経路**なので、着地点でも同じ形で降ろせるよう
     * sp の使い方を揃えておく */
    stp     x29, x30, [sp, #-96]!
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]

    msr     sp_el0, x1          /* EL0 のスタック */
    msr     elr_el1, x0         /* EL0 で走り始める場所 */

    /* SPSR_EL1 = 0。M[3:0] = 0 で EL0t、DAIF も全部 0 で割り込みを開ける。
     * **ここで I を立てると EL0 でタイマが上がらなくなる** */
    msr     spsr_el1, xzr

    mov     x0, x2              /* ユーザーの x0 */

    /* カーネルの値を EL0 へ持ち込まない。残しておくと、ユーザーから
     * カーネルのアドレスや中身が読めてしまう */
    mov     x1, xzr
    mov     x2, xzr
    mov     x3, xzr
    mov     x4, xzr
    mov     x5, xzr
    mov     x6, xzr
    mov     x7, xzr
    mov     x8, xzr
    mov     x9, xzr
    mov     x10, xzr
    mov     x11, xzr
    mov     x12, xzr
    mov     x13, xzr
    mov     x14, xzr
    mov     x15, xzr
    mov     x16, xzr
    mov     x17, xzr
    mov     x18, xzr
    mov     x19, xzr
    mov     x20, xzr
    mov     x21, xzr
    mov     x22, xzr
    mov     x23, xzr
    mov     x24, xzr
    mov     x25, xzr
    mov     x26, xzr
    mov     x27, xzr
    mov     x28, xzr
    mov     x29, xzr
    mov     x30, xzr

    eret

/* EL0 を終えたときの着地点。
 *
 * 例外ハンドラが積んであるフレームの ELR をここに、SPSR を EL1h に書き替えて
 * から RESTORE_ALL + eret する。ベクタの RESTORE_ALL が SP_EL1 を元に戻して
 * いるので、**上で積んだ callee-saved がそのまま残っている**。 */
.globl aarch64_user_abort_landing
aarch64_user_abort_landing:
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #64]
    ldp     x27, x28, [sp, #80]
    ldp     x29, x30, [sp], #96
    ret
