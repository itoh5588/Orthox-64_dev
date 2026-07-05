#include <stddef.h>
#include <stdint.h>
#include "syscall.h"

#define O_RDONLY 0
#define O_DIRECTORY 00200000

static long riscv64_syscall6(long n, long a0, long a1, long a2, long a3, long a4, long a5) {
    register long x10 __asm__("a0") = a0;
    register long x11 __asm__("a1") = a1;
    register long x12 __asm__("a2") = a2;
    register long x13 __asm__("a3") = a3;
    register long x14 __asm__("a4") = a4;
    register long x15 __asm__("a5") = a5;
    register long x17 __asm__("a7") = n;
    __asm__ volatile("ecall"
                     : "+r"(x10)
                     : "r"(x11), "r"(x12), "r"(x13), "r"(x14), "r"(x15), "r"(x17)
                     : "memory");
    return x10;
}

static long riscv64_syscall4(long n, long a0, long a1, long a2, long a3) {
    return riscv64_syscall6(n, a0, a1, a2, a3, 0, 0);
}

static long riscv64_syscall3(long n, long a0, long a1, long a2) {
    return riscv64_syscall6(n, a0, a1, a2, 0, 0, 0);
}

static long riscv64_syscall2(long n, long a0, long a1) {
    return riscv64_syscall6(n, a0, a1, 0, 0, 0, 0);
}

static long riscv64_syscall1(long n, long a0) {
    return riscv64_syscall6(n, a0, 0, 0, 0, 0, 0);
}

static size_t riscv64_strlen(const char* s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static void riscv64_write_str(int fd, const char* s) {
    (void)riscv64_syscall3(SYS_WRITE, fd, (long)(uintptr_t)s, (long)riscv64_strlen(s));
}

static void riscv64_fail(const char* why) {
    riscv64_write_str(1, why);
    (void)riscv64_syscall1(SYS_EXIT, 1);
    for (;;) {}
}

static void* riscv64_memset(void* dst, int value, size_t len) {
    unsigned char* out = (unsigned char*)dst;
    for (size_t i = 0; i < len; i++) out[i] = (unsigned char)value;
    return dst;
}

static int riscv64_run(void) {
    static const char root_path[] = "/";
    static const char bootstrap_rel[] = "bootstrap-user";
    static const char pwd_prefix[] = "PWD:";
    static const char cwd2_msg[] = "CWD2\n";
    static const char pwd_nl[] = "\n";
    static const char stat_msg[] = "STAT\n";
    static const char elf_prefix[] = "ELF:";
    static const char ok_msg[] = "FOK\n";
    static const char fork_msg[] = "FRK\n";
    static const char mmap_msg[] = "MAP\n";
    static const char mmap_ok_msg[] = "MMOK\n";
    static const char wait_ok_msg[] = "WAIT\n";
    char cwd[64];
    unsigned char stat_buf[256];
    unsigned char page[4096];
    char* map;
    long dirfd;
    long fd;
    long rc;
    long pid;
    int wstatus = 0;

    rc = riscv64_syscall2(SYS_GETCWD, (long)(uintptr_t)cwd, sizeof(cwd));
    if (rc <= 0) riscv64_fail("FER getcwd\n");
    riscv64_write_str(1, pwd_prefix);
    riscv64_write_str(1, cwd);
    riscv64_write_str(1, pwd_nl);

    dirfd = riscv64_syscall3(SYS_OPEN, (long)(uintptr_t)root_path, O_RDONLY | O_DIRECTORY, 0);
    if (dirfd < 0) riscv64_fail("FER open /\n");

    rc = riscv64_syscall1(SYS_FCHDIR, dirfd);
    if (rc < 0) riscv64_fail("FER fchdir\n");
    rc = riscv64_syscall2(SYS_GETCWD, (long)(uintptr_t)cwd, sizeof(cwd));
    if (rc <= 0 || cwd[0] != '/' || cwd[1] != '\0') riscv64_fail("FER getcwd2\n");
    riscv64_write_str(1, cwd2_msg);

    fd = riscv64_syscall4(SYS_OPENAT, dirfd, (long)(uintptr_t)bootstrap_rel, O_RDONLY, 0);
    if (fd < 0) riscv64_fail("FER openat\n");

    riscv64_memset(stat_buf, 0, sizeof(stat_buf));
    rc = riscv64_syscall2(SYS_STAT, (long)(uintptr_t)root_path, (long)(uintptr_t)stat_buf);
    if (rc < 0) riscv64_fail("FER stat /\n");
    riscv64_memset(stat_buf, 0, sizeof(stat_buf));
    rc = riscv64_syscall4(SYS_FSTATAT, dirfd, (long)(uintptr_t)bootstrap_rel, (long)(uintptr_t)stat_buf, 0);
    if (rc < 0) riscv64_fail("FER fstatat\n");
    rc = riscv64_syscall2(SYS_FSTAT, fd, (long)(uintptr_t)stat_buf);
    if (rc < 0) riscv64_fail("FER fstat\n");
    riscv64_write_str(1, stat_msg);

    riscv64_memset(page, 0, sizeof(page));
    rc = riscv64_syscall3(SYS_READ, fd, (long)(uintptr_t)page, 4);
    if (rc != 4) riscv64_fail("FER read\n");
    if (page[0] != 0x7f || page[1] != 'E' || page[2] != 'L' || page[3] != 'F') {
        riscv64_fail("FER magic\n");
    }
    riscv64_write_str(1, elf_prefix);
    (void)riscv64_syscall3(SYS_WRITE, 1, (long)(uintptr_t)(page + 1), 3);
    riscv64_write_str(1, pwd_nl);

    map = (char*)(uintptr_t)riscv64_syscall6(SYS_MMAP,
                                             0,
                                             4096,
                                             PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS,
                                             -1,
                                             0);
    if ((long)(uintptr_t)map < 0) riscv64_fail("FER mmap\n");
    map[0] = 'O';
    map[1] = 'K';
    map[2] = '\0';
    riscv64_write_str(1, mmap_msg);
    if (map[0] != 'O' || map[1] != 'K' || map[2] != '\0') riscv64_fail("FER mmapchk\n");
    riscv64_write_str(1, mmap_ok_msg);

    pid = riscv64_syscall1(SYS_FORK, 0);
    if (pid < 0) riscv64_fail("FER fork\n");
    if (pid == 0) {
        riscv64_write_str(1, fork_msg);
        (void)riscv64_syscall1(SYS_EXIT, 0);
        for (;;) {}
    }

    rc = riscv64_syscall3(SYS_WAIT4, pid, (long)(uintptr_t)&wstatus, 0);
    if (rc != pid || wstatus != 0) riscv64_fail("FER wait4\n");
    riscv64_write_str(1, wait_ok_msg);

    (void)riscv64_syscall1(SYS_CLOSE, fd);
    (void)riscv64_syscall1(SYS_CLOSE, dirfd);
    riscv64_write_str(1, ok_msg);
    return 0;
}

void _start(void) {
    int rc = riscv64_run();
    (void)riscv64_syscall1(SYS_EXIT, rc);
    for (;;) {}
}
