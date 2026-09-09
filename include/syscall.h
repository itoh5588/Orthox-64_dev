#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

// Linux x86_64 system call numbers used by userland and musl.
#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_OPEN    2
#define SYS_CLOSE   3
#define SYS_STAT    4
#define SYS_FSTAT   5
#define SYS_LSTAT   6
#define SYS_ACCESS  21
#define SYS_GETDENTS 78
#define SYS_LSEEK   8
#define SYS_MMAP    9
#define SYS_MPROTECT 10
#define SYS_MUNMAP  11
#define SYS_BRK     12
#define SYS_RT_SIGACTION 13
#define SYS_RT_SIGPROCMASK 14
#define SYS_IOCTL   16
#define SYS_PREAD64 17
#define SYS_PWRITE64 18
#define SYS_READV   19
#define SYS_WRITEV  20
#define SYS_PIPE    22
#define SYS_SCHED_YIELD 24
#define SYS_MREMAP  25
#define SYS_MADVISE 28
#define SYS_DUP2    33
#define SYS_NANOSLEEP 35
#define SYS_GETPID  39
#define SYS_SOCKET  41
#define SYS_CONNECT 42
#define SYS_ACCEPT  43
#define SYS_SENDTO  44
#define SYS_RECVFROM 45
#define SYS_SHUTDOWN 48
#define SYS_BIND    49
#define SYS_LISTEN  50
#define SYS_GETSOCKNAME 51
#define SYS_GETPEERNAME 52
#define SYS_SETSOCKOPT 54
#define SYS_FORK    57
#define SYS_EXECVE  59
#define SYS_EXIT    60
#define SYS_WAIT4   61
#define SYS_KILL    62
#define SYS_UNAME   63
#define SYS_FCNTL   72
#define SYS_GETTIMEOFDAY 96
#define SYS_GETRLIMIT 97
#define SYS_SYSINFO 99
#define SYS_GETUID  102
#define SYS_GETGID  104
#define SYS_GETEUID 107
#define SYS_GETCWD  79
#define SYS_CHDIR   80
#define SYS_FCHDIR  81
#define SYS_RENAME  82
#define SYS_MKDIR   83
#define SYS_RMDIR   84
#define SYS_UNLINK  87
#define SYS_MKNOD   133
#define SYS_READLINK 89
#define SYS_CHMOD   90
#define SYS_TRUNCATE 76
#define SYS_FTRUNCATE 77
#define SYS_SETPGID 109
#define SYS_GETPPID 110
#define SYS_GETPGRP 111
#define SYS_SETSID  112
#define SYS_GETEGID 108
#define SYS_SIGALTSTACK 131
#define SYS_ARCH_PRCTL 158
#define SYS_SYNC    162
#define SYS_FUTEX   202
#define SYS_GETDENTS64 217
#define SYS_SET_TID_ADDRESS 218
#define SYS_CLOCK_GETTIME 228
#define SYS_EXIT_GROUP 231
#define SYS_OPENAT  257
#define SYS_MKDIRAT 258
#define SYS_FSTATAT 262
#define SYS_UNLINKAT 263
#define SYS_READLINKAT 267
#define SYS_FACCESSAT 269
#define SYS_PSELECT6 270
#define SYS_SET_ROBUST_LIST 273
#define SYS_UTIMENSAT 280
#define SYS_PIPE2   293
#define SYS_PRLIMIT64 302
#define SYS_GETRANDOM 318

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

#define FUTEX_WAIT    0
#define FUTEX_WAKE    1
#define FUTEX_PRIVATE 128

// OrthOS private syscalls. Keep these outside the Linux x86_64 range.
#define ORTH_SYS_BASE 1000
#define ORTH_SYS_LS               (ORTH_SYS_BASE + 0)
#define ORTH_SYS_GET_VIDEO_INFO   (ORTH_SYS_BASE + 1)
#define ORTH_SYS_MAP_FRAMEBUFFER  (ORTH_SYS_BASE + 2)
#define ORTH_SYS_GET_TICKS_MS     (ORTH_SYS_BASE + 3)
#define ORTH_SYS_SLEEP_MS         (ORTH_SYS_BASE + 4)
#define ORTH_SYS_GET_KEY_EVENT    (ORTH_SYS_BASE + 5)
#define ORTH_SYS_SOUND_ON         (ORTH_SYS_BASE + 6)
#define ORTH_SYS_SOUND_OFF        (ORTH_SYS_BASE + 7)
#define ORTH_SYS_SOUND_PCM_U8     (ORTH_SYS_BASE + 8)
#define ORTH_SYS_USB_INFO         (ORTH_SYS_BASE + 9)
#define ORTH_SYS_USB_READ_BLOCK   (ORTH_SYS_BASE + 10)
#define ORTH_SYS_MOUNT_USB_ROOT   (ORTH_SYS_BASE + 11)
#define ORTH_SYS_MOUNT_MODULE_ROOT (ORTH_SYS_BASE + 12)
#define ORTH_SYS_GET_MOUNT_STATUS (ORTH_SYS_BASE + 13)
#define ORTH_SYS_TCGETPGRP        (ORTH_SYS_BASE + 14)
#define ORTH_SYS_TCSETPGRP        (ORTH_SYS_BASE + 15)
#define ORTH_SYS_TCGETATTR        (ORTH_SYS_BASE + 16)
#define ORTH_SYS_TCSETATTR        (ORTH_SYS_BASE + 17)
#define ORTH_SYS_SIGPROCMASK      (ORTH_SYS_BASE + 18)
#define ORTH_SYS_SIGPENDING       (ORTH_SYS_BASE + 19)
#define ORTH_SYS_SIGACTION        (ORTH_SYS_BASE + 20)
#define ORTH_SYS_DNS_LOOKUP       (ORTH_SYS_BASE + 21)
#define ORTH_SYS_GET_CPU_ID       (ORTH_SYS_BASE + 22)
#define ORTH_SYS_SET_FORK_SPREAD  (ORTH_SYS_BASE + 23)
#define ORTH_SYS_GET_FORK_SPREAD  (ORTH_SYS_BASE + 24)
#define ORTH_SYS_GET_RUNQ_STATS   (ORTH_SYS_BASE + 25)

// mmap flags/protection (Linux-compatible subset)
#ifndef PROT_NONE
#define PROT_NONE   0x0
#endif
#ifndef PROT_READ
#define PROT_READ   0x1
#endif
#ifndef PROT_WRITE
#define PROT_WRITE  0x2
#endif
#ifndef PROT_EXEC
#define PROT_EXEC   0x4
#endif

#ifndef MAP_SHARED
#define MAP_SHARED    0x01
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE   0x02
#endif
#ifndef MAP_FIXED
#define MAP_FIXED     0x10
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif

struct video_info {
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint64_t bpp;
};

struct key_event {
    uint8_t pressed;
    uint8_t scancode;
    uint16_t ascii;
};

struct orth_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_cc[20];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

struct orth_sigaction {
    uint64_t sa_handler;
    uint64_t sa_mask;
    uint32_t sa_flags;
    uint32_t reserved;
};

struct orth_runq_stat {
    uint32_t cpu_id;
    uint32_t runq_count;
    uint32_t total_load;
    uint32_t affined_tasks;
    uint32_t affined_ready;
    uint32_t affined_running;
    uint32_t affined_sleeping;
    uint32_t blocked_ready;
    uint32_t blocked_running;
    uint32_t blocked_sleeping;
    int32_t current_pid;
    int32_t current_state;
    int32_t runq_head_pid;
    int32_t runq_tail_pid;
    uint32_t current_is_idle;
    uint32_t migratable_count;
};

#ifndef ORTH_DIRENT_DEFINED
#define ORTH_DIRENT_DEFINED
struct orth_dirent {
    uint32_t mode;
    uint32_t size;
    char name[248];
};
#endif

void syscall_init(void);
void syscall_init_cpu(void);
int get_video_info(struct video_info* info);
uint64_t map_framebuffer(void);
uint64_t get_ticks_ms(void);
int sleep_ms(uint64_t ms);
int get_key_event(struct key_event* ev);
int sound_on(uint32_t freq_hz);
int sound_off(void);
int sound_pcm_u8(const uint8_t* samples, uint32_t count, uint32_t sample_rate);
int usb_info(void);
int usb_read_block_sys(uint32_t lba, void* buf, uint32_t count);
int mount_usb_root(const char* path);
int mount_module_root(void);
int get_mount_status(char* buf, uint32_t size);
int getdents_sys(int fd, struct orth_dirent* dirp, uint32_t count);
int chdir_sys(const char* path);
int getcwd_sys(char* buf, uint32_t size);
int kill_sys(int pid, int sig);
int getpgrp_sys(void);
int setpgid_sys(int pid, int pgid);
int setsid_sys(void);
int tcgetpgrp_sys(int fd);
int tcsetpgrp_sys(int fd, int pgrp);
int tcgetattr_sys(int fd, struct orth_termios* tio);
int tcsetattr_sys(int fd, int optional_actions, const struct orth_termios* tio);
int sigprocmask_sys(int how, const uint64_t* set, uint64_t* oldset);
int sigpending_sys(uint64_t* set);
int sigaction_sys(int sig, const struct orth_sigaction* act, struct orth_sigaction* oldact);
int dns_lookup_ipv4(const char* hostname, uint32_t* out_addr);
int get_cpu_id(void);
int set_fork_spread(int enabled);
int get_fork_spread(void);
int get_runq_stats(struct orth_runq_stat* out, uint32_t max_count);

#endif
