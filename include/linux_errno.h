#ifndef ORTHOX_LINUX_ERRNO_H
#define ORTHOX_LINUX_ERRNO_H

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

#define LINUX_EPERM        1
#define LINUX_ENOENT       2
#define LINUX_ESRCH        3
/* pselect6 がシグナルで抜けたとき (kernel/linux_syscall.c)。make の jobserver が
 * 子の終わりをこれで知る */
#define LINUX_EINTR        4
#define LINUX_E2BIG        7
#define LINUX_EBADF        9
#define LINUX_ECHILD      10
#define LINUX_EAGAIN      11
#define LINUX_ENOMEM      12
/* faccessat で実行ビットが無いとき (kernel/sys_access.c)。x86 の kernel/fs.c が
 * 自前の EACCES を使っていたが、共有層から見える名前が無かった */
#define LINUX_EACCES      13
#define LINUX_EFAULT      14
#define LINUX_EEXIST      17
/* パイプを mmap しようとしたとき (kernel/sys_mmap.c)。x86 が生の -19 で
 * 返していたものに名前を付けた */
#define LINUX_ENODEV      19
#define LINUX_ENOTDIR     20
#define LINUX_EISDIR      21
#define LINUX_EINVAL      22
/* open file description を作れない (x86 の kernel/fs.c も同じ場面で ENFILE) */
#define LINUX_ENFILE      23
#define LINUX_EMFILE      24
#define LINUX_ENOTTY      25
#define LINUX_ENOSPC      28
#define LINUX_ESPIPE      29
#define LINUX_EROFS       30
/* getcwd でバッファが足りないとき (kernel/sys_task.c)。kernel/fs.c が
 * 自前で ERANGE 34 を持っていたが、共有層から見える名前が無かった */
#define LINUX_ERANGE      34
#define LINUX_ENOSYS      38
#define LINUX_ENOTEMPTY   39

#endif /* ORTHOX_LINUX_ERRNO_H */
