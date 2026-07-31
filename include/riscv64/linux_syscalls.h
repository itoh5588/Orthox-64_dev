#ifndef ORTHOX_ARCH_RISCV64_LINUX_SYSCALLS_H
#define ORTHOX_ARCH_RISCV64_LINUX_SYSCALLS_H

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

#define RISCV64_LINUX_SYS_GETCWD           17
#define RISCV64_LINUX_SYS_DUP              23
#define RISCV64_LINUX_SYS_DUP3             24
#define RISCV64_LINUX_SYS_FCNTL            25
#define RISCV64_LINUX_SYS_IOCTL            29
#define RISCV64_LINUX_SYS_MKDIRAT          34
#define RISCV64_LINUX_SYS_UNLINKAT         35
#define RISCV64_LINUX_SYS_LINKAT           37
#define RISCV64_LINUX_SYS_TRUNCATE         45
#define RISCV64_LINUX_SYS_FTRUNCATE        46
#define RISCV64_LINUX_SYS_FACCESSAT        48
#define RISCV64_LINUX_SYS_CHDIR            49
#define RISCV64_LINUX_SYS_FCHDIR           50
#define RISCV64_LINUX_SYS_FCHMOD           52
#define RISCV64_LINUX_SYS_FCHMODAT         53
#define RISCV64_LINUX_SYS_OPENAT           56
#define RISCV64_LINUX_SYS_CLOSE            57
#define RISCV64_LINUX_SYS_PIPE2            59
#define RISCV64_LINUX_SYS_GETDENTS64       61
#define RISCV64_LINUX_SYS_LSEEK            62
#define RISCV64_LINUX_SYS_READ             63
#define RISCV64_LINUX_SYS_WRITE            64
#define RISCV64_LINUX_SYS_READV            65
#define RISCV64_LINUX_SYS_WRITEV           66
#define RISCV64_LINUX_SYS_READLINKAT       78
#define RISCV64_LINUX_SYS_NEWFSTATAT       79
#define RISCV64_LINUX_SYS_FSTAT            80
#define RISCV64_LINUX_SYS_SYNC             81
#define RISCV64_LINUX_SYS_FSYNC            82
#define RISCV64_LINUX_SYS_FDATASYNC        83
#define RISCV64_LINUX_SYS_UTIMENSAT        88
#define RISCV64_LINUX_SYS_EXIT             93
#define RISCV64_LINUX_SYS_EXIT_GROUP       94
#define RISCV64_LINUX_SYS_WAITID           95
#define RISCV64_LINUX_SYS_SET_TID_ADDRESS  96
#define RISCV64_LINUX_SYS_FUTEX            98
#define RISCV64_LINUX_SYS_NANOSLEEP        101
#define RISCV64_LINUX_SYS_CLOCK_GETTIME    113
#define RISCV64_LINUX_SYS_RT_SIGACTION     134
#define RISCV64_LINUX_SYS_RT_SIGPROCMASK   135
#define RISCV64_LINUX_SYS_UNAME            160
#define RISCV64_LINUX_SYS_GETPID           172
#define RISCV64_LINUX_SYS_GETPPID          173
#define RISCV64_LINUX_SYS_GETUID           174
#define RISCV64_LINUX_SYS_GETEUID          175
#define RISCV64_LINUX_SYS_GETGID           176
#define RISCV64_LINUX_SYS_GETEGID          177
#define RISCV64_LINUX_SYS_BRK              214
#define RISCV64_LINUX_SYS_MUNMAP           215
#define RISCV64_LINUX_SYS_CLONE            220
#define RISCV64_LINUX_SYS_EXECVE           221
#define RISCV64_LINUX_SYS_MMAP             222
#define RISCV64_LINUX_SYS_WAIT4            260
#define RISCV64_LINUX_SYS_RENAMEAT2        276
#define RISCV64_LINUX_SYS_GETRANDOM        278

/* *at 系のディレクトリ fd。musl と同じ値 */
#define RISCV64_LINUX_AT_FDCWD             (-100)
#define RISCV64_LINUX_AT_EMPTY_PATH        0x1000

#endif
