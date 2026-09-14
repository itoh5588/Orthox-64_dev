#include <stddef.h>
#include <stdint.h>
#include "fs.h"
#include "linux_errno.h"
#include "linux_syscall.h"   /* arch_console_echo_enabled の宣言 */
#include "sys_internal.h"
#include "task.h"

/* コンソール tty かどうかの判定。元は x86 (kernel/sys_fs.c) にだけあり、
 * linux 側 (aarch64/riscv64 共有の kernel/linux_syscall.c) の
 * TCGETS/TCSETS/TIOCGWINSZ はどの fd でも成功していた (別実装 29 組の
 * 18 組目、ioctl の残り、2026-09-14)。/dev/tty と /dev/console
 * (major 5) はコンソール tty 扱いだが、/dev/null など他のキャラクタ
 * デバイスは ENOTTY のままにする */
int fd_is_console(int fd) {
    struct task* current = get_current_task();
    file_type_t type;
    if (!current) return 0;
    if (fd < 0 || fd >= MAX_FDS) return 0;
    if (!current->fds[fd].in_use) return 0;
    type = fs_fd_type(&current->fds[fd]);
    if (type == FT_CONSOLE) return 1;
    return type == FT_CHARDEV && fs_fd_aux0(&current->fds[fd]) == 5U;
}

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

/* ---- termios 本体 (TCGETS/TCSETS) ----------------------------------------
 *
 * 元は x86 (kernel/sys_fs.c の g_console_termios, struct orth_termios
 * c_cc[20] 固定) と linux 側 (kernel/linux_syscall.c の
 * g_linux_console_termios, struct linux_termios c_line + c_cc[32]) に
 * 別々の状態と別々のレイアウトで複製されていた。
 *
 * ports/musl-install (各アーキ) 配下の include/bits/termios.h を
 * 3 アーキとも突き合わせると
 * musl の struct termios は c_line + c_cc[NCCS=32] で完全に同一 —— x86 の
 * c_cc[20] (c_line 無し) は「アーキごとの ABI 差」ではなく、x86 側が
 * musl の ABI とそもそも一致していなかっただけだった。初期値も両者で
 * 完全に一致していたので (VINTR=3 / VQUIT=28 / ... 含め)、値の選び直しは
 * 要らず、linux 側のレイアウトへ揃えるだけで統合できた
 * (別実装 29 組の 18 組目、ioctl の残り、2026-09-14)
 *
 * linux_console_is_intr_char / linux_console_deliver_intr /
 * arch_console_onlcr_enabled (kernel/linux_syscall.c) がフィールドを直接
 * 読むので non-static (sys_internal.h に extern 宣言) */
struct orth_termios g_console_termios = {
    .c_iflag = 0x00000002u,
    .c_oflag = 0x00000001u,
    .c_cflag = 0,
    .c_lflag = 0x00000001u | 0x00000002u | 0x00000008u,
    .c_cc = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26 },
    .c_ispeed = 115200,
    .c_ospeed = 115200,
};

int sys_tcgetattr(int fd, struct orth_termios* tio) {
    if (!tio) return -LINUX_EFAULT;
    if (!fd_is_console(fd)) return -LINUX_ENOTTY;
    *tio = g_console_termios;
    return 0;
}

int sys_tcsetattr(int fd, int optional_actions, const struct orth_termios* tio) {
    (void)optional_actions;
    if (!tio) return -LINUX_EFAULT;
    if (!fd_is_console(fd)) return -LINUX_ENOTTY;
    g_console_termios = *tio;
    return 0;
}

/* termios の ECHO (c_lflag bit3)。行編集 (busybox の lineedit) は raw モードに
 * して自前でエコーするので、ここが立っていないときにカーネルがエコーすると
 * 1 文字が 2 回出る。fs.c のコンソール読み取りが参照する。
 *
 * kernel/fs.c の weak デフォルト (常に有効) は元々 x86 だけが使っており、
 * termios を統合するのでここでも上書きする。ECHO ビットは 3 アーキとも
 * 同じ位置 (0x8) なので判定は 1 つで済む */
int arch_console_echo_enabled(void) {
    return (g_console_termios.c_lflag & 0x00000008u) != 0;
}
