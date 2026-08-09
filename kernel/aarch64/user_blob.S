/* EL0 で動く最小のユーザーコード (M3a)。
 *
 * カーネルの中に埋め込んである。**ELF を読み込む仕組みはまだ無い** (M3c)。
 * ここでの目的は「EL0 に降りて svc #0 で戻ってこられる」ことだけを通すこと。
 *
 * 置き場所はリンカスクリプトの .user_text / .user_data。
 * カーネルの区画と同じページに混ざらないよう 4KB 境界で切り分けてある。
 * 混ざっていると、ユーザー権限を与えた瞬間にカーネルの中身まで
 * EL0 から見えてしまう。
 *
 * ここのコードは **PC 相対だけで書く**。EL0 のページを別の VA に張り替えても
 * そのまま動くようにするため (M3b でアドレス空間を分けるときに効く)。
 * adr は PC 相対なので、そのまま使える。
 *
 * システムコールの番号と空回しの回数は include/aarch64/usermode.h で
 * カーネル側と共有する。**両方に書くと必ずずれる。**
 */
#include "aarch64/usermode.h"

.section .user_text, "ax"
.globl aarch64_user_entry

/* 入口。x0 にカーネルが「わざと触らせるカーネルのアドレス」を渡してくる */
aarch64_user_entry:
    mov     x19, x0             /* 触ると落ちるはずのアドレスを取っておく */

    /* --- 1. write(1, msg1, len1) --------------------------------------
     * ここに文字が出れば、EL0 に降りて svc で戻ってこられている */
    mov     x8, #AARCH64_NR_WRITE
    mov     x0, #1
    adr     x1, user_msg1
    mov     x2, #(user_msg1_end - user_msg1)
    svc     #0

    /* --- 2. 空回し ------------------------------------------------------
     * この間にタイマ割り込みが上がるはず。**EL0 実行中の IRQ はベクタの
     * 別の入口 (+0x480) に来る**ので、ここが効いていることの確認になる。
     * M2 までは「カーネル実行中の IRQ」(+0x280) しか通っていない */
    movz    x20, #(AARCH64_USER_SPIN >> 16), lsl #16
    movk    x20, #(AARCH64_USER_SPIN & 0xffff)
1:
    subs    x20, x20, #1
    b.ne    1b

    /* --- 3. カーネルのアドレスをわざと読む ------------------------------
     * ページは張ってあるが AP が EL1 だけなので、permission fault に
     * なるはず。カーネル側が受け止めて命令 1 つぶん進めて戻してくれる
     * (M2 の MMU 探針と同じ仕組み) */
    ldr     x21, [x19]

    /* --- 4. write(1, msg2, len2) ---------------------------------------
     * ここに出れば、EL0 で落ちた後の再開まで成立している */
    mov     x8, #AARCH64_NR_WRITE
    mov     x0, #1
    adr     x1, user_msg2
    mov     x2, #(user_msg2_end - user_msg2)
    svc     #0

    /* --- 5. exit(0) ----------------------------------------------------- */
    mov     x8, #AARCH64_NR_EXIT
    mov     x0, #0
    svc     #0

    /* exit が戻ってきたら異常。ここで止める */
2:
    b       2b

user_msg1:
    .ascii  "  [EL0] hello from user mode\n"
user_msg1_end:

user_msg2:
    .ascii  "  [EL0] resumed after permission fault\n"
user_msg2_end:

/* ユーザーのスタック。EL0 は SP_EL0 を使う (カーネルの SP_EL1 とは別物)。
 * .user_data に置いて EL0 から読み書きできる権限で張る */
.section .user_data, "aw"
.align 16
.globl aarch64_user_stack
aarch64_user_stack:
    .skip 4096
.globl aarch64_user_stack_top
aarch64_user_stack_top:
