#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/random.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>
#ifdef sa_handler
#undef sa_handler
#endif
#ifdef sa_sigaction
#undef sa_sigaction
#endif
#include "../include/syscall.h"

static long orth_status_ret(int64_t ret);

static int64_t orth_syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    uint64_t ret;
    __asm__ volatile (
        "syscall\n"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );
    return (int64_t)ret;
}

int arch_prctl(int code, unsigned long addr) {
    return (int)orth_status_ret(orth_syscall3(SYS_ARCH_PRCTL, (uint64_t)code, (uint64_t)addr, 0));
}

static int64_t orth_syscall6(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                             uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = arg4;
    register uint64_t r8 __asm__("r8") = arg5;
    register uint64_t r9 __asm__("r9") = arg6;
    __asm__ volatile (
        "syscall\n"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return (int64_t)ret;
}

static long orth_status_ret(int64_t ret) {
    if (ret < 0 && ret >= -4095) {
        errno = (int)-ret;
        return -1;
    }
    return (long)ret;
}

static void *orth_ptr_ret(int64_t ret) {
    if (ret < 0 && ret >= -4095) {
        errno = (int)-ret;
        return MAP_FAILED;
    }
    return (void *)(uintptr_t)ret;
}

struct kstat {
    uint64_t dev;
    uint64_t ino;
    uint64_t nlink;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t pad0;
    uint64_t rdev;
    int64_t size;
    int64_t blksize;
    int64_t blocks;
    int64_t atime_sec;
    int64_t atime_nsec;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    int64_t ctime_sec;
    int64_t ctime_nsec;
    int64_t unused[3];
};

static uint64_t orth_sigset_to_u64(const sigset_t *set) {
    if (!set) return 0;
    return (uint64_t)set->__bits[0];
}

static void orth_sigset_from_u64(sigset_t *set, uint64_t bits) {
    size_t i;
    if (!set) return;
    for (i = 0; i < sizeof(set->__bits) / sizeof(set->__bits[0]); i++) {
        set->__bits[i] = 0;
    }
    set->__bits[0] = (unsigned long)bits;
}

static void stat_from_kstat(struct stat* st, const struct kstat* kst) {
    if (!st || !kst) return;
    *st = (struct stat){0};
    st->st_dev = kst->dev;
    st->st_ino = kst->ino;
    st->st_mode = kst->mode;
    st->st_nlink = kst->nlink;
    st->st_uid = kst->uid;
    st->st_gid = kst->gid;
    st->st_rdev = kst->rdev;
    st->st_size = kst->size;
    st->st_blksize = kst->blksize;
    st->st_blocks = kst->blocks;
    st->st_atim.tv_sec = kst->atime_sec;
    st->st_atim.tv_nsec = kst->atime_nsec;
    st->st_mtim.tv_sec = kst->mtime_sec;
    st->st_mtim.tv_nsec = kst->mtime_nsec;
    st->st_ctim.tv_sec = kst->ctime_sec;
    st->st_ctim.tv_nsec = kst->ctime_nsec;
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz);
ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz);
int faccessat(int dirfd, const char *path, int mode, int flags);

int _fork(void) { return (int)orth_status_ret(orth_syscall3(SYS_FORK, 0, 0, 0)); }
int _execve(const char *pathname, char *const argv[], char *const envp[]) { return (int)orth_status_ret(orth_syscall3(SYS_EXECVE, (uint64_t)pathname, (uint64_t)argv, (uint64_t)envp)); }
int _wait(int *wstatus) { return (int)orth_status_ret(orth_syscall3(SYS_WAIT4, (uint64_t)-1, (uint64_t)wstatus, 0)); }
int _getpid(void) { return (int)orth_syscall3(SYS_GETPID, 0, 0, 0); }
void _exit(int status) { orth_syscall3(SYS_EXIT, (uint64_t)status, 0, 0); for (;;) {} }
ssize_t _write(int fd, const void *buf, size_t count) { return (ssize_t)orth_status_ret(orth_syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, (uint64_t)count)); }
ssize_t _read(int fd, void *buf, size_t count) { return (ssize_t)orth_status_ret(orth_syscall3(SYS_READ, (uint64_t)fd, (uint64_t)buf, (uint64_t)count)); }
int _close(int fd) { return (int)orth_status_ret(orth_syscall3(SYS_CLOSE, (uint64_t)fd, 0, 0)); }

int _open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return (int)orth_status_ret(orth_syscall3(SYS_OPEN, (uint64_t)path, (uint64_t)flags, (uint64_t)mode));
}

int openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return (int)orth_status_ret(orth_syscall6(SYS_OPENAT, (uint64_t)dirfd, (uint64_t)path, (uint64_t)flags, (uint64_t)mode, 0, 0));
}

void *_sbrk(intptr_t increment) {
    uint64_t current_brk = (uint64_t)orth_syscall3(SYS_BRK, 0, 0, 0);
    if (increment == 0) return (void *)current_brk;
    uint64_t new_brk = (uint64_t)orth_syscall3(SYS_BRK, current_brk + increment, 0, 0);
    if (new_brk == current_brk && increment > 0) return (void *)-1;
    return (void *)current_brk;
}

int _fstat(int fd, struct stat *st) {
    struct kstat kst;
    int ret = (int)orth_syscall3(SYS_FSTAT, (uint64_t)fd, (uint64_t)&kst, 0);
    if (ret == 0) {
        stat_from_kstat(st, &kst);
    } else {
        return (int)orth_status_ret(ret);
    }
    return ret;
}

int _stat(const char *path, struct stat *st) {
    struct kstat kst;
    int ret = (int)orth_syscall3(SYS_STAT, (uint64_t)path, (uint64_t)&kst, 0);
    if (ret == 0) {
        stat_from_kstat(st, &kst);
    } else {
        return (int)orth_status_ret(ret);
    }
    return ret;
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    return (ssize_t)orth_status_ret(orth_syscall3(SYS_READLINK, (uint64_t)path, (uint64_t)buf, (uint64_t)bufsiz));
}

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz) {
    return (ssize_t)orth_status_ret(orth_syscall6(SYS_READLINKAT, (uint64_t)dirfd, (uint64_t)path,
                                         (uint64_t)buf, (uint64_t)bufsiz, 0, 0));
}

int faccessat(int dirfd, const char *path, int mode, int flags) {
    return (int)orth_status_ret(orth_syscall6(SYS_FACCESSAT, (uint64_t)dirfd, (uint64_t)path,
                                 (uint64_t)mode, (uint64_t)flags, 0, 0));
}

int utime(const char *path, const struct utimbuf *times) {
    struct stat st;
    (void)times;
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    if (stat(path, &st) < 0) {
        if (errno == 0) errno = ENOENT;
        return -1;
    }
    return 0;
}

int utimes(const char *path, const struct timeval times[2]) {
    struct stat st;
    (void)times;
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    if (stat(path, &st) < 0) {
        if (errno == 0) errno = ENOENT;
        return -1;
    }
    return 0;
}

int lutimes(const char *path, const struct timeval times[2]) {
    return utimes(path, times);
}

int _isatty(int fd) {
    struct stat st;
    if (_fstat(fd, &st) == 0) return ((st.st_mode & 0170000) == 0020000) ? 1 : 0;
    return 0;
}

off_t _lseek(int fd, off_t ptr, int dir) { return (off_t)orth_status_ret(orth_syscall3(SYS_LSEEK, (uint64_t)fd, (uint64_t)ptr, (uint64_t)dir)); }
int _unlink(const char *path) { return (int)orth_status_ret(orth_syscall3(SYS_UNLINK, (uint64_t)path, 0, 0)); }
int _rename(const char *oldpath, const char *newpath) { return (int)orth_status_ret(orth_syscall3(SYS_RENAME, (uint64_t)oldpath, (uint64_t)newpath, 0)); }
int _chmod(const char *path, mode_t mode) { return (int)orth_status_ret(orth_syscall3(SYS_CHMOD, (uint64_t)path, (uint64_t)mode, 0)); }
int _pipe(int pipefd[2]) { return (int)orth_status_ret(orth_syscall3(SYS_PIPE, (uint64_t)pipefd, 0, 0)); }
int pipe2(int pipefd[2], int flags) { return (int)orth_status_ret(orth_syscall3(SYS_PIPE2, (uint64_t)pipefd, (uint64_t)flags, 0)); }
int _dup2(int oldfd, int newfd) { return (int)orth_status_ret(orth_syscall3(SYS_DUP2, (uint64_t)oldfd, (uint64_t)newfd, 0)); }
int _kill(int pid, int sig) { return (int)orth_status_ret(orth_syscall3(SYS_KILL, (uint64_t)pid, (uint64_t)sig, 0)); }
int gettimeofday(struct timeval *tv, void *tz) {
    return (int)orth_status_ret(orth_syscall3(SYS_GETTIMEOFDAY, (uint64_t)tv, (uint64_t)tz, 0));
}
int clock_gettime(clockid_t clock_id, struct timespec *tp) {
    return (int)orth_status_ret(orth_syscall3(SYS_CLOCK_GETTIME, (uint64_t)clock_id, (uint64_t)tp, 0));
}
ssize_t getrandom(void *buf, size_t buflen, unsigned flags) {
    return (ssize_t)orth_status_ret(orth_syscall3(SYS_GETRANDOM, (uint64_t)buf, (uint64_t)buflen, (uint64_t)flags));
}
int socket(int domain, int type, int protocol) {
    return (int)orth_status_ret(orth_syscall3(SYS_SOCKET, (uint64_t)domain, (uint64_t)type, (uint64_t)protocol));
}
int connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    return (int)orth_status_ret(orth_syscall3(SYS_CONNECT, (uint64_t)fd, (uint64_t)addr, (uint64_t)addrlen));
}
int accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    return (int)orth_status_ret(orth_syscall3(SYS_ACCEPT, (uint64_t)fd, (uint64_t)addr, (uint64_t)addrlen));
}
int bind(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    return (int)orth_status_ret(orth_syscall3(SYS_BIND, (uint64_t)fd, (uint64_t)addr, (uint64_t)addrlen));
}
int listen(int fd, int backlog) {
    return (int)orth_status_ret(orth_syscall3(SYS_LISTEN, (uint64_t)fd, (uint64_t)backlog, 0));
}
int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    return (int)orth_status_ret(orth_syscall3(SYS_GETSOCKNAME, (uint64_t)fd, (uint64_t)addr, (uint64_t)addrlen));
}
int getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    return (int)orth_status_ret(orth_syscall3(SYS_GETPEERNAME, (uint64_t)fd, (uint64_t)addr, (uint64_t)addrlen));
}
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen) {
    return (int)orth_status_ret(orth_syscall6(SYS_SETSOCKOPT, (uint64_t)fd, (uint64_t)level, (uint64_t)optname,
                                 (uint64_t)optval, (uint64_t)optlen, 0));
}
int shutdown(int fd, int how) {
    return (int)orth_status_ret(orth_syscall3(SYS_SHUTDOWN, (uint64_t)fd, (uint64_t)how, 0));
}
ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen) {
    return (ssize_t)orth_status_ret(orth_syscall6(SYS_SENDTO, (uint64_t)fd, (uint64_t)buf, (uint64_t)len,
                                         (uint64_t)flags, (uint64_t)dest_addr, (uint64_t)addrlen));
}
ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen) {
    return (ssize_t)orth_status_ret(orth_syscall6(SYS_RECVFROM, (uint64_t)fd, (uint64_t)buf, (uint64_t)len,
                                         (uint64_t)flags, (uint64_t)src_addr, (uint64_t)addrlen));
}

int chdir_sys(const char *path) { return (int)orth_status_ret(orth_syscall3(SYS_CHDIR, (uint64_t)path, 0, 0)); }
int fchdir_sys(int fd) { return (int)orth_status_ret(orth_syscall3(SYS_FCHDIR, (uint64_t)fd, 0, 0)); }
int mkdir_sys(const char *path, int mode) { return (int)orth_status_ret(orth_syscall3(SYS_MKDIR, (uint64_t)path, (uint64_t)mode, 0)); }
int mkdirat(int dirfd, const char *path, mode_t mode) { return (int)orth_status_ret(orth_syscall3(SYS_MKDIRAT, (uint64_t)dirfd, (uint64_t)path, (uint64_t)mode)); }
int rmdir_sys(const char *path) { return (int)orth_status_ret(orth_syscall3(SYS_RMDIR, (uint64_t)path, 0, 0)); }
int unlinkat(int dirfd, const char *path, int flags) { return (int)orth_status_ret(orth_syscall3(SYS_UNLINKAT, (uint64_t)dirfd, (uint64_t)path, (uint64_t)flags)); }
int fstatat(int dirfd, const char *path, struct stat *st, int flags) {
    struct kstat kst;
    int ret = (int)orth_syscall6(SYS_FSTATAT, (uint64_t)dirfd, (uint64_t)path, (uint64_t)&kst, (uint64_t)flags, 0, 0);
    if (ret == 0) {
        stat_from_kstat(st, &kst);
    } else {
        return (int)orth_status_ret(ret);
    }
    return ret;
}
int getcwd_sys(char *buf, uint32_t size) { return (int)orth_status_ret(orth_syscall3(SYS_GETCWD, (uint64_t)buf, (uint64_t)size, 0)); }

int execve(const char *path, char *const argv[], char *const envp[]) { return _execve(path, argv, envp); }
ssize_t write(int fd, const void *buf, size_t count) { return _write(fd, buf, count); }
ssize_t read(int fd, void *buf, size_t count) { return _read(fd, buf, count); }
ssize_t writev(int fd, const struct iovec *iov, int iovcnt) { return (ssize_t)orth_status_ret(orth_syscall3(SYS_WRITEV, (uint64_t)fd, (uint64_t)iov, (uint64_t)iovcnt)); }
ssize_t readv(int fd, const struct iovec *iov, int iovcnt) { return (ssize_t)orth_status_ret(orth_syscall3(SYS_READV, (uint64_t)fd, (uint64_t)iov, (uint64_t)iovcnt)); }
int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return (int)orth_status_ret(orth_syscall3(SYS_OPEN, (uint64_t)path, (uint64_t)flags, (uint64_t)mode));
}
int close(int fd) { return _close(fd); }
void *sbrk(intptr_t inc) { return _sbrk(inc); }
pid_t getpid(void) { return _getpid(); }
int waitpid(pid_t pid, int *wstatus, int options) { return (int)orth_status_ret(orth_syscall3(SYS_WAIT4, (uint64_t)pid, (uint64_t)wstatus, (uint64_t)options)); }
int fstat(int fd, struct stat *st) { return _fstat(fd, st); }
int stat(const char *path, struct stat *st) { return _stat(path, st); }
int lstat(const char *path, struct stat *st) { return _stat(path, st); }
int access(const char *path, int mode) { return faccessat(-100, path, mode, 0); }
int isatty(int fd) { return _isatty(fd); }
off_t lseek(int fd, off_t ptr, int dir) { return _lseek(fd, ptr, dir); }
int unlink(const char *path) { return _unlink(path); }
int rename(const char *oldpath, const char *newpath) { return _rename(oldpath, newpath); }
int chmod(const char *path, mode_t mode) { return _chmod(path, mode); }
int pipe(int pipefd[2]) { return _pipe(pipefd); }
int dup2(int oldfd, int newfd) { return _dup2(oldfd, newfd); }
int dup(int fd) { return (int)orth_status_ret(orth_syscall3(SYS_FCNTL, (uint64_t)fd, (uint64_t)F_DUPFD, 0)); }
int kill(pid_t pid, int sig) { return _kill(pid, sig); }
pid_t wait(int *wstatus) { return _wait(wstatus); }

int chdir(const char *path) { return chdir_sys(path); }
int fchdir(int fd) { return fchdir_sys(fd); }
char *getcwd(char *buf, size_t size) {
    if (!buf || size == 0) {
        errno = EINVAL;
        return 0;
    }
    if (getcwd_sys(buf, (uint32_t)size) < 0) {
        errno = ERANGE;
        return 0;
    }
    return buf;
}

int getpagesize(void) { return 4096; }
long sysconf(int name) { (void)name; return 4096; }

int tcgetattr(int fd, struct termios *tio) { return (int)orth_status_ret(orth_syscall3(ORTH_SYS_TCGETATTR, (uint64_t)fd, (uint64_t)tio, 0)); }
int tcsetattr(int fd, int optional_actions, const struct termios *tio) { return (int)orth_status_ret(orth_syscall3(ORTH_SYS_TCSETATTR, (uint64_t)fd, (uint64_t)optional_actions, (uint64_t)tio)); }
pid_t getpgrp(void) { return (pid_t)orth_status_ret(orth_syscall3(SYS_GETPGRP, 0, 0, 0)); }
int setpgid(pid_t pid, pid_t pgid) { return (int)orth_status_ret(orth_syscall3(SYS_SETPGID, (uint64_t)pid, (uint64_t)pgid, 0)); }
pid_t setsid(void) { return (pid_t)orth_status_ret(orth_syscall3(SYS_SETSID, 0, 0, 0)); }
pid_t tcgetpgrp(int fd) { return (pid_t)orth_status_ret(orth_syscall3(ORTH_SYS_TCGETPGRP, (uint64_t)fd, 0, 0)); }
int tcsetpgrp(int fd, pid_t pgrp_id) { return (int)orth_status_ret(orth_syscall3(ORTH_SYS_TCSETPGRP, (uint64_t)fd, (uint64_t)pgrp_id, 0)); }

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    struct orth_sigaction in_act;
    struct orth_sigaction out_act;
    struct orth_sigaction *in_ptr = 0;
    struct orth_sigaction *out_ptr = 0;
    int ret;
    if (act) {
        in_act.sa_handler = (uint64_t)(uintptr_t)act->__sa_handler.sa_handler;
        in_act.sa_mask = orth_sigset_to_u64(&act->sa_mask);
        in_act.sa_flags = (uint32_t)act->sa_flags;
        in_act.reserved = 0;
        in_ptr = &in_act;
    }
    if (oldact) out_ptr = &out_act;
    ret = (int)orth_status_ret(orth_syscall3(ORTH_SYS_SIGACTION, (uint64_t)sig, (uint64_t)in_ptr, (uint64_t)out_ptr));
    if (ret == 0 && oldact) {
        oldact->__sa_handler.sa_handler = (void (*)(int))(uintptr_t)out_act.sa_handler;
        orth_sigset_from_u64(&oldact->sa_mask, out_act.sa_mask);
        oldact->sa_flags = (int)out_act.sa_flags;
    }
    return ret;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    uint64_t in_bits = orth_sigset_to_u64(set);
    uint64_t out_bits = 0;
    int ret = (int)orth_status_ret(orth_syscall3(ORTH_SYS_SIGPROCMASK, (uint64_t)how, set ? (uint64_t)&in_bits : 0, oldset ? (uint64_t)&out_bits : 0));
    if (ret == 0 && oldset) orth_sigset_from_u64(oldset, out_bits);
    return ret;
}

int sigpending(sigset_t *set) {
    uint64_t bits = 0;
    int ret = (int)orth_status_ret(orth_syscall3(ORTH_SYS_SIGPENDING, (uint64_t)&bits, 0, 0));
    if (ret == 0 && set) orth_sigset_from_u64(set, bits);
    return ret;
}

void (*__wrap_signal(int sig, void (*handler)(int)))(int) {
    struct sigaction act;
    struct sigaction oldact;
    act.__sa_handler.sa_handler = handler;
    orth_sigset_from_u64(&act.sa_mask, 0);
    act.sa_flags = 0;
    if (sigaction(sig, &act, &oldact) < 0) {
        return SIG_ERR;
    }
    return oldact.__sa_handler.sa_handler;
}

int ioctl(int fd, int request, ...) {
    va_list ap;
    void *arg = 0;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);
    switch (request) {
        case TIOCGWINSZ:
            if (arg) {
                struct winsize *ws = (struct winsize *)arg;
                ws->ws_row = 25;
                ws->ws_col = 80;
                ws->ws_xpixel = 0;
                ws->ws_ypixel = 0;
                return 0;
            }
            errno = EINVAL;
            return -1;
        case TIOCGPGRP:
            if (arg) {
                *(pid_t *)arg = tcgetpgrp(fd);
                return 0;
            }
            errno = EINVAL;
            return -1;
        case TIOCSPGRP:
            if (arg) return tcsetpgrp(fd, *(pid_t *)arg);
            errno = EINVAL;
            return -1;
        default:
            errno = ENOTTY;
            return -1;
    }
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    return orth_ptr_ret(orth_syscall6(SYS_MMAP, (uint64_t)addr, (uint64_t)length, (uint64_t)prot,
                                 (uint64_t)flags, (uint64_t)fd, (uint64_t)offset));
}
int munmap(void *addr, size_t length) { return (int)orth_status_ret(orth_syscall3(SYS_MUNMAP, (uint64_t)addr, (uint64_t)length, 0)); }
int mprotect(void *addr, size_t length, int prot) { return (int)orth_status_ret(orth_syscall3(SYS_MPROTECT, (uint64_t)addr, (uint64_t)length, (uint64_t)prot)); }
void *mremap(void *old_addr, size_t old_len, size_t new_len, int flags, ...) {
    va_list ap;
    void *new_addr = 0;
    if (flags & MREMAP_FIXED) {
        va_start(ap, flags);
        new_addr = va_arg(ap, void *);
        va_end(ap);
    }
    return orth_ptr_ret(orth_syscall6(SYS_MREMAP, (uint64_t)old_addr, (uint64_t)old_len, (uint64_t)new_len,
                                      (uint64_t)flags, (uint64_t)new_addr, 0));
}
int ftruncate(int fd, off_t length) { return (int)orth_status_ret(orth_syscall3(SYS_FTRUNCATE, (uint64_t)fd, (uint64_t)length, 0)); }
int truncate(const char *path, off_t length) { return (int)orth_status_ret(orth_syscall3(SYS_TRUNCATE, (uint64_t)path, (uint64_t)length, 0)); }

int get_video_info(struct video_info *info) { return (int)orth_status_ret(orth_syscall3(ORTH_SYS_GET_VIDEO_INFO, (uint64_t)info, 0, 0)); }
uint64_t map_framebuffer(void) { return (uint64_t)orth_syscall3(ORTH_SYS_MAP_FRAMEBUFFER, 0, 0, 0); }
uint64_t get_ticks_ms(void) { return (uint64_t)orth_syscall3(ORTH_SYS_GET_TICKS_MS, 0, 0, 0); }
int sleep_ms(uint64_t ms) { return (int)orth_status_ret(orth_syscall3(ORTH_SYS_SLEEP_MS, ms, 0, 0)); }
int get_key_event(struct key_event *ev) { return (int)orth_status_ret(orth_syscall3(ORTH_SYS_GET_KEY_EVENT, (uint64_t)ev, 0, 0)); }
int sound_on(uint32_t freq_hz) { return (int)orth_status_ret(orth_syscall3(ORTH_SYS_SOUND_ON, (uint64_t)freq_hz, 0, 0)); }
int sound_off(void) { return (int)orth_status_ret(orth_syscall3(ORTH_SYS_SOUND_OFF, 0, 0, 0)); }
int sound_pcm_u8(const uint8_t *samples, uint32_t count, uint32_t sample_rate) { return (int)orth_status_ret(orth_syscall3(ORTH_SYS_SOUND_PCM_U8, (uint64_t)samples, (uint64_t)count, (uint64_t)sample_rate)); }
int usb_info(void) { return (int)orth_status_ret(orth_syscall3(ORTH_SYS_USB_INFO, 0, 0, 0)); }
int usb_read_block_sys(uint32_t lba, void *buf, uint32_t count) { return (int)orth_status_ret(orth_syscall3(ORTH_SYS_USB_READ_BLOCK, (uint64_t)lba, (uint64_t)buf, (uint64_t)count)); }
int mount_usb_root(const char *path) { return (int)orth_status_ret(orth_syscall3(ORTH_SYS_MOUNT_USB_ROOT, (uint64_t)path, 0, 0)); }
int mount_module_root(void) { return (int)orth_status_ret(orth_syscall3(ORTH_SYS_MOUNT_MODULE_ROOT, 0, 0, 0)); }
int get_mount_status(char *buf, uint32_t size) { return (int)orth_status_ret(orth_syscall3(ORTH_SYS_GET_MOUNT_STATUS, (uint64_t)buf, (uint64_t)size, 0)); }
int dns_lookup_ipv4(const char *hostname, uint32_t *out_addr) { return (int)orth_status_ret(orth_syscall3(ORTH_SYS_DNS_LOOKUP, (uint64_t)hostname, (uint64_t)out_addr, 0)); }
int get_cpu_id(void) { return (int)orth_syscall3(ORTH_SYS_GET_CPU_ID, 0, 0, 0); }
int set_fork_spread(int enabled) { return (int)orth_status_ret(orth_syscall3(ORTH_SYS_SET_FORK_SPREAD, (uint64_t)enabled, 0, 0)); }
int get_fork_spread(void) { return (int)orth_syscall3(ORTH_SYS_GET_FORK_SPREAD, 0, 0, 0); }
int get_runq_stats(struct orth_runq_stat *out, uint32_t max_count) { return (int)orth_status_ret(orth_syscall3(ORTH_SYS_GET_RUNQ_STATS, (uint64_t)out, (uint64_t)max_count, 0)); }
int getdents_sys(int fd, struct orth_dirent *dirp, uint32_t count) { return (int)orth_status_ret(orth_syscall3(SYS_GETDENTS, (uint64_t)fd, (uint64_t)dirp, (uint64_t)count)); }
