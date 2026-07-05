#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define WEAK_SYM __attribute__((weak))

WEAK_SYM long sysconf(int name) { 
    // _SC_PAGESIZE is typically 30 or similar, but just return 4096 for anything resembling a page size request.
    return 4096; 
}

WEAK_SYM int getpagesize(void) {
    return 4096;
}

WEAK_SYM mode_t umask(mode_t mask) { return 0; }
WEAK_SYM int chmod(const char *path, mode_t mode) { return 0; }
WEAK_SYM int fcntl(int fd, int cmd, ...) { return 0; }
WEAK_SYM char *getcwd(char *buf, size_t size) { 
    if (buf && size > 1) {
        buf[0] = '/';
        buf[1] = '\0';
        return buf;
    }
    return 0; 
}
WEAK_SYM int utime(const char *filename, const void *times) { return 0; }
WEAK_SYM int chown(const char *path, uid_t owner, gid_t group) { return 0; }
WEAK_SYM int link(const char *oldpath, const char *newpath) { 
    errno = EMLINK;
    return -1; 
}
WEAK_SYM int rmdir(const char *pathname) { 
    errno = ENOTEMPTY;
    return -1; 
}

// Memory mapping stubs
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif

static void print_hex_stub(const char *prefix, size_t val) {
    write(1, prefix, strlen(prefix));
    char buf[20];
    buf[0] = '0'; buf[1] = 'x';
    int i = 15;
    buf[16] = '\n';
    buf[17] = '\0';
    size_t tmp = val;
    for (; i >= 2; i--) {
        int nibble = tmp & 0xF;
        buf[i] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
        tmp >>= 4;
    }
    write(1, buf, 17);
}

extern void *memalign(size_t alignment, size_t size);

WEAK_SYM void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    print_hex_stub("[user_stub] mmap requested length: ", length);
    void *p = memalign(4096, length);
    if (p) {
        memset(p, 0, length);
        return p;
    }

    const char *msg = "[user_stub] mmap malloc failed!\n";
    write(1, msg, strlen(msg));

    errno = ENOMEM;
    return (void *)-1;
}
WEAK_SYM int munmap(void *addr, size_t length) {
    if (addr && addr != (void*)-1) {
        free(addr);
    }
    return 0;
}

WEAK_SYM int msync(void *addr, size_t length, int flags) {
    return 0;
}

// Signal stubs
WEAK_SYM int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    return 0;
}

WEAK_SYM int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    return 0;
}

// Process stubs

WEAK_SYM int mkdir(const char *pathname, mode_t mode) {
    return 0;
}

WEAK_SYM unsigned int sleep(unsigned int seconds) {
    return 0;
}

WEAK_SYM unsigned int alarm(unsigned int seconds) {
    return 0;
}

WEAK_SYM int execvp(const char *file, char *const argv[]) {
    errno = ENOSYS;
    return -1;
}

WEAK_SYM int execv(const char *path, char *const argv[]) {
    errno = ENOSYS;
    return -1;
}

WEAK_SYM int chdir(const char *path) {
    return 0;
}

struct timeval;
WEAK_SYM int gettimeofday(struct timeval *tv, void *tz) {
    if (tv) {
        memset(tv, 0, sizeof(*tv));
    }
    return 0;
}

WEAK_SYM char *getwd(char *buf) {
    if (!buf) return NULL;
    buf[0] = '/';
    buf[1] = '\0';
    return buf;
}
