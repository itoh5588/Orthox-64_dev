#ifndef ORTHOX_RISCV64_ERRNO_H
#define ORTHOX_RISCV64_ERRNO_H

/*
 * riscv64 の syscall 戻り値で使う errno 定数 (Linux asm-generic の値)。
 *
 * カーネルの戻り値は -errno 規約。musl の __syscall_ret() が
 * 「-4096 < ret < 0」を見て errno へ写すので、失敗を -1 で返すと
 * ユーザーには EPERM = "Operation not permitted" として出てくる。
 * busybox は ENOENT / ECHILD を見て挙動を変える箇所が多く
 * (`rm -f` と ash のジョブ回収が典型)、EPERM だと誤動作する。
 *
 * 「よく分からない失敗はとりあえず -1」を書かないための共有定義。
 * tests/riscv64_errno_smoke.sh が代表的な失敗系を見張っている。
 */

#define RISCV64_EPERM        1
#define RISCV64_ENOENT       2
#define RISCV64_ESRCH        3
#define RISCV64_EBADF        9
#define RISCV64_ECHILD      10
#define RISCV64_EAGAIN      11
#define RISCV64_ENOMEM      12
#define RISCV64_EFAULT      14
#define RISCV64_EEXIST      17
#define RISCV64_ENOTDIR     20
#define RISCV64_EISDIR      21
#define RISCV64_EINVAL      22
#define RISCV64_EMFILE      24
#define RISCV64_ENOTTY      25
#define RISCV64_ENOSPC      28
#define RISCV64_ESPIPE      29
#define RISCV64_EROFS       30
#define RISCV64_ENOSYS      38
#define RISCV64_ENOTEMPTY   39

#endif /* ORTHOX_RISCV64_ERRNO_H */
