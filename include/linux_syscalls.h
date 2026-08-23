#ifndef ORTHOX_LINUX_SYSCALLS_H
#define ORTHOX_LINUX_SYSCALLS_H

/*
 * riscv64 (asm-generic) の Linux syscall 番号。
 *
 * riscv64 のユーザーランドは musl のみで、musl はこの番号しか発行しない。
 * include/syscall.h の x86 レガシー番号 (SYS_READ=0, SYS_FORK=57 ...) は
 * riscv64 の番号空間と無関係に衝突するため、riscv64 側では一切使わないこと。
 * 過去に 2 度、レガシー番号を混ぜたことで実害が出ている:
 *   - SYS_FORK(57) vs close(57)      → `close(0)` が fork として実行された
 *   - SYS_GETDENTS(78) vs readlinkat → realpath が壊れた
 * 番号を足すときは asm-generic の unistd.h と突き合わせること。
 *
 * カーネル内の selftest (kernel/riscv64/boot.c) もこのヘッダを使う。
 * かつては selftest がレガシー番号を直書きしていたため、ディスパッチャから
 * レガシー番号を落とせなかった。
 */

#define LINUX_SYS_GETCWD           17
#define LINUX_SYS_DUP              23
#define LINUX_SYS_DUP3             24
#define LINUX_SYS_FCNTL            25
#define LINUX_SYS_IOCTL            29
#define LINUX_SYS_MKDIRAT          34
#define LINUX_SYS_UNLINKAT         35
#define LINUX_SYS_LINKAT           37
#define LINUX_SYS_TRUNCATE         45
#define LINUX_SYS_FTRUNCATE        46
#define LINUX_SYS_FACCESSAT        48
#define LINUX_SYS_CHDIR            49
#define LINUX_SYS_FCHDIR           50
#define LINUX_SYS_FCHMOD           52
#define LINUX_SYS_FCHMODAT         53
#define LINUX_SYS_OPENAT           56
#define LINUX_SYS_CLOSE            57
#define LINUX_SYS_PIPE2            59
#define LINUX_SYS_GETDENTS64       61
#define LINUX_SYS_LSEEK            62
#define LINUX_SYS_READ             63
#define LINUX_SYS_WRITE            64
#define LINUX_SYS_READV            65
#define LINUX_SYS_WRITEV           66
#define LINUX_SYS_PPOLL            73
#define LINUX_SYS_READLINKAT       78
#define LINUX_SYS_NEWFSTATAT       79
#define LINUX_SYS_FSTAT            80
#define LINUX_SYS_SYNC             81
#define LINUX_SYS_FSYNC            82
#define LINUX_SYS_FDATASYNC        83
#define LINUX_SYS_UTIMENSAT        88
#define LINUX_SYS_EXIT             93
#define LINUX_SYS_EXIT_GROUP       94
#define LINUX_SYS_WAITID           95
#define LINUX_SYS_SET_TID_ADDRESS  96
#define LINUX_SYS_FUTEX            98
#define LINUX_SYS_NANOSLEEP        101
#define LINUX_SYS_CLOCK_GETTIME    113
#define LINUX_SYS_RT_SIGACTION     134
#define LINUX_SYS_RT_SIGPROCMASK   135
#define LINUX_SYS_UNAME            160
/* reboot(2)。**焼き直しのたびに電源を抜かなくて済むように入れた。**
 * Linux と同じで、魔法の数を 2 つ揃えないと効かない (誤爆よけ) */
#define LINUX_SYS_REBOOT           142
#define LINUX_REBOOT_MAGIC1        0xfee1deadU
#define LINUX_REBOOT_MAGIC2        672274793U      /* 0x28121969 */
#define LINUX_REBOOT_MAGIC2A       85072278U       /* 0x05121996 */
#define LINUX_REBOOT_MAGIC2B       369367448U      /* 0x16041998 */
#define LINUX_REBOOT_MAGIC2C       537993216U      /* 0x20112000 */
#define LINUX_REBOOT_CMD_RESTART   0x01234567U
/* **実測で ENOSYS が出たもの (2026-08-11)。** Orthox の中で
 * gcc / ld / make を動かしたときに呼ばれた。どれも戻り値を見て続行する
 * 作りだったので致命的ではなかったが、埋めておく */
#define LINUX_SYS_GETRLIMIT        163
#define LINUX_SYS_SETRLIMIT        164
#define LINUX_SYS_GETRUSAGE        165
#define LINUX_SYS_UMASK            166
#define LINUX_SYS_SYSINFO          179
#define LINUX_SYS_GETPID           172
#define LINUX_SYS_GETPPID          173
#define LINUX_SYS_GETUID           174
#define LINUX_SYS_GETEUID          175
#define LINUX_SYS_GETGID           176
#define LINUX_SYS_GETEGID          177
#define LINUX_SYS_BRK              214
#define LINUX_SYS_MUNMAP           215
#define LINUX_SYS_CLONE            220
#define LINUX_SYS_EXECVE           221
#define LINUX_SYS_MMAP             222
#define LINUX_SYS_WAIT4            260
#define LINUX_SYS_PRLIMIT64        261
#define LINUX_SYS_RENAMEAT2        276
#define LINUX_SYS_GETRANDOM        278

/* ---- getrlimit / prlimit64 の資源番号 (asm-generic) ---------------------- */
#define LINUX_RLIMIT_CPU        0
#define LINUX_RLIMIT_FSIZE      1
#define LINUX_RLIMIT_DATA       2
#define LINUX_RLIMIT_STACK      3
#define LINUX_RLIMIT_CORE       4
#define LINUX_RLIMIT_RSS        5
#define LINUX_RLIMIT_NPROC      6
#define LINUX_RLIMIT_NOFILE     7
#define LINUX_RLIMIT_MEMLOCK    8
#define LINUX_RLIMIT_AS         9
#define LINUX_RLIMIT_NLIMITS    16
#define LINUX_RLIM_INFINITY     (~0ULL)

/* LP64 なので struct rlimit と struct rlimit64 は同じ形 */
struct linux_rlimit64 {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

/* getrusage(2)。**中身は 0 で構わない** — 呼び手 (gcc の -ftime-report 等)
 * は「取れたかどうか」しか見ない。形さえ合っていれば読み書きが壊れない */
struct linux_timeval64 {
    int64_t tv_sec;
    int64_t tv_usec;
};
struct linux_rusage {
    struct linux_timeval64 ru_utime;
    struct linux_timeval64 ru_stime;
    int64_t ru_maxrss, ru_ixrss, ru_idrss, ru_isrss;
    int64_t ru_minflt, ru_majflt, ru_nswap;
    int64_t ru_inblock, ru_oublock;
    int64_t ru_msgsnd, ru_msgrcv;
    int64_t ru_nsignals, ru_nvcsw, ru_nivcsw;
};
#define LINUX_RUSAGE_SELF        0
#define LINUX_RUSAGE_CHILDREN  (-1)

/* sysinfo(2)。**mem_unit を 0 にしないこと** — 呼び手が totalram に
 * 掛けるので、0 だと「メモリ 0」に見える */
struct linux_sysinfo {
    int64_t  uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    char     _f[0];
};

/* *at 系のディレクトリ fd。musl と同じ値 */
#define LINUX_AT_FDCWD             (-100)
#define LINUX_AT_EMPTY_PATH        0x1000

#endif
