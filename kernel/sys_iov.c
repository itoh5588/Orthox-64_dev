/* ---- write / writev / readv (3 アーキ共通) ---------------------------------
 *
 * **別実装 29 組のうち 3 組を畳んだもの (2026-09-13)。**もとは
 *
 *     x86              kernel/sys_fs.c        の sys_writev / sys_readv
 *     aarch64/riscv64  kernel/linux_syscall.c の linux_bootstrap_sys_write /
 *                                                writev / readv
 *
 * write は linux 側が sys_write を呼ぶだけの 1 段だったので外した。sys_write /
 * sys_read は 3 アーキにある (x86 と aarch64 は kernel/sys_fs.c、riscv64 は
 * kernel/riscv64/fs.c)。
 *
 * writev / readv の判断は同じで、違いは 1 つだけだった:
 *
 * | | x86 | linux 側 (採用) |
 * |---|---|---|
 * | iovcnt == 0 かつ iov == NULL | **EFAULT** | 0 |
 *
 * **Linux は iovcnt が 0 なら iov を見ずに 0 を返す**ので linux 側に寄せた。
 * x86 の答えが変わる側。 */

#include <stddef.h>
#include <stdint.h>
#include "linux_abi.h"       /* struct linux_iovec */
#include "linux_errno.h"
#include "sys_internal.h"
#include "task.h"

#ifndef ORTHOX_MEM_PROGRESS
#define ORTHOX_MEM_PROGRESS 0
#endif

/* 途中で失敗したら、それまでに運べたぶんを返す。短く運べたら (1 本の iov を
 * 使い切れなかったら) そこで止める —— 残りを続けて運ぶと、呼び手から見て
 * 順序が崩れる */
int64_t sys_writev(int fd, const struct linux_iovec* iov, int iovcnt) {
    int64_t total = 0;
    /* **条件で errno を分ける。**Linux は iovcnt が負なら EINVAL、
     * iov が読めなければ EFAULT */
    if (iovcnt < 0) return -LINUX_EINVAL;
    if (iovcnt != 0 && !iov) return -LINUX_EFAULT;
    for (int i = 0; i < iovcnt; i++) {
        int64_t rc = sys_write(fd, iov[i].iov_base, iov[i].iov_len);
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

int64_t sys_readv(int fd, const struct linux_iovec* iov, int iovcnt) {
    int64_t total = 0;
    if (iovcnt < 0) return -LINUX_EINVAL;
    if (iovcnt != 0 && !iov) return -LINUX_EFAULT;
    for (int i = 0; i < iovcnt; i++) {
        int64_t rc = sys_read(fd, (void*)iov[i].iov_base, iov[i].iov_len);
        if (rc < 0) return (total > 0) ? total : rc;
        total += rc;
        if ((size_t)rc != iov[i].iov_len) break;
    }
    return total;
}
