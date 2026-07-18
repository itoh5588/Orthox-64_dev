// riscv64 単発ユーザープログラム (freestanding)
// カーネルの /bootstrap-user として埋め込まれ、execve で起動される。
// Linux riscv64 syscall ABI (ecall, a7=番号) を直接使う。

#include <stdint.h>

#define SYS_GETCWD 17
#define SYS_WRITE  64
#define SYS_EXIT   93
#define SYS_GETPID 172

static long syscall3(long num, long a0, long a1, long a2) {
    register long ra0 __asm__("a0") = a0;
    register long ra1 __asm__("a1") = a1;
    register long ra2 __asm__("a2") = a2;
    register long ra7 __asm__("a7") = num;
    __asm__ volatile("ecall"
                     : "+r"(ra0)
                     : "r"(ra1), "r"(ra2), "r"(ra7)
                     : "memory");
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

void _start(int argc, char** argv) {
    char cwd[64];
    long pid;

    (void)argc;
    (void)argv;

    u_write("riscv64 user hello\n");

    pid = syscall3(SYS_GETPID, 0, 0, 0);
    u_write("pid: ");
    u_puthex((uint64_t)pid);
    u_write("\n");

    for (int i = 0; i < (int)sizeof(cwd); i++) cwd[i] = '\0';
    if (syscall3(SYS_GETCWD, (long)(uintptr_t)cwd, sizeof(cwd), 0) != 0) {
        u_write("cwd: ");
        u_write(cwd);
        u_write("\n");
    }

    u_write("riscv64 user done\n");
    syscall3(SYS_EXIT, 0, 0, 0);
    for (;;) {
    }
}
