/*
 * **段が同じページを跨ぐ ELF** を読ませる探針の x86_64 版
 * (freestanding, syscall 命令を直叩き)。riscv64 版
 * (user/riscv64_straddle_probe.c) と同じことを x86 で見る。
 *
 * x86 は kernel/x86_64/init.c が **Limine のモジュール sh.elf を
 * elf_load で読んで最初のユーザータスクにする**ので、この ELF を
 * sh.elf の位置に置けば、rootfs.img が無くても検査できる。
 *
 * 2026-09-12 まで x86 の arch_vm_update_page_flags は何もしないスタブ
 * だったので、**共有ページには先に触った段 (.text の R+X) が残り、
 * .data 側に書けなかった。**aarch64 / riscv64 は逆に後勝ちで
 * R+W になり、実行できなかった。
 */

#include <stdint.h>

#define SYS_WRITE 1
#define SYS_EXIT  60

static int64_t sys3(int64_t num, int64_t a1, int64_t a2, int64_t a3) {
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(num), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static void u_write(const char* s) {
    int64_t len = 0;
    while (s[len]) len++;
    sys3(SYS_WRITE, 1, (int64_t)(uintptr_t)s, len);
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

static void u_exit(int64_t code) {
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

/* **入口で RSP を 16 バイト境界に揃える。**カーネルが積む初期スタックは
 * 8 バイト境界までしか揃っておらず (実測 RSP=0x7FFFFFFFEDF8)、clang が
 * u_puthex のループを SSE にすると movapd が #GP を出す。user/crt0.S が
 * `andq $-16, %rsp` でやっているのと同じ補正を、crt0 の無いこの探針でも行う */
__asm__(
    ".section .text\n"
    ".globl _start\n"
    ".type _start, @function\n"
    "_start:\n"
    "    xor %rbp, %rbp\n"
    "    andq $-16, %rsp\n"
    "    call probe_main\n"
    "1:  hlt\n"
    "    jmp 1b\n"
);

void probe_main(void);

void probe_main(void) {
    uint64_t code_page = ((uint64_t)(uintptr_t)&tail_add) >> 12;
    uint64_t data_page = ((uint64_t)(uintptr_t)&g_data) >> 12;

    u_write("STRADDLE-START\n");
    u_write("  tail_add: "); u_puthex((uint64_t)(uintptr_t)&tail_add); u_write("\n");
    u_write("  g_data  : "); u_puthex((uint64_t)(uintptr_t)&g_data);   u_write("\n");

    /* **前提そのものを確かめる。**同じページに乗っていなければ、この探針は
     * 何も検査していない。緑にせず、そう言って落ちる */
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
