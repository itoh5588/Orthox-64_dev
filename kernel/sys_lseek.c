/* ---- lseek (3 アーキ共通) --------------------------------------------------
 *
 * **別実装 29 組のうちの 1 組を畳んだもの (2026-09-13)。**もとは
 *
 *     x86              kernel/fs.c            の fs_lseek (経由 sys_lseek)
 *     aarch64/riscv64  kernel/linux_syscall.c の linux_bootstrap_sys_lseek
 *
 * の 2 つがあり、**x86 側が単純化しすぎていた。**
 *
 * | | x86 (fs_lseek) | linux 側 (採用) |
 * |---|---|---|
 * | ディレクトリの fd  | 素通り (offset を動かせてしまう) | **ESPIPE** |
 * | EOF を越える SEEK_SET/CUR | 常に許す                | XV6FS/RAMFS の書き込み用 fd だけ許す。それ以外は EINVAL |
 * | コンソールの fd    | 何を渡しても 0 を返す (従来互換) | 特別扱いなし |
 *
 * **linux 側に寄せ、コンソールの互換だけ残した。**
 *
 *   - ESPIPE はディレクトリを lseek する呼び手を弾く POSIX の規約どおりの
 *     挙動で、x86 だけが漏れていた
 *   - EOF 越えのシークは「穴あき書き込み」のためだけに開けた抜け道
 *     (P-9、2026-08-29: /tmp を ramfs に置いたら `gcc -static` の `as` が
 *     `file truncated` で落ちた。セクションを置くために EOF より先へ
 *     seek してから書く動きを通すため)。**無制限に許すと、読み取り専用の
 *     fd や他の種類のファイルで意味の無い offset を作れてしまう**
 *   - コンソールは昔から「何を渡しても 0 を返す」という互換を持っていたので、
 *     ここだけは x86 の挙動を残した (linux 側は特別扱いが無く、0 以外への
 *     seek は EINVAL になりえた)
 *
 * arch_fs_refresh_size は riscv64 だけが実体を持つ (別々に open した fd が
 * 同じ xv6fs の inode を指すとき、サイズをキャッシュではなく inode から
 * 取り直すため)。x86 / aarch64 の kernel/fs.c は fd->file を共有ポインタで
 * 持つので、そこを経由するだけで常に最新の値になり、hook は空でよい。
 * ここでは弱いシンボルとして空実装を置き、riscv64 の強い実体が勝つように
 * した (x86 は元々この hook を定義していなかった)。 */

#include <stddef.h>
#include <stdint.h>
#include "fs.h"
#include "linux_errno.h"
#include "sys_internal.h"
#include "task.h"

void arch_fs_refresh_size(file_descriptor_t* f);
__attribute__((weak)) void arch_fs_refresh_size(file_descriptor_t* f) { (void)f; }

int64_t sys_lseek(int fd, int64_t offset, int whence) {
    struct task* current = get_current_task();
    file_descriptor_t* f;
    int64_t base;
    int64_t next;

    if (!current) return -LINUX_ESRCH;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -LINUX_EBADF;
    f = &current->fds[fd];

    /* コンソールは seek 不可だが従来互換で 0 を返す (fd 番号ではなく型で判定) */
    if (fs_fd_type(f) == FT_CONSOLE) return 0;
    if (fs_fd_type(f) == FT_DIR) return -LINUX_ESPIPE;

    /* offset / size は共有 open file description (fd->file) 側にある。
     * dup / fork した相方の書き込みがここに見えるのはそのため。別々に
     * open した fd は別の file を持つので、riscv64 は inode から取り直す */
    arch_fs_refresh_size(f);

    switch (whence) {
        case 0: base = 0; break;
        case 1: base = (int64_t)fs_fd_offset(f); break;
        case 2: base = (int64_t)fs_fd_size(f); break;
        default: return -LINUX_EINVAL;
    }

    next = base + offset;
    if (next < 0) return -LINUX_EINVAL;

    /* **書き込み用に開いたファイルは EOF 越えのシークを許す (穴あき書き込み)。**
     * ramfs の書き込みは ramfs_grow(off + count) で伸ばし、新しい領域を
     * 0 で埋めるので、穴は正しく 0 として読める。それ以外の種類・読み取り
     * 専用の fd では EOF を越えた offset を作らせない */
    if ((uint64_t)next > fs_fd_size(f) &&
        !((fs_fd_type(f) == FT_XV6FS || fs_fd_type(f) == FT_RAMFS) &&
          ((f->flags & 3) == O_WRONLY || (f->flags & 3) == O_RDWR))) {
        return -LINUX_EINVAL;
    }

    fs_fd_set_offset(f, (size_t)next);
    return next;
}
