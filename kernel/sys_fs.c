#include <stddef.h>
#include <stdint.h>
#include "fs.h"
#include "lapic.h"
#include "sys_internal.h"
#include "task.h"

#define ORTH_TCGETS      0x5401
#define ORTH_TCSETS      0x5402
#define ORTH_TIOCGPGRP   0x540F
#define ORTH_TIOCSPGRP   0x5410
#define ORTH_TIOCGWINSZ  0x5413
#define ORTH_FIONCLEX    0x5450
#define ORTH_FIOCLEX     0x5451

#define ORTH_F_SETFD     2

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EFAULT
#define EFAULT 14
#endif
#ifndef ENOTTY
#define ENOTTY 25
#endif

#ifndef ORTHOX_MEM_PROGRESS
#define ORTHOX_MEM_PROGRESS 0
#endif

static struct orth_termios g_console_termios = {
    .c_iflag = 0x00000002u,
    .c_oflag = 0x00000001u,
    .c_cflag = 0,
    .c_lflag = 0x00000001u | 0x00000002u | 0x00000008u,
    .c_cc = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26 },
    .c_ispeed = 115200,
    .c_ospeed = 115200,
};

int sys_open(const char* path, int flags, int mode) {
    return fs_open(path, flags, mode);
}

int sys_openat(int dirfd, const char* path, int flags, int mode) {
    return fs_openat(dirfd, path, flags, mode);
}

int64_t sys_read(int fd, void* buf, size_t count) {
    return fs_read(fd, buf, count);
}

int64_t sys_write(int fd, const void* buf, size_t count) {
    return fs_write(fd, buf, count);
}

int sys_close(int fd) {
    return fs_close(fd);
}

int sys_fcntl(int fd, int cmd, uint64_t arg) {
    return fs_fcntl(fd, cmd, arg);
}

int sys_pipe(int pipefd[2]) {
    return fs_pipe(pipefd);
}

int sys_pipe2(int pipefd[2], int flags) {
    return fs_pipe2(pipefd, flags);
}

int sys_dup2(int oldfd, int newfd) {
    return fs_dup2(oldfd, newfd);
}

int sys_fstat(int fd, struct kstat* st) {
    return fs_fstat(fd, st);
}

int sys_stat(const char* path, struct kstat* st) {
    return fs_stat(path, st);
}

int sys_fstatat(int dirfd, const char* path, struct kstat* st, int flags) {
    return fs_fstatat(dirfd, path, st, flags);
}

int sys_access(const char* path, int mode) {
    return fs_access(path, mode);
}

int sys_faccessat(int dirfd, const char* path, int mode, int flags) {
    return fs_faccessat(dirfd, path, mode, flags);
}

int64_t sys_readlink(const char* path, char* buf, size_t bufsiz) {
    return fs_readlink(path, buf, bufsiz);
}

int64_t sys_readlinkat(int dirfd, const char* path, char* buf, size_t bufsiz) {
    return fs_readlinkat(dirfd, path, buf, bufsiz);
}

int64_t sys_lseek(int fd, int64_t offset, int whence) {
    return fs_lseek(fd, offset, whence);
}

int sys_getdents(int fd, struct orth_dirent* dirp, size_t count) {
    return fs_getdents(fd, dirp, count);
}

int sys_getdents64(int fd, void* dirp, size_t count) {
    return fs_getdents64(fd, dirp, count);
}

int sys_chdir(const char* path) {
    return fs_chdir(path);
}

int sys_fchdir(int fd) {
    return fs_fchdir(fd);
}

int sys_getcwd(char* buf, size_t size) {
    return fs_getcwd(buf, size);
}

int sys_truncate(const char* path, uint64_t length) {
    return fs_truncate(path, length);
}

int sys_ftruncate(int fd, uint64_t length) {
    return fs_ftruncate(fd, length);
}

int sys_utimensat(int dirfd, const char* path, const void* times, int flags) {
    return fs_utimensat(dirfd, path, times, flags);
}

int sys_sync(void) {
    return fs_sync();
}

int sys_unlink(const char* path) {
    return fs_unlink(path);
}

int sys_unlinkat(int dirfd, const char* path, int flags) {
    return fs_unlinkat(dirfd, path, flags);
}

int sys_rename(const char* oldpath, const char* newpath) {
    return fs_rename(oldpath, newpath);
}

int sys_chmod(const char* path, uint32_t mode) {
    return fs_chmod(path, mode);
}

int sys_mkdir(const char* path, int mode) {
    return fs_mkdir(path, mode);
}

int sys_mknod(const char* path, uint32_t mode, uint64_t dev) {
    return fs_mknod(path, mode, dev);
}

int sys_mkdirat(int dirfd, const char* path, int mode) {
    return fs_mkdirat(dirfd, path, mode);
}

int sys_rmdir(const char* path) {
    return fs_rmdir(path);
}

int64_t sys_pread64(int fd, void* buf, size_t count, int64_t offset) {
    int64_t old_offset;
    int64_t ret;
    if (offset < 0) return -EINVAL;
    old_offset = fs_lseek(fd, 0, 1);
    if (old_offset < 0) return old_offset;
    if (fs_lseek(fd, offset, 0) < 0) {
        fs_lseek(fd, old_offset, 0);
        return -1;
    }
    ret = fs_read(fd, buf, count);
    fs_lseek(fd, old_offset, 0);
    return ret;
}

int64_t sys_pwrite64(int fd, const void* buf, size_t count, int64_t offset) {
    int64_t old_offset;
    int64_t ret;
    if (offset < 0) return -EINVAL;
    old_offset = fs_lseek(fd, 0, 1);
    if (old_offset < 0) return old_offset;
    if (fs_lseek(fd, offset, 0) < 0) {
        fs_lseek(fd, old_offset, 0);
        return -1;
    }
    ret = fs_write(fd, buf, count);
    fs_lseek(fd, old_offset, 0);
    return ret;
}

static int fd_is_console(int fd) {
    struct task* current = get_current_task();
    file_type_t type;
    if (!current) return 0;
    if (fd < 0 || fd >= MAX_FDS) return 0;
    if (!current->fds[fd].in_use) return 0;
    type = fs_fd_type(&current->fds[fd]);
    if (type == FT_CONSOLE) return 1;
    /* /dev/tty and /dev/console (major 5) act as the console tty;
     * other character devices such as /dev/null must stay ENOTTY. */
    return type == FT_CHARDEV && fs_fd_aux0(&current->fds[fd]) == 5U;
}

int sys_tcgetattr(int fd, struct orth_termios* tio) {
    if (!tio) return -EFAULT;
    if (!fd_is_console(fd)) return -ENOTTY;
    *tio = g_console_termios;
    return 0;
}

int sys_tcsetattr(int fd, int optional_actions, const struct orth_termios* tio) {
    (void)optional_actions;
    if (!tio) return -EFAULT;
    if (!fd_is_console(fd)) return -ENOTTY;
    g_console_termios = *tio;
    return 0;
}

int sys_ioctl(int fd, unsigned long request, uint64_t arg) {
    switch (request) {
        case ORTH_TIOCGWINSZ:
            if (!arg) return -EFAULT;
            if (!fd_is_console(fd)) return -ENOTTY;
            ((struct orth_winsize*)arg)->ws_row = 25;
            ((struct orth_winsize*)arg)->ws_col = 80;
            ((struct orth_winsize*)arg)->ws_xpixel = 0;
            ((struct orth_winsize*)arg)->ws_ypixel = 0;
            return 0;
        case ORTH_TIOCGPGRP:
            if (!arg) return -1;
            *(int*)arg = sys_tcgetpgrp(fd);
            return 0;
        case ORTH_TIOCSPGRP:
            if (!arg) return -1;
            return sys_tcsetpgrp(fd, *(const int*)arg);
        case ORTH_TCGETS:
            return sys_tcgetattr(fd, (struct orth_termios*)arg);
        case ORTH_TCSETS:
            return sys_tcsetattr(fd, 0, (const struct orth_termios*)arg);
        case ORTH_FIOCLEX:
            return fs_fcntl(fd, ORTH_F_SETFD, FD_CLOEXEC);
        case ORTH_FIONCLEX:
            return fs_fcntl(fd, ORTH_F_SETFD, 0);
        default:
            return -ENOTTY;
    }
}

int sys_lstat(const char* path, struct kstat* st) {
    return fs_stat(path, st);
}

int64_t sys_writev(int fd, const struct orth_iovec* iov, int iovcnt) {
    int64_t total = 0;
    if (!iov || iovcnt < 0) return -1;
    for (int i = 0; i < iovcnt; i++) {
        int64_t rc = fs_write(fd, iov[i].iov_base, iov[i].iov_len);
        if (rc < 0) return (total > 0) ? total : rc;
        total += rc;
        if ((size_t)rc != iov[i].iov_len) break;
    }
#if ORTHOX_MEM_PROGRESS
    {
        struct task* current = get_current_task();
        if (current && current->trace_progress && total > 0) {
            current->trace_write_bytes += (uint64_t)total;
            if ((uint64_t)total > current->trace_write_max) {
                current->trace_write_max = (uint64_t)total;
            }
        }
    }
#endif
    return total;
}

int64_t sys_readv(int fd, const struct orth_iovec* iov, int iovcnt) {
    int64_t total = 0;
    if (!iov || iovcnt < 0) return -1;
    for (int i = 0; i < iovcnt; i++) {
        int64_t rc = fs_read(fd, (void*)iov[i].iov_base, iov[i].iov_len);
        if (rc < 0) return (total > 0) ? total : rc;
        total += rc;
        if ((size_t)rc != iov[i].iov_len) break;
    }
    return total;
}

int sys_mount_module_root(void) {
    return fs_mount_module_root();
}

int sys_get_mount_status(char* buf, size_t size) {
    return fs_get_mount_status(buf, size);
}

int sys_pipe_user(int* user_pipefd) {
    int pipefd[2];
    int ret = fs_pipe(pipefd);
    if (ret == 0) {
        user_pipefd[0] = pipefd[0];
        user_pipefd[1] = pipefd[1];
    }
    return ret;
}

int sys_pipe2_user(int* user_pipefd, int flags) {
    int pipefd[2];
    int ret = fs_pipe2(pipefd, flags);
    if (ret == 0) {
        user_pipefd[0] = pipefd[0];
        user_pipefd[1] = pipefd[1];
    }
    return ret;
}

void sys_ls_private(void) {
    sys_ls();
}

/* ------------------------------------------------------------------ */
/* pselect6 (最小実装)                                                  */
/*                                                                     */
/* GNU make の fifo ジョブサーバが必要とする範囲のみ:                    */
/*   - readfds/writefds のポーリング (10ms 粒度)                        */
/*   - シグナル pending で EINTR (SIGCHLD → reap のために必須)          */
/*   - timespec タイムアウト                                            */
/* sigmask 引数は無視する (シグナルはハンドラ配送されず pending bit の   */
/* みなので、マスク一時差し替えの意味が無い)。pending bit が消えない     */
/* シグナルを抱えたタスクは EINTR を連発するが、make は reap で bit20    */
/* を消すので進行する。                                                  */
/* ------------------------------------------------------------------ */

#ifndef EINTR
#define EINTR 4
#endif
#ifndef EBADF
#define EBADF 9
#endif

static int pselect_fd_ready(const file_descriptor_t* f, int want_write) {
    file_type_t type = fs_fd_type(f);
    if (type == FT_PIPE) {
        const pipe_t* pipe = (const pipe_t*)fs_fd_data(f);
        if (!pipe) {
            const fs_file_t* file = f->file;
            pipe = file ? (const pipe_t*)file->private_data : 0;
        }
        if (!pipe) return 1;
        /* BKL 下のスナップショット読み。外れても次周期で拾う。 */
        if (want_write) return pipe->count < PIPE_BUF_SIZE || pipe->ref_count < 2;
        return pipe->count > 0 || pipe->ref_count < 2; /* データ有り or EOF */
    }
    /* コンソール・通常ファイル・キャラデバイス等は常に ready 扱い。 */
    return 1;
}

int64_t sys_pselect6(int nfds, uint64_t* readfds, uint64_t* writefds,
                     uint64_t* exceptfds, const struct orth_timespec_k* timeout,
                     const void* sigmask) {
    struct task* current = get_current_task();
    uint64_t deadline = 0;
    int have_deadline = 0;
    int nwords;
    (void)sigmask;

    if (!current) return -EINVAL;
    if (nfds < 0) return -EINVAL;
    if (nfds > MAX_FDS) nfds = MAX_FDS;
    nwords = (nfds + 63) / 64;

    if (timeout) {
        uint64_t ms;
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
            timeout->tv_nsec >= 1000000000LL) return -EINVAL;
        ms = (uint64_t)timeout->tv_sec * 1000ULL +
             (uint64_t)((timeout->tv_nsec + 999999LL) / 1000000LL);
        deadline = lapic_get_ticks_ms() + ms;
        have_deadline = 1;
    }

    while (1) {
        uint64_t rdy_r[(MAX_FDS + 63) / 64] = {0};
        uint64_t rdy_w[(MAX_FDS + 63) / 64] = {0};
        int nready = 0;

        for (int fd = 0; fd < nfds; fd++) {
            int word = fd >> 6;
            uint64_t bit = 1ULL << (fd & 63);
            int want_r = readfds && (readfds[word] & bit);
            int want_w = writefds && (writefds[word] & bit);
            if (!want_r && !want_w) continue;
            if (!current->fds[fd].in_use) return -EBADF;
            if (want_r && pselect_fd_ready(&current->fds[fd], 0)) {
                rdy_r[word] |= bit;
                nready++;
            }
            if (want_w && pselect_fd_ready(&current->fds[fd], 1)) {
                rdy_w[word] |= bit;
                nready++;
            }
        }

        if (nready > 0) {
            for (int w = 0; w < nwords; w++) {
                if (readfds) readfds[w] = rdy_r[w];
                if (writefds) writefds[w] = rdy_w[w];
                if (exceptfds) exceptfds[w] = 0;
            }
            return nready;
        }
        if (current->sig_pending) return -EINTR;
        if (have_deadline && lapic_get_ticks_ms() >= deadline) {
            for (int w = 0; w < nwords; w++) {
                if (readfds) readfds[w] = 0;
                if (writefds) writefds[w] = 0;
                if (exceptfds) exceptfds[w] = 0;
            }
            return 0;
        }
        sys_sleep_ms(10);
    }
}
