#ifndef SYS_INTERNAL_H
#define SYS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include "syscall.h"
#include "linux_abi.h"

struct kstat;
struct task;

struct linux_stack_t_k {
    void* ss_sp;
    int ss_flags;
    size_t ss_size;
};

struct linux_rt_sigaction_k {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint32_t mask[2];
};

int sys_gettimeofday(struct linux_timeval* tv);
int sys_clock_gettime(int clock_id, struct linux_timespec* ts);
int sys_sched_yield(void);
int sys_nanosleep(const struct linux_timespec* req, struct linux_timespec* rem);
int sys_uname(struct linux_utsname* buf);
/* kernel/sys_rlimit.c (3 アーキ共通)。**resource は int。**範囲外を
 * EINVAL で断るので、符号なしにすると負が巨大値に化けて検査が効かない */
int sys_getrlimit(int resource, struct linux_rlimit* rlim);
int sys_setrlimit(int resource, const struct linux_rlimit* rlim);
int sys_prlimit64(int pid, int resource, const struct linux_rlimit* new_limit,
                  struct linux_rlimit* old_limit);
struct linux_sysinfo;   /* include/linux_syscalls.h。実装は kernel/sys_task.c */
int sys_sysinfo(struct linux_sysinfo* info);
int sys_sleep_ms(uint64_t ms);
int sys_sigaltstack(const struct linux_stack_t_k* ss, struct linux_stack_t_k* old_ss);
int sys_sigprocmask(int how, const uint64_t* set, uint64_t* oldset);
int sys_rt_sigprocmask(int how, const uint64_t* set, uint64_t* oldset, size_t sigsetsize);
int sys_sigpending(uint64_t* set);
int sys_sigaction(int sig, const struct orth_sigaction* act, struct orth_sigaction* oldact);
int sys_rt_sigaction(int sig, const struct linux_rt_sigaction_k* act,
                     struct linux_rt_sigaction_k* oldact, size_t sigsetsize);
uint64_t sys_brk(uint64_t addr);
void* sys_mmap(void* addr, size_t length, int prot, int flags, int fd, int64_t offset);
int sys_munmap(void* addr, size_t length);
int sys_mprotect(void* addr, size_t length, int prot);
void* sys_mremap(void* old_addr, size_t old_len, size_t new_len, int flags, void* new_addr);
int sys_madvise(void* addr, size_t len, int advice);
uint64_t sys_getpid(void);
uint64_t sys_getppid(void);
uint64_t sys_getuid(void);
uint64_t sys_getgid(void);
uint64_t sys_geteuid(void);
uint64_t sys_getegid(void);
int sys_arch_prctl(int code, uint64_t addr);
int sys_futex(volatile int* uaddr, int op, int val);
int sys_set_robust_list(const void* head, size_t len);
int sys_set_tid_address(int* tidptr);
void sys_exit(int status);
int64_t sys_wait4(int pid, int* wstatus, int options);
int sys_kill(int pid, int sig);
int64_t sys_pselect6(int nfds, uint64_t* readfds, uint64_t* writefds,
                     uint64_t* exceptfds, const struct linux_timespec* timeout,
                     const void* sigmask);
int sys_getpgrp(void);
int sys_setpgid(int pid, int pgid);
int sys_setsid(void);
int sys_tcgetpgrp(int fd);
int sys_tcsetpgrp(int fd, int pgrp);
int sys_open(const char* path, int flags, int mode);
int sys_openat(int dirfd, const char* path, int flags, int mode);
int64_t sys_read(int fd, void* buf, size_t count);
int64_t sys_write(int fd, const void* buf, size_t count);
int sys_close(int fd);
int sys_fcntl(int fd, int cmd, uint64_t arg);
int sys_pipe(int pipefd[2]);
int sys_pipe2(int pipefd[2], int flags);
int sys_dup2(int oldfd, int newfd);
int sys_fstat(int fd, struct kstat* st);
int sys_stat(const char* path, struct kstat* st);
int sys_fstatat(int dirfd, const char* path, struct kstat* st, int flags);
int sys_access(const char* path, int mode);
int sys_faccessat(int dirfd, const char* path, int mode, int flags);
int64_t sys_readlink(const char* path, char* buf, size_t bufsiz);
int64_t sys_lseek(int fd, int64_t offset, int whence);
int sys_getdents(int fd, struct orth_dirent* dirp, size_t count);
int sys_getdents64(int fd, void* dirp, size_t count);
int sys_chdir(const char* path);
int sys_fchdir(int fd);
int sys_getcwd(char* buf, size_t size);
int sys_truncate(const char* path, uint64_t length);
int sys_ftruncate(int fd, uint64_t length);
int sys_utimensat(int dirfd, const char* path, const void* times, int flags);
int sys_sync(void);
struct orth_statfs;
int sys_statfs(const char* path, struct orth_statfs* out);
int sys_fstatfs(int fd, struct orth_statfs* out);
int sys_unlink(const char* path);
int sys_unlinkat(int dirfd, const char* path, int flags);
int sys_link(const char* oldpath, const char* newpath);
int sys_linkat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath, int flags);
int sys_rename(const char* oldpath, const char* newpath);
int sys_renameat(int olddirfd, const char* oldpath,
                 int newdirfd, const char* newpath, unsigned int flags);
int sys_chmod(const char* path, uint32_t mode);
int sys_mkdir(const char* path, int mode);
int sys_mkdirat(int dirfd, const char* path, int mode);
int sys_mknod(const char* path, uint32_t mode, uint64_t dev);
int sys_rmdir(const char* path);
int64_t sys_pread64(int fd, void* buf, size_t count, int64_t offset);
int64_t sys_pwrite64(int fd, const void* buf, size_t count, int64_t offset);
int sys_tcgetattr(int fd, struct orth_termios* tio);
int sys_tcsetattr(int fd, int optional_actions, const struct orth_termios* tio);
int sys_ioctl(int fd, unsigned long request, uint64_t arg);
int sys_lstat(const char* path, struct kstat* st);
int64_t sys_writev(int fd, const struct linux_iovec* iov, int iovcnt);
int64_t sys_readv(int fd, const struct linux_iovec* iov, int iovcnt);
int sys_mount_module_root(void);
int sys_get_mount_status(char* buf, size_t size);
int sys_pipe_user(int* user_pipefd);
int sys_pipe2_user(int* user_pipefd, int flags);
void sys_ls_private(void);
int sys_get_video_info(struct video_info* info);
uint64_t sys_map_framebuffer(void);
uint64_t sys_get_ticks_ms(void);
int sys_get_key_event(struct key_event* ev);
int sys_sound_on(uint32_t freq_hz);
int sys_sound_off(void);
int sys_sound_pcm_u8(const uint8_t* samples, uint32_t count, uint32_t sample_rate);
int sys_usb_info(void);
int sys_usb_read_block(uint32_t lba, void* user_buf, uint32_t count);
int sys_mount_usb_root(void);
int sys_get_cpu_id(void);
int sys_set_fork_spread(int enabled);
int sys_get_fork_spread(void);
int sys_get_runq_stats(struct orth_runq_stat* out, uint32_t max_count);
int64_t sys_getrandom(void* buf, size_t len, unsigned flags);
int sys_dns_lookup(const char* hostname, uint32_t* out_addr);
void syscall_trace_progress_bump(struct task* current, uint64_t* counter);
void syscall_trace_progress_bump_syscall(struct task* current, uint64_t syscall_no, uint64_t arg2);
void syscall_memtrace_mmap(const char* tag, uint64_t addr, uint64_t length, uint64_t result,
                           int prot, int flags, int fd);

#endif
