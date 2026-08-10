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

#include "aarch64/task.h"
#include "aarch64/trap.h"

.section .text

/* uint64_t aarch64_link_va_offset(void)
 *
 * リンカスクリプトが使った KERNEL_VA_OFFSET を返す。
 * **include/aarch64/vm.h の値と一致しているかを実行時に確かめるため。**
 * ずれていると、MMU を入れて上位 VA へ飛んだ瞬間に沈黙する。
 *
 * C から直接は参照できない (値が大きすぎて adrp が届かない)。
 * リテラルプール経由なら取れる */
.globl aarch64_link_va_offset
aarch64_link_va_offset:
    ldr     x0, =KERNEL_VA_OFFSET
    ret

/* void aarch64_vm_enter_high(uint64_t new_sp, uint64_t cont)
 *
 * MMU を入れた直後に、**物理アドレスの世界から上位 VA の世界へ移る**。
 * 戻ってこない。
 *
 * いまのスタックには物理アドレスの戻り先が積まれているので、そのまま
 * 使い続けると恒等マッピングを外した瞬間に破綻する。**新しいスタックに
 * 乗り換えてから飛ぶ。**
 *
 * x30 (戻り先) も物理のままなので、飛んだ先では ret しないこと。
 * 呼び出し元 (aarch64_vm_init) には二度と戻らない。 */
.globl aarch64_vm_enter_high
aarch64_vm_enter_high:
    mov     sp, x0
    mov     x29, xzr        /* フレームポインタを切る。物理の鎖を残さない */
    mov     x30, xzr        /* 戻り先も切る。ここから先で ret したら止まる */
    br      x1

/* 何もせず待つ。**上位 VA 側に置く。** start.S (.boot) からは PC 相対で
 * 届かないので、あちらは自前のループで完結させてある */
.globl aarch64_wait_forever
aarch64_wait_forever:
    wfe
    b       aarch64_wait_forever

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

/* void aarch64_task_enter_initial_user_context(const struct arch_task_context* ctx)
 *
 * 共有タスク層が「最初にユーザーへ降りる」ときに呼ぶ (M3c-2b)。
 * riscv64 の riscv64_task_enter_initial_user_context に相当する。**戻らない。**
 *
 * 上の aarch64_enter_el0 と違い、**フレームに組み立てられた全レジスタを
 * そのまま復元する**。execve が引数を x0-x2 に、sp を SP_EL0 に仕込んで
 * あるので、ここで作り直してはいけない。
 *
 * x0 が ctx。user_frame の先頭へ進めてから読む。 */
.globl aarch64_task_enter_initial_user_context
aarch64_task_enter_initial_user_context:
    add     x0, x0, #AARCH64_TASKCTX_USER_FRAME

    ldr     x1, [x0, #AARCH64_FRAME_ELR]
    msr     elr_el1, x1
    ldr     x1, [x0, #AARCH64_FRAME_SPSR]
    msr     spsr_el1, x1
    ldr     x1, [x0, #AARCH64_FRAME_SP_EL0]
    msr     sp_el0, x1

    /* **x0 を最後に読む。** 途中で潰すとフレームの場所を見失う。
     * ldp は base と dest が重なると未定義なので、x0 だけ ldr で拾う */
    ldr     x1,  [x0, #8 * 1]
    ldp     x2,  x3,  [x0, #16 * 1]
    ldp     x4,  x5,  [x0, #16 * 2]
    ldp     x6,  x7,  [x0, #16 * 3]
    ldp     x8,  x9,  [x0, #16 * 4]
    ldp     x10, x11, [x0, #16 * 5]
    ldp     x12, x13, [x0, #16 * 6]
    ldp     x14, x15, [x0, #16 * 7]
    ldp     x16, x17, [x0, #16 * 8]
    ldp     x18, x19, [x0, #16 * 9]
    ldp     x20, x21, [x0, #16 * 10]
    ldp     x22, x23, [x0, #16 * 11]
    ldp     x24, x25, [x0, #16 * 12]
    ldp     x26, x27, [x0, #16 * 13]
    ldp     x28, x29, [x0, #16 * 14]
    ldr     x30, [x0, #8 * 30]
    ldr     x0,  [x0, #8 * 0]
    eret

/* fork した子が最初に走り出す場所は kernel/aarch64/task.c に C で置いた。
 * get_current_task() から ctx を取って上の
 * aarch64_task_enter_initial_user_context へ渡すだけなので、
 * ctx のオフセットを .S に持たせるより C のほうが壊れにくい
 * (riscv64 も kernel/riscv64/task.c に C で置いている) */

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
