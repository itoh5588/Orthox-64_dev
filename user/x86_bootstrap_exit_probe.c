/*
 * bootstrap task (カーネルが直接起こした最初のユーザータスク。ppid==0) が
 * exit したときの経路を実機 (QEMU) で確かめる探針。
 *
 * kernel/sys_task.c の sys_exit は `!current || current->ppid == 0` を
 * bootstrap task の exit として arch_halt_forever() へ落とす (別実装 29 組の
 * exit/wait4 統合、2026-09-13)。aarch64/riscv64 はこの分岐を smoke の
 * /bin/hello 経由で日常的に踏んでいるが、x86 は bootstrap task が ash
 * (shell) で、この環境の ash は対話プロンプトから exit を打っても
 * 標準入力を EOF にしても sys_exit を呼ばない制約があり (2026-09-13 に
 * 実機で確認)、x86 だけこの分岐を検証できていなかった。
 *
 * x86 は kernel/x86_64/init.c が Limine のモジュール sh.elf を elf_load で
 * 読んで最初のユーザータスクにするので、この ELF を sh.elf の位置に置けば
 * rootfs.img も ash も要らず、bootstrap task 自身として直接 exit(2) を
 * 呼べる (user/x86_straddle_probe.c と同じ手法)。
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

/* **入口で RSP を 16 バイト境界に揃える。**カーネルが積む初期スタックは
 * 8 バイト境界までしか揃っておらず、clang が SSE 命令を出すと #GP になる
 * (user/x86_straddle_probe.c で実測済みの罠。crt0 の無いこの探針でも
 * 同じ補正が要る) */
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
    u_write("BOOTSTRAP-EXIT-PROBE-START\n");

    /* ppid==0 (bootstrap task) 自身の exit。sys_exit がこの分岐で
     * arch_halt_forever() を呼んで戻らない想定。戻ってきたら分岐を
     * 踏めていない = 異常なので、そう分かるログを残す */
    sys3(SYS_EXIT, 7, 0, 0);

    u_write("BOOTSTRAP-EXIT-PROBE-RETURNED\n");
    for (;;) { }
}
