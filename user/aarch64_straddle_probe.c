/*
 * **段が同じページを跨ぐ ELF** を読ませる探針の aarch64 版
 * (freestanding, svc 直叩き)。riscv64 / x86 版と同じことを見る。
 *
 * aarch64 は task_execve でディスクの ELF を読むので、台本が
 * /bin/straddle に置き、カーネルを AARCH64_INIT_PATH_VALUE=/bin/straddle で
 * 組む。
 *
 * 2026-09-12 まで aarch64 の arch_vm_update_page_flags は**後勝ち**で、
 * 共有ページは .data の R+W になり、**そこの命令が実行できなかった。**
 */

#include <stdint.h>

#define SYS_WRITE 64
#define SYS_EXIT  93

static long sys3(long num, long a0, long a1, long a2) {
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x8 __asm__("x8") = num;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static void u_write(const char* s) {
    long len = 0;
    while (s[len]) len++;
    sys3(SYS_WRITE, 1, (long)(uintptr_t)s, len);
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
    sys3(SYS_EXIT, code, 0, 0);
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

    /* **前提そのものを確かめる。**同じページに乗っていなければ何も検査して
     * いないので、緑にせず落ちる */
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
