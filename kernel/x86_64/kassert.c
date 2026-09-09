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
