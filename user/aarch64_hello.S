/* aarch64 の最小ユーザープログラム (P1)。
 *
 * **libc を使わない。** 目的は「ELF を読み込んで EL0 で走らせ、svc が
 * 共有システムコール層に届く」ことだけを確かめること。musl は P2。
 *
 *   x8 = システムコール番号 (asm-generic。riscv64 と同じ)
 *   x0-x5 = 引数 / x0 = 戻り値
 */
.section .text
.globl _start
_start:
    /* write(1, msg, len) */
    mov     x0, #1
    adrp    x1, msg
    add     x1, x1, :lo12:msg
    mov     x2, #(msg_end - msg)
    mov     x8, #64                 /* LINUX_SYS_WRITE */
    svc     #0

    /* **戻り値を確かめてから exit する。** write が失敗していても
     * exit(0) してしまうと「動いた」に見える */
    cmp     x0, #(msg_end - msg)
    b.ne    1f

    mov     x0, #0
    b       2f
1:
    mov     x0, #1                  /* write が期待どおりでない */
2:
    mov     x8, #93                 /* LINUX_SYS_EXIT */
    svc     #0

    /* exit が戻ってきたら、それ自体が異常 */
3:  b       3b

.section .rodata
msg:
    .ascii  "[EL0] hello from aarch64 userland via svc\n"
msg_end:
