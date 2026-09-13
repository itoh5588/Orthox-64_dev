#include "kassert.h"

extern void puts(const char *s);
extern void puthex(uint64_t v);

static void kernel_halt_forever(void) __attribute__((noreturn));

static void kernel_halt_forever(void) {
    __asm__ volatile("cli" ::: "memory");
    for (;;) {
        __asm__ volatile("hlt" ::: "memory");
    }
}

/* include/linux_syscall.h の arch hook。aarch64 / riscv64 は既に持っていたが
 * x86 には無く、**カーネルが直接起こした最初のユーザータスク (親が居ない)
 * が exit したときに使う場所が無かった** (2026-09-13、exit を 3 アーキ共通に
 * 畳んだときに追加。kernel/sys_task.c の sys_exit を参照)。中身は
 * kernel_halt_forever と同じ cli+hlt ループ */
void arch_halt_forever(void) {
    kernel_halt_forever();
}

void kernel_panic(const char *file, int line, const char *func,
                  const char *expr) {
    puts("\r\n*** KERNEL PANIC ***\r\n");
    puts("expr: ");
    puts(expr ? expr : "(null)");
    puts("\r\nfunc: ");
    puts(func ? func : "(null)");
    puts("\r\nfile: ");
    puts(file ? file : "(null)");
    puts(":0x");
    puthex((uint64_t)(uint32_t)line);
    puts("\r\nHALTING...\r\n");
    kernel_halt_forever();
}
