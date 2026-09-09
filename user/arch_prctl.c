#include <stdint.h>

// 直接システムコールを発行する関数
static int64_t syscall(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    uint64_t ret;
    __asm__ volatile (
        "syscall\n"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );
    return (int64_t)ret;
}

#define SYS_ARCH_PRCTL 158

int arch_prctl(int code, unsigned long addr) {
    return (int)syscall(SYS_ARCH_PRCTL, (uint64_t)code, (uint64_t)addr, 0);
}
