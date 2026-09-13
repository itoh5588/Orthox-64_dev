#include <stddef.h>
#include <stdint.h>
#include "linux_errno.h"
#include "linux_syscall.h"   /* arch_random_bytes */
#include "sys_internal.h"

/* **getrandom(2)。乱数の材料は arch が出す。**
 *
 * 2026-09-07 まで、ここに rdtsc / cpuid / rdrand を直書きしていた。**x86 の
 * 命令が syscall 層に居た**ので、材料の作り方を kernel/x86_64/rng.c へ出した。
 * アルゴリズムは変えていない。
 *
 * **2026-09-08 に aarch64 / riscv64 側 (linux_bootstrap_sys_getrandom) と
 * 揃えた。**それまでは arch_random_fill で必ず len バイト埋めており、
 * **乱数源の無い機械でも「乱数を得た」と答えていた。**いまは
 * arch_random_bytes を使い、源が無ければ ENOSYS を返す ——
 * **適当な値を混ぜて長さだけ揃えると、呼んだ側は乱数を得たつもりで
 * 先へ進む。**
 *
 * **2026-09-13 に 3 アーキ共通にした (別実装 29 組の 1 組)。**09-08 に
 * 揃えた時点で linux 側の複製とは同じ中身になっていたので、このファイルを
 * aarch64 / riscv64 の一覧にも入れ、linux_syscall.c の複製を外した。 */
int64_t sys_getrandom(void* buf, size_t len, unsigned flags) {
    int64_t got;
    (void)flags;
    if (!buf) return -LINUX_EFAULT;
    if (len == 0) return 0;
    got = arch_random_bytes(buf, len);
    if (got < 0) return -LINUX_ENOSYS;   /* この機械に乱数源が無い */
    return got;
}
