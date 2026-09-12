/*
 * **段が同じページを跨ぐ ELF** を読ませる探針 (freestanding, ecall 直叩き)。
 *
 * リンカ台本 scripts/user-riscv64-straddle.ld が .text (R+X) と .data (R+W)
 * を境目で整列させずに並べるので、**境目のページは両方の段に覆われる。**
 * 通常のツールチェインはここを 1 ページ空けるため、Orthox が現に読んでいる
 * ELF にこの形は無い (out/ と ports/ の全 ELF を調べた)。
 *
 * kernel/elf.c は 1 ページを 1 度しか貼らず、2 つ目の段では
 * arch_vm_update_page_flags を呼ぶ。**その属性が「和」でないと動かない:**
 *
 *   後勝ち (aarch64 / riscv64 の元の形)  R+W になり tail_add が実行できない
 *   先勝ち (x86 の元のスタブ)            R+X になり g_data に書けない
 *
 * どちらの壊れ方も同じ 1 本で捕まえられるよう、**共有ページのコードを呼び、
 * 共有ページのデータに書く。**
 */

#include <stdint.h>

#define SYS_WRITE 64
#define SYS_EXIT  93

static long syscall3(long num, long a0, long a1, long a2) {
    register long ra0 __asm__("a0") = a0;
    register long ra1 __asm__("a1") = a1;
    register long ra2 __asm__("a2") = a2;
    register long ra7 __asm__("a7") = num;
    __asm__ volatile("ecall" : "+r"(ra0) : "r"(ra1), "r"(ra2), "r"(ra7) : "memory");
    return ra0;
}

static void u_write(const char* s) {
    long len = 0;
    while (s[len]) len++;
    syscall3(SYS_WRITE, 1, (long)(uintptr_t)s, len);
}

static void u_puthex(uint64_t v) {
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        uint64_t nib = (v >> ((15 - i) * 4)) & 0xF;
        buf[2 + i] = (char)(nib < 10 ? '0' + nib : 'a' + (nib - 10));
    }
    buf[18] = '\0';
    u_write(buf);
}

static void u_exit(long code) {
    syscall3(SYS_EXIT, code, 0, 0);
    for (;;) { }
}

/* **.data の先頭。**台本が .text の直後に置くので、共有ページの書ける側 */
volatile long g_data __attribute__((section(".data.head"))) = 1;

/* **.text の末尾。**共有ページの実行する側 */
__attribute__((noinline, section(".text.tail")))
static long tail_add(long a) {
    return a + 41;
}

void _start(void) {
    uint64_t code_page = ((uint64_t)(uintptr_t)&tail_add) >> 12;
    uint64_t data_page = ((uint64_t)(uintptr_t)&g_data) >> 12;

    u_write("STRADDLE-START\n");
    u_write("  tail_add: "); u_puthex((uint64_t)(uintptr_t)&tail_add); u_write("\n");
    u_write("  g_data  : "); u_puthex((uint64_t)(uintptr_t)&g_data);   u_write("\n");

    /* **前提そのものを確かめる。**同じページに乗っていなければ、この探針は
     * 何も検査していない (リンカの出力次第で境目が丁度ページ境界に来ると
     * こうなる)。緑にせず、そう言って落ちる */
    if (code_page != data_page) {
        u_write("STRADDLE-NOT-SHARED\n");
        u_exit(2);
    }

    g_data = 1;                       /* 共有ページの書ける側 */
    if (tail_add(g_data) != 42) {     /* 共有ページの実行する側 */
        u_write("STRADDLE-BAD\n");
        u_exit(1);
    }
    u_write("STRADDLE-OK\n");
    u_exit(0);
}
