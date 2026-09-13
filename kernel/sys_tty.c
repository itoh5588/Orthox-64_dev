#include <stddef.h>
#include <stdint.h>
#include "linux_errno.h"
#include "sys_internal.h"
#include "task.h"

/* ---- 端末のフォアグラウンドプロセスグループ ------------------------------
 *
 * 元は x86 (kernel/sys_proc.c), aarch64 (kernel/aarch64/syscall.c),
 * aarch64/riscv64 の linux_bootstrap_sys_ioctl (kernel/linux_syscall.c) の
 * 3 箇所に同じロジックがそれぞれ複製されていた。aarch64 は sys_proc.c が
 * x86 の MSR 操作 (sys_arch_prctl) を含みリンクできないため独自に複製して
 * いたもの。**ジョブ制御はまだ無い**ので、値を覚えるだけ (ash は取得できれば
 * 動く)。EINVAL 検査は aarch64 版にのみあったものを採用した。 */
static int g_tty_pgrp;

int sys_tcgetpgrp(int fd) {
    (void)fd;
    if (g_tty_pgrp == 0) {
        struct task* current = get_current_task();
        if (current) g_tty_pgrp = current->pgid;
    }
    return g_tty_pgrp;
}

int sys_tcsetpgrp(int fd, int pgrp) {
    (void)fd;
    if (pgrp <= 0) return -LINUX_EINVAL;
    g_tty_pgrp = pgrp;
    return 0;
}

void tty_pgrp_set(int pgrp) {
    g_tty_pgrp = pgrp;
}

/* sys_tcgetpgrp と違い、未設定 (0) を現在タスクの pgid へ初期化する副作用を
 * 持たない。linux_console_deliver_intr が「まだ誰も TIOCSPGRP/TIOCGPGRP
 * していないなら何もしない」を判定するための素の読み取り専用アクセサ */
int tty_pgrp_peek(void) {
    return g_tty_pgrp;
}
