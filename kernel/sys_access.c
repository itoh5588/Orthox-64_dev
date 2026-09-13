/* ---- faccessat (3 アーキ共通) ----------------------------------------------
 *
 * **別実装 29 組のうちの 1 組を畳んだもの (2026-09-13)。**もとは
 *
 *     x86              kernel/sys_fs.c        の sys_faccessat -> fs_faccessat
 *     aarch64/riscv64  kernel/linux_syscall.c の case の中で sys_fstatat を代用
 *
 * **linux 側は access(2) の役を果たしていなかった。**
 *
 * | | x86 (fs_faccessat) | linux 側 (fstatat で代用) |
 * |---|---|---|
 * | mode      | 見る (X_OK は実行ビットかディレクトリが要る) | **見ない (常に通す)** |
 * | 失敗の理由 | ENOENT に潰す | **何でも -2 (ENOENT)** |
 * | flags     | **第 4 レジスタを読む** | 見ない |
 *
 * **x86 の判定を、3 アーキにある sys_fstatat の上に載せた。**riscv64 は
 * fs_faccessat を持たない (kernel/riscv64/fs.c) が sys_fstatat はある。
 * 失敗の理由は sys_fstatat の返り値をそのまま返す (ENOTDIR などを潰さない)。
 *
 * **SYS_faccessat は syscall としては引数 3 つ。**flags は別番号の
 * faccessat2 で渡る。musl は flag が 0 なら syscall(SYS_faccessat, fd,
 * filename, amode) と 3 引数で呼ぶ (ports/musl/src/unistd/faccessat.c) ので、
 * 第 4 レジスタには前の値が残っている。x86 はそれを flags と見なしており、
 * 余計なビットが立っていると EINVAL を返しえた。**ディスパッチは両側とも
 * flags に 0 を渡す。**faccessat2 の口はまだ無い (musl は ENOSYS を見て
 * 自分で落ちる)。
 *
 * uid / gid は 0 しか無いので、root の規則で判定する (R_OK / W_OK は常に通し、
 * X_OK だけ実行ビットを見る)。 */

#include <stddef.h>
#include <stdint.h>
#include "fs.h"               /* struct kstat / KSTAT_MODE_DIR */
#include "linux_errno.h"
#include "linux_syscalls.h"   /* LINUX_AT_* / LINUX_*_OK */
#include "sys_internal.h"

int sys_faccessat(int dirfd, const char* path, int mode, int flags) {
    struct kstat st;
    int rc;
    if (!path) return -LINUX_EFAULT;
    if (flags & ~(LINUX_AT_EACCESS | LINUX_AT_SYMLINK_NOFOLLOW)) return -LINUX_EINVAL;
    if (mode & ~(LINUX_R_OK | LINUX_W_OK | LINUX_X_OK)) return -LINUX_EINVAL;

    /* AT_EACCESS は fstatat に渡さない (x86 / aarch64 の fs_fstatat は
     * AT_SYMLINK_NOFOLLOW 以外を EINVAL で断る) */
    rc = sys_fstatat(dirfd, path, &st, flags & LINUX_AT_SYMLINK_NOFOLLOW);
    if (rc < 0) return rc;

    if (mode == LINUX_F_OK) return 0;
    if ((mode & LINUX_X_OK) == 0) return 0;
    if ((st.mode & 0170000U) == KSTAT_MODE_DIR) return 0;
    if (st.mode & 0111U) return 0;
    return -LINUX_EACCES;
}
