#ifndef ORTHOX_LINUX_ABI_H
#define ORTHOX_LINUX_ABI_H

#include <stddef.h>
#include <stdint.h>

/* **ユーザ空間と受け渡しする Linux ABI の構造体。ここが唯一の定義。**
 *
 * 2026-09-07 まで、**同じ ABI を 3 箇所が別名で定義していた**:
 *   include/sys_internal.h      x86 の syscall 実装が使う (_k 付き)
 *   include/linux_syscalls.h    aarch64 / riscv64 側 (64 付き)
 *   kernel/linux_syscall.c      ファイルの中に直書き
 * レイアウトは同じで名前だけが違い、**どれを直せばよいのか分からなかった。**
 * 名前は Linux の呼び方に合わせた。
 *
 * **レイアウトはユーザ空間との取り決めなので勝手に変えられない。**メンバーを
 * 足す・並べ替えると musl 側の解釈と食い違う。
 *
 * **ここに入れていない ABI 構造体が 2 つある。**両者でレイアウトが違っていて、
 * どちらを正とするかは別の判断が要るため統合していない:
 *   sysinfo   x86 側は末尾が __reserved[256] で 368 バイト、linux 側は _f[0] で
 *             112 バイト。**musl の struct sysinfo にも __reserved[256] が
 *             あって 368 バイト**なので、大きさが合っているのは x86 側。
 *             linux 側は先頭 112 バイトだけ書く (はみ出しはしない。musl は
 *             __reserved を読まないので実害は出ていない)
 *   termios   x86 側は c_cc[20] で c_line が無い。Linux ABI は c_line + c_cc[32]
 */

struct linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct linux_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

/* **LP64 なので struct rlimit と struct rlimit64 は同じ形。** */
struct linux_rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

/* **asm-generic の utsname は各フィールド 65 バイト固定。** */
struct linux_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

/* **iov_base に const は付けない。**readv は同じ形で書き込み先を受ける */
struct linux_iovec {
    void* iov_base;
    size_t iov_len;
};

struct linux_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

/* rt_sigprocmask の how。**同じ値を x86 側 (sys_signal.c) と linux 側
 * (linux_syscall.c) の両方が使う。**以前は x86 が生の 0/1/2、linux 側が
 * この名前で、片方を直したときにもう片方を見落とす形だった。 */
#define LINUX_SIG_BLOCK   0
#define LINUX_SIG_UNBLOCK 1
#define LINUX_SIG_SETMASK 2

/* シグナル番号。**2026-09-09 まで x86 だけ SIGCHLD に 20 を使っていた。**
 * 20 は x86_64 Linux では SIGTSTP なので、musl でビルドした busybox からは
 * 「子が終わった」ではなく「端末で停止しろ」に見える。3 アーキとも
 * asm-generic / x86_64 共通の番号 (どちらも同じ値) に揃えた。
 *
 * **必要になったものだけ置く。**全部並べると、使っていない番号が正しいか
 * 確かめられないまま増える。 */
#define LINUX_SIGINT   2
#define LINUX_SIGQUIT  3
#define LINUX_SIGCHLD 17

#endif
