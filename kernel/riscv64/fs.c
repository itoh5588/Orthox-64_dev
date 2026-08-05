#include <stddef.h>
#include <stdint.h>
#include "fs.h"
#include "pmm.h"
#include "riscv64/boot.h"
#include "riscv64/bootstrap_user.h"
#include "riscv64/errno.h"
#include "riscv64/syscall.h"
#include "sys_internal.h"
#include "task.h"
#include "vmm.h"
#include "xv6fs.h"

struct riscv64_linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
};

/* Linux AT_REMOVEDIR (unlinkat の第3引数) */
#define RISCV64_AT_REMOVEDIR 0x200

/* errno 定数は include/riscv64/errno.h (syscall.c と共有) */

/* 待ち手が「寝ている」かの判定。read/write は TASK_SLEEPING で寝るが、
 * ppoll は期限付きで待つため TASK_IO_WAIT になる。どちらも起こす対象 */
static int riscv64_fs_task_is_waiting(const struct task* t) {
    return t && (t->state == TASK_SLEEPING || t->state == TASK_IO_WAIT);
}

/* 待ち行列から取り出した面々を起こす。必ずロックの外で呼ぶこと
 * (task_wake は g_task_lock と IPI を伴う) */
static void riscv64_fs_wake_list(struct task** list, int n) {
    for (int i = 0; i < n; i++) {
        if (riscv64_fs_task_is_waiting(list[i])) task_wake(list[i]);
    }
}

/* O_ACCMODE 判定: 書き込み可能な開き方か */
static int riscv64_fs_flags_writable(int flags) {
    int acc = flags & 3;
    return acc == O_WRONLY || acc == O_RDWR;
}

/* 疑似キャラクタデバイス (xv6fs 上に inode を持たず、カーネルが直接応答する) */
#define RISCV64_DEV_NONE     0
#define RISCV64_DEV_NULL     1
#define RISCV64_DEV_ZERO     2
#define RISCV64_DEV_CONSOLE  3

static int riscv64_fs_path_eq(const char* a, const char* b);

static int riscv64_fs_special_dev(const char* resolved) {
    if (!resolved) return RISCV64_DEV_NONE;
    if (riscv64_fs_path_eq(resolved, "/dev/null")) return RISCV64_DEV_NULL;
    if (riscv64_fs_path_eq(resolved, "/dev/zero")) return RISCV64_DEV_ZERO;
    if (riscv64_fs_path_eq(resolved, "/dev/tty")) return RISCV64_DEV_CONSOLE;
    if (riscv64_fs_path_eq(resolved, "/dev/console")) return RISCV64_DEV_CONSOLE;
    return RISCV64_DEV_NONE;
}

static int riscv64_fs_path_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void riscv64_fs_strcpy(char* dst, const char* src, size_t size) {
    size_t i = 0;
    if (!dst || size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (src[i] && i + 1 < size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void riscv64_fs_kstat_defaults(struct kstat* st, uint32_t mode, int64_t size) {
    if (!st) return;
    st->dev = 0;
    st->ino = 0;
    st->mode = mode;
    st->uid = 0;
    st->gid = 0;
    st->nlink = 1;
    st->rdev = 0;
    st->size = size;
    st->atime_sec = 0;
    st->mtime_sec = 0;
    st->ctime_sec = 0;
}

static uint8_t riscv64_fs_dirent_type(uint32_t mode) {
    if ((mode & 0170000U) == KSTAT_MODE_DIR) return 4;
    if ((mode & 0170000U) == KSTAT_MODE_FILE) return 8;
    if ((mode & 0170000U) == KSTAT_MODE_CHR) return 2;
    return 0;
}

static int riscv64_fs_append_dirent(struct orth_dirent* dirents, size_t max_count, size_t* count,
                                    const char* name, uint32_t mode, uint32_t size) {
    size_t index = *count;
    size_t i = 0;
    if (!dirents || !count || !name || index >= max_count) return -1;
    dirents[index].mode = mode;
    dirents[index].size = size;
    while (name[i] && i + 1 < sizeof(dirents[index].name)) {
        dirents[index].name[i] = name[i];
        i++;
    }
    dirents[index].name[i] = '\0';
    *count = index + 1;
    return 0;
}

static int riscv64_fs_build_dirents(const char* resolved, struct orth_dirent* dirents, size_t max_count, size_t* out_count) {
    size_t count = 0;
    if (!resolved || !dirents || !out_count) return -1;
    if (xv6fs_is_mounted()) {
        return xv6fs_list_dir(resolved, dirents, max_count, out_count);
    }
    if (!riscv64_fs_path_eq(resolved, "/")) return -1;
    if (riscv64_fs_append_dirent(dirents, max_count, &count, ".", KSTAT_MODE_DIR | 0755U, 0) < 0) return -1;
    if (riscv64_fs_append_dirent(dirents, max_count, &count, "..", KSTAT_MODE_DIR | 0755U, 0) < 0) return -1;
    if (riscv64_fs_append_dirent(dirents, max_count, &count, "bootstrap-user", KSTAT_MODE_FILE | 0644U, 0) < 0) return -1;
    *out_count = count;
    return 0;
}

/* '.' と '..' をその場で畳んでパスを正規化する。
 *
 * **畳まないと文字列が実体より長いまま残る。** cwd と相対パスを単純連結すると
 *   /src/gcc-self/build/gcc + ../../gcc/ada/gcc-interface/ada-tree.def
 *   = 64 文字
 * になるが、file_descriptor_t の name[64] は 63 文字までしか保持できないので
 * 黙って切り捨てられ、別のパスとして扱われて I/O error になる。
 * 畳めば /src/gcc-self/gcc/ada/gcc-interface/ada-tree.def の 47 文字で収まる。
 * (Orthox 上で GCC のソースをコンパイルしたときに実際に踏んだ)
 *
 * xv6fs は '..' の dirent を辿れるので畳まなくても解決自体はできるが、
 * 長さのぶんだけ上限に当たりやすくなる。 */
static void riscv64_fs_normalize_path(char* p) {
    char* w = p;         /* 書き込み位置 */
    const char* r = p;   /* 読み取り位置 */

    if (!p) return;
    if (*r == '/') { *w++ = '/'; r++; }

    while (*r) {
        const char* seg;
        size_t len = 0;
        size_t k;

        while (*r == '/') r++;
        if (!*r) break;
        seg = r;
        while (*r && *r != '/') { r++; len++; }

        if (len == 1 && seg[0] == '.') continue;             /* "." は捨てる */
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {    /* ".." は 1 段戻る */
            while (w > p && w[-1] != '/') w--;                /* 直前の要素を消す */
            if (w > p + 1) w--;                               /* その前の '/' も (root は残す) */
            continue;
        }
        /* **区切りは要素の前に書くこと。** 後ろに書くと、最後の要素の直後に
         * '/' を書いた時点で読み取り側 (r) がまだ見ている終端 NUL を潰し、
         * その先の残骸を読み続けてしまう (/dev/null が /dev/null/-user になった)。
         * 前に書けば w は常に r より後ろに行かない。 */
        if (w > p && w[-1] != '/') *w++ = '/';
        for (k = 0; k < len; k++) *w++ = seg[k];
    }

    if (w == p) *w++ = '/';               /* 全部消えたら "/" */
    *w = '\0';
}

static int riscv64_fs_resolve_path(const char* path, char* out, size_t size) {
    struct task* current = get_current_task();
    size_t i = 0;
    size_t j = 0;

    if (!path || !out || size == 0) return -1;
    if (path[0] == '/') {
        while (path[j] && i + 1 < size) out[i++] = path[j++];
        if (path[j]) return -1;           /* 収まらなければ黙って切らずに失敗させる */
        out[i] = '\0';
        riscv64_fs_normalize_path(out);
        return 0;
    }
    if (!current || current->cwd[0] == '\0') return -1;
    while (current->cwd[j] && i + 1 < size) out[i++] = current->cwd[j++];
    if (current->cwd[j]) return -1;
    if (i > 0 && out[i - 1] != '/' && i + 1 < size) out[i++] = '/';
    j = 0;
    while (path[j] && i + 1 < size) out[i++] = path[j++];
    if (path[j]) return -1;
    out[i] = '\0';
    riscv64_fs_normalize_path(out);
    return 0;
}

static int riscv64_fs_resolve_dirfd_path(int dirfd, const char* path, char* out, size_t size) {
    struct task* current = get_current_task();
    size_t i = 0;
    size_t j = 0;

    if (!path || !out || size == 0) return -1;
    if (path[0] == '/') return riscv64_fs_resolve_path(path, out, size);
    if (dirfd == -100) return riscv64_fs_resolve_path(path, out, size);
    if (!current || dirfd < 0 || dirfd >= MAX_FDS || !current->fds[dirfd].in_use) return -1;
    if (current->fds[dirfd].type != FT_DIR || current->fds[dirfd].name[0] == '\0') return -1;

    while (current->fds[dirfd].name[j] && i + 1 < size) out[i++] = current->fds[dirfd].name[j++];
    if (i > 0 && out[i - 1] != '/' && i + 1 < size) out[i++] = '/';
    j = 0;
    while (path[j] && i + 1 < size) out[i++] = path[j++];
    out[i] = '\0';
    return 0;
}

int fs_get_file_data(const char* path, void** data, size_t* size) {
    if (riscv64_bootstrap_user_file_data(path, data, size) == 0) return 0;
    if (xv6fs_is_mounted()) {
        uint32_t xv6_mode = 0;
        uint64_t xv6_size = 0;
        char resolved[256];
        if (riscv64_fs_resolve_path(path, resolved, sizeof(resolved)) < 0) goto notfound;
        if (xv6fs_stat_path(resolved, &xv6_mode, &xv6_size, 0, 0) < 0) goto notfound;
        if ((xv6_mode & 0170000U) != KSTAT_MODE_FILE) goto notfound;
        if (data) {
            size_t npages = ((size_t)xv6_size + PAGE_SIZE - 1U) / PAGE_SIZE;
            struct xv6fs_inode* ip;
            void* buf;
            if (npages == 0) npages = 1;
            buf = PHYS_TO_VIRT(pmm_alloc((int)npages));
            if (!buf) goto notfound;
            ip = xv6fs_namei(resolved);
            if (!ip) {
                pmm_free((void*)VIRT_TO_PHYS((uint64_t)buf), (int)npages);
                goto notfound;
            }
            xv6fs_ilock(ip);
            xv6fs_readi(ip, buf, 0, (uint32_t)xv6_size);
            xv6fs_iunlock(ip);
            xv6fs_iput(ip);
            *data = buf;
        }
        if (size) *size = (size_t)xv6_size;
        return 0;
    }
notfound:
    if (data) *data = 0;
    if (size) *size = 0;
    return -1;
}

int sys_open(const char* path, int flags, int mode) {
    struct task* current = get_current_task();
    char resolved[256];
    void* file_data = 0;
    size_t file_size = 0;
    int fd = -1;

    if (!current || !path) return -RISCV64_EFAULT;
    if (riscv64_fs_resolve_path(path, resolved, sizeof(resolved)) < 0) return -RISCV64_ENOENT;

    // POSIX: open は最小の空き fd を返す。busybox ash はバックグラウンドジョブで
    // close(0) 後の open が必ず 0 を返すことに依存している (`cmd &` の stdin=/dev/null)
    for (int i = 0; i < MAX_FDS; i++) {
        if (!current->fds[i].in_use) {
            fd = i;
            break;
        }
    }
    if (fd < 0) return -RISCV64_EMFILE;

    {
        // 疑似キャラクタデバイスは xv6fs より先に解決する
        int dev = riscv64_fs_special_dev(resolved);
        if (dev != RISCV64_DEV_NONE) {
            if (dev == RISCV64_DEV_CONSOLE) {
                fs_init_console_fd(&current->fds[fd], flags);
            } else {
                current->fds[fd].type = FT_CHARDEV;
                current->fds[fd].data = 0;
                current->fds[fd].size = 0;
                current->fds[fd].offset = 0;
                current->fds[fd].in_use = 1;
                current->fds[fd].flags = flags;
                current->fds[fd].fd_flags = 0;
                current->fds[fd].aux0 = (uint32_t)dev;
                current->fds[fd].aux1 = 0;
            }
            riscv64_fs_strcpy(current->fds[fd].name, resolved, sizeof(current->fds[fd].name));
            return fd;
        }
    }

    {
        // ディレクトリ判定: O_DIRECTORY 指定、または xv6fs 上のディレクトリ
        int is_dir = (flags & O_DIRECTORY) != 0;
        uint32_t xv6_mode = 0;
        uint64_t xv6_size = 0;
        int have_xv6 = 0;
        if (xv6fs_is_mounted() && xv6fs_stat_path(resolved, &xv6_mode, &xv6_size, 0, 0) == 0) {
            have_xv6 = 1;
            if ((xv6_mode & 0170000U) == KSTAT_MODE_DIR) is_dir = 1;
        }
        if (!xv6fs_is_mounted() && riscv64_fs_path_eq(resolved, "/")) is_dir = 1;

        // O_CREAT: xv6fs 上に存在しなければ通常ファイルを新規作成する
        if (!have_xv6 && !is_dir && (flags & O_CREAT) != 0 && xv6fs_is_mounted()) {
            int create_mode = (mode & 07777) ? (mode & 07777) : 0644;
            if (xv6fs_create_file(resolved, create_mode, 0) == 0 &&
                xv6fs_stat_path(resolved, &xv6_mode, &xv6_size, 0, 0) == 0) {
                have_xv6 = 1;
                if ((xv6_mode & 0170000U) == KSTAT_MODE_DIR) is_dir = 1;
            }
        }

        if (is_dir) {
            /* ディレクトリの中身を open 時に丸ごと写し取る。
             *
             * 入りきらないと「一部だけ見える」状態になり、しかも何も言わない。
             * 4 ページ = 64 エントリ固定だった頃、GCC のビルドディレクトリ
             * (185 エントリ) で後から作った .o が列挙に出ず、`echo *.o` が
             * 展開されないという形で出た。個別の stat では見えるので、
             * 気付くまで遠回りした。
             *
             * 収まらなかったら諦めずにページ数を増やして取り直す。
             * count == cap は「ちょうど入った」と「溢れた」の区別が付かないので、
             * 安全側 (取り直し) に倒す。 */
            size_t pages = 4;
            size_t cap;
            void* dir_page;
            struct orth_dirent* dirents;
            size_t count = 0;

            /* 必要な大きさは先に決める。足りなければ捨てて取り直す、という
             * 形にすると、そのたびに全エントリの inode を読み直すことになり、
             * 185 エントリのディレクトリで `echo *.o` が返らなくなった。
             * ディレクトリの size をエントリ 1 個分で割れば個数の上限が出る。 */
            if (xv6fs_is_mounted()) {
                uint64_t dsize = 0;
                if (xv6fs_stat_path(resolved, 0, &dsize, 0, 0) == 0) {
                    size_t need = (size_t)(dsize / sizeof(struct xv6fs_dirent));
                    size_t want = (need * sizeof(struct orth_dirent) + PAGE_SIZE - 1) / PAGE_SIZE;
                    if (want + 1 > pages) pages = want + 1;
                }
            }
            if (pages > 512) return -1;   /* 2MB を超えるディレクトリは扱わない */

            cap = pages * PAGE_SIZE / sizeof(struct orth_dirent);
            dir_page = pmm_alloc(pages);
            if (!dir_page) return -1;
            dirents = (struct orth_dirent*)PHYS_TO_VIRT(dir_page);
            for (size_t i = 0; i < pages * PAGE_SIZE; i++) ((uint8_t*)dirents)[i] = 0;
            if (riscv64_fs_build_dirents(resolved, dirents, cap, &count) < 0) {
                pmm_free(dir_page, pages);
                return -1;
            }
            /* 見積もりを超えた = 一部しか見えていない。黙って切り捨てない */
            if (count >= cap) {
                pmm_free(dir_page, pages);
                return -1;
            }
            current->fds[fd].data = dirents;
            current->fds[fd].size = count * sizeof(struct orth_dirent);
            current->fds[fd].aux0 = (uint32_t)pages;
            current->fds[fd].type = FT_DIR;
            current->fds[fd].offset = 0;
            current->fds[fd].in_use = 1;
            current->fds[fd].flags = flags;
            current->fds[fd].fd_flags = 0;
            current->fds[fd].aux1 = 0;
            riscv64_fs_strcpy(current->fds[fd].name, resolved, sizeof(current->fds[fd].name));
            return fd;
        }

        if (have_xv6) {
            if ((xv6_mode & 0170000U) != KSTAT_MODE_FILE) return -1;
            // O_TRUNC は書き込み用に開いた場合のみ有効
            if ((flags & O_TRUNC) != 0 && riscv64_fs_flags_writable(flags) && xv6_size != 0) {
                if (xv6fs_truncate_file(resolved, 0) < 0) return -1;
                xv6_size = 0;
            }
            current->fds[fd].type = FT_XV6FS;
            current->fds[fd].data = 0;
            current->fds[fd].size = (size_t)xv6_size;
            // O_APPEND は末尾から書き始める (以後の write でも都度末尾へ)
            current->fds[fd].offset = (flags & O_APPEND) ? (size_t)xv6_size : 0;
            current->fds[fd].in_use = 1;
            current->fds[fd].flags = flags;
            current->fds[fd].fd_flags = 0;
            current->fds[fd].aux0 = 0;
            current->fds[fd].aux1 = 0;
            riscv64_fs_strcpy(current->fds[fd].name, resolved, sizeof(current->fds[fd].name));
            return fd;
        }
    }

    if (fs_get_file_data(resolved, &file_data, &file_size) < 0) return -RISCV64_ENOENT;

    current->fds[fd].type = FT_MODULE;
    current->fds[fd].data = file_data;
    current->fds[fd].size = file_size;
    current->fds[fd].offset = 0;
    current->fds[fd].in_use = 1;
    current->fds[fd].flags = flags;
    current->fds[fd].aux0 = 0;
    current->fds[fd].aux1 = 0;
    riscv64_fs_strcpy(current->fds[fd].name, resolved, sizeof(current->fds[fd].name));
    return fd;
}

int sys_openat(int dirfd, const char* path, int flags, int mode) {
    char resolved[256];
    if (riscv64_fs_resolve_dirfd_path(dirfd, path, resolved, sizeof(resolved)) < 0) return -RISCV64_ENOENT;
    return sys_open(resolved, flags, mode);
}

int64_t sys_write(int fd, const void* buf, size_t count) {
    struct task* current = get_current_task();
    const uint8_t* src = (const uint8_t*)buf;

    if (!current) return -RISCV64_EPERM;
    if (!buf) return -RISCV64_EFAULT;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -RISCV64_EBADF;
    if (current->fds[fd].type == FT_PIPE) {
        pipe_t* pipe = (pipe_t*)current->fds[fd].data;
        size_t written = 0;
        if (!pipe) return -1;
        while (written < count) {
            size_t space;
            size_t chunk;
            struct task* readers_to_wake[FS_WAITQ_MAX];
            int n_readers = 0;
            uint64_t irqf = spin_lock_irqsave(&pipe->lock);
            if (pipe->ref_count < 2) {
                spin_unlock_irqrestore(&pipe->lock, irqf);
                /* 読み手が閉じた: EPIPE 相当 */
                return written > 0 ? (int64_t)written : -32;
            }
            space = PIPE_BUF_SIZE - pipe->count;
            if (space == 0) {
                task_mark_sleeping(current);
                fs_waitq_add(&pipe->write_wq, current);
                spin_unlock_irqrestore(&pipe->lock, irqf);
                kernel_yield();
                irqf = spin_lock_irqsave(&pipe->lock);
                fs_waitq_remove(&pipe->write_wq, current);
                spin_unlock_irqrestore(&pipe->lock, irqf);
                continue;
            }
            chunk = count - written;
            if (chunk > space) chunk = space;
            for (size_t i = 0; i < chunk; i++) {
                pipe->buffer[pipe->write_pos] = (char)src[written + i];
                pipe->write_pos = (pipe->write_pos + 1) % PIPE_BUF_SIZE;
                pipe->count++;
            }
            written += chunk;
            n_readers = fs_waitq_take_all(&pipe->read_wq, readers_to_wake, FS_WAITQ_MAX);
            spin_unlock_irqrestore(&pipe->lock, irqf);
            riscv64_fs_wake_list(readers_to_wake, n_readers);
        }
        return (int64_t)written;
    }
    if (current->fds[fd].type == FT_CHARDEV) {
        /* /dev/null, /dev/zero: 書き込みは捨てる */
        return (int64_t)count;
    }
    if (current->fds[fd].type == FT_XV6FS) {
        file_descriptor_t* f = &current->fds[fd];
        size_t off;
        if (!riscv64_fs_flags_writable(f->flags)) return -9; /* EBADF */
        if (f->name[0] == '\0') return -RISCV64_EBADF;
        if (count == 0) return 0;
        if (f->flags & O_APPEND) {
            uint32_t xv6_mode = 0;
            uint64_t xv6_size = 0;
            if (xv6fs_stat_path(f->name, &xv6_mode, &xv6_size, 0, 0) == 0) {
                f->size = (size_t)xv6_size;
            }
            f->offset = f->size;
        }
        off = f->offset;
        if (xv6fs_write_file(f->name, (uint64_t)off, buf, count) < 0) return -RISCV64_ENOSPC;
        f->offset = off + count;
        if (f->offset > f->size) f->size = f->offset;
        return (int64_t)count;
    }
    if (current->fds[fd].type != FT_CONSOLE) return -RISCV64_EBADF;

    for (size_t i = 0; i < count; i++) {
        riscv64_uart_putchar((char)src[i]);
    }
    return (int64_t)count;
}

int64_t sys_read(int fd, void* buf, size_t count) {
    struct task* current = get_current_task();
    file_descriptor_t* f;
    size_t remaining;
    size_t to_read;

    if (!current) return -RISCV64_EPERM;
    if (!buf) return -RISCV64_EFAULT;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -RISCV64_EBADF;
    f = &current->fds[fd];
    if (f->type == FT_CONSOLE) {
        uint8_t* dst = (uint8_t*)buf;
        size_t read_count = 0;
        if (count == 0) return 0;
        while (read_count == 0) {
            /* 「空だから寝る」までを不可分にする。分けると、その隙に届いた
             * 文字で誰も起こしてくれず固まる (UART 割り込み化で顕在化する) */
            int got = riscv64_console_read_or_wait((char*)dst, (int)count, current);
            if (got <= 0) {
                kernel_yield();
                riscv64_console_clear_waiter(current);
                continue;
            }
            read_count = (size_t)got;
            /* termios の ECHO が落ちていれば何も出さない (raw モードの
             * 行編集は自前でエコーするので、ここで出すと二重になる) */
            if (fd == 0 && riscv64_console_echo_enabled()) {
                for (size_t i = 0; i < read_count; i++) {
                    int ch = dst[i];
                    if (ch == '\n') {
                        riscv64_uart_puts("\r\n");
                    } else if (ch == '\b' || ch == 0x7f) {
                        riscv64_uart_puts("\b \b");
                    } else {
                        riscv64_uart_putchar((char)ch);
                    }
                }
            }
        }
        return (int64_t)read_count;
    }
    if (f->type == FT_DIR) return -RISCV64_EISDIR;
    if (f->type == FT_CHARDEV) {
        if (f->aux0 == RISCV64_DEV_ZERO) {
            for (size_t i = 0; i < count; i++) ((uint8_t*)buf)[i] = 0;
            return (int64_t)count;
        }
        return 0; /* /dev/null は常に EOF */
    }
    if (f->type == FT_PIPE) {
        pipe_t* pipe = (pipe_t*)f->data;
        if (!pipe) return -1;
        for (;;) {
            size_t to_read;
            struct task* writers_to_wake[FS_WAITQ_MAX];
            int n_writers = 0;
            uint64_t irqf = spin_lock_irqsave(&pipe->lock);
            if (pipe->count == 0 && pipe->ref_count < 2) {
                spin_unlock_irqrestore(&pipe->lock, irqf);
                return 0; /* 書き込み側クローズ → EOF */
            }
            if (pipe->count == 0) {
                task_mark_sleeping(current);
                fs_waitq_add(&pipe->read_wq, current);
                spin_unlock_irqrestore(&pipe->lock, irqf);
                kernel_yield();
                irqf = spin_lock_irqsave(&pipe->lock);
                fs_waitq_remove(&pipe->read_wq, current);
                spin_unlock_irqrestore(&pipe->lock, irqf);
                continue;
            }
            to_read = (count > pipe->count) ? pipe->count : count;
            for (size_t i = 0; i < to_read; i++) {
                ((char*)buf)[i] = pipe->buffer[pipe->read_pos];
                pipe->read_pos = (pipe->read_pos + 1) % PIPE_BUF_SIZE;
                pipe->count--;
            }
            n_writers = fs_waitq_take_all(&pipe->write_wq, writers_to_wake, FS_WAITQ_MAX);
            spin_unlock_irqrestore(&pipe->lock, irqf);
            riscv64_fs_wake_list(writers_to_wake, n_writers);
            return (int64_t)to_read;
        }
    }
    if (f->type == FT_XV6FS) {
        struct xv6fs_inode* ip;
        int got;
        if (f->offset >= f->size) return 0;
        remaining = f->size - f->offset;
        to_read = (count > remaining) ? remaining : count;
        ip = xv6fs_namei(f->name);
        if (!ip) return -1;
        xv6fs_ilock(ip);
        got = xv6fs_readi(ip, buf, (uint32_t)f->offset, (uint32_t)to_read);
        xv6fs_iunlock(ip);
        xv6fs_iput(ip);
        if (got < 0) return -1;
        f->offset += (size_t)got;
        return (int64_t)got;
    }
    if (f->type != FT_MODULE) return -RISCV64_EBADF;
    if (f->offset >= f->size) return 0;

    remaining = f->size - f->offset;
    to_read = (count > remaining) ? remaining : count;
    for (size_t i = 0; i < to_read; i++) {
        ((uint8_t*)buf)[i] = ((const uint8_t*)f->data)[f->offset + i];
    }
    f->offset += to_read;
    return (int64_t)to_read;
}

int sys_stat(const char* path, struct kstat* st) {
    char resolved[256];
    void* file_data = 0;
    size_t file_size = 0;

    if (!path || !st) return -RISCV64_EFAULT;
    if (riscv64_fs_resolve_path(path, resolved, sizeof(resolved)) < 0) return -RISCV64_ENOENT;

    {
        int dev = riscv64_fs_special_dev(resolved);
        if (dev != RISCV64_DEV_NONE) {
            riscv64_fs_kstat_defaults(st, KSTAT_MODE_CHR | 0666U, 0);
            st->dev = 1;
            st->ino = 100U + (uint64_t)dev;
            st->rdev = 1;
            return 0;
        }
    }

    if (xv6fs_is_mounted()) {
        uint32_t xv6_mode = 0;
        uint64_t xv6_size = 0;
        uint32_t xv6_rdev = 0;
        if (xv6fs_stat_path(resolved, &xv6_mode, &xv6_size, 0, &xv6_rdev) == 0) {
            uint64_t xv6_ino = 0;
            riscv64_fs_kstat_defaults(st, xv6_mode, (int64_t)xv6_size);
            st->rdev = xv6_rdev;
            /* **本物の inode 番号を返すこと。** ここを定数 2 にしていたため
             * すべてのディレクトリが同じ ino になり、Orthox 上の gcc が
             * インクルードパスの重複判定 (dev+ino で比較する) で /include を
             * "duplicate" と見なして捨て、stdio.h が見つからなくなっていた。 */
            if (xv6fs_ino_path(resolved, &xv6_ino) == 0) st->ino = xv6_ino;
            return 0;
        }
        // xv6fs に無くても埋め込み /bootstrap-user は見せる
    }

    if (riscv64_fs_path_eq(resolved, "/")) {
        riscv64_fs_kstat_defaults(st, KSTAT_MODE_DIR | 0755U, 0);
        st->ino = 1;
        return 0;
    }

    /* 存在しないパスは必ず -ENOENT を返すこと。-1 のままだと musl 側で EPERM に
     * なり、`rm -f nonexistent` が "Operation not permitted" で失敗する
     * (busybox は lstat の ENOENT を見て -f を黙って成功扱いにする) */
    if (fs_get_file_data(resolved, &file_data, &file_size) < 0) return -RISCV64_ENOENT;
    riscv64_fs_kstat_defaults(st, KSTAT_MODE_FILE | 0644U, (int64_t)file_size);
    st->ino = ((uint64_t)(uintptr_t)file_data) >> 4;
    return 0;
}

int sys_fstat(int fd, struct kstat* st) {
    struct task* current = get_current_task();
    file_descriptor_t* f;

    if (!current) return -RISCV64_EPERM;
    if (!st) return -RISCV64_EFAULT;
    if (fd == 0 || fd == 1 || fd == 2) {
        riscv64_fs_kstat_defaults(st, KSTAT_MODE_CHR | 0666U, 0);
        st->dev = 1;
        st->ino = (uint64_t)fd + 1U;
        st->rdev = 1;
        return 0;
    }
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -RISCV64_EBADF;
    f = &current->fds[fd];

    if (f->type == FT_CONSOLE || f->type == FT_CHARDEV) {
        riscv64_fs_kstat_defaults(st, KSTAT_MODE_CHR | 0666U, 0);
        st->dev = 1;
        st->ino = (uint64_t)fd + 1U;
        st->rdev = 1;
        return 0;
    }
    if (f->type == FT_DIR) {
        riscv64_fs_kstat_defaults(st, KSTAT_MODE_DIR | 0755U, 0);
        st->ino = 1;
        return 0;
    }
    if ((f->type == FT_MODULE || f->type == FT_XV6FS) && f->name[0] != '\0') {
        return sys_stat(f->name, st);
    }
    return -RISCV64_EBADF;
}

int sys_fstatat(int dirfd, const char* path, struct kstat* st, int flags) {
    char resolved[256];
    (void)flags;
    if (riscv64_fs_resolve_dirfd_path(dirfd, path, resolved, sizeof(resolved)) < 0) return -RISCV64_ENOENT;
    return sys_stat(resolved, st);
}

int sys_chdir(const char* path) {
    struct task* current = get_current_task();
    struct kstat st;
    char resolved[256];

    if (!current || !path || path[0] == '\0') return -RISCV64_ENOENT;
    if (riscv64_fs_resolve_path(path, resolved, sizeof(resolved)) < 0) return -RISCV64_ENOENT;
    if (sys_stat(resolved, &st) < 0) return -RISCV64_ENOENT;
    if ((st.mode & 0170000U) != KSTAT_MODE_DIR) return -RISCV64_ENOTDIR;
    riscv64_fs_strcpy(current->cwd, resolved, sizeof(current->cwd));
    return 0;
}

int sys_fchdir(int fd) {
    struct task* current = get_current_task();
    file_descriptor_t* f;

    if (!current) return -RISCV64_EPERM;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -RISCV64_EBADF;
    f = &current->fds[fd];
    if (f->type != FT_DIR || f->name[0] == '\0') return -RISCV64_ENOTDIR;
    return sys_chdir(f->name);
}

/* ------------------------------------------------------------------ */
/* 書き込み系エントリポイント (すべて xv6fs のパスベース API に委譲)   */
/* ------------------------------------------------------------------ */

/* xv6fs のパス API は失敗を -1 でしか返さないので、事前に stat して errno を
 * 決める。`rm -f` は lstat の ENOENT を見て黙って成功扱いにするため、ここが
 * EPERM だと `rm -f nonexistent` が失敗する */
static int riscv64_fs_unlink_resolved(const char* resolved) {
    struct kstat st;
    if (sys_stat(resolved, &st) < 0) return -RISCV64_ENOENT;
    if ((st.mode & 0170000U) == KSTAT_MODE_DIR) return -RISCV64_EISDIR;
    return xv6fs_unlink_path(resolved) == 0 ? 0 : -RISCV64_EPERM;
}

static int riscv64_fs_rmdir_resolved(const char* resolved) {
    struct kstat st;
    if (sys_stat(resolved, &st) < 0) return -RISCV64_ENOENT;
    if ((st.mode & 0170000U) != KSTAT_MODE_DIR) return -RISCV64_ENOTDIR;
    /* xv6fs_rmdir_path が弾くのは実質「空でない」ケース */
    return xv6fs_rmdir_path(resolved) == 0 ? 0 : -RISCV64_ENOTEMPTY;
}

int sys_unlink(const char* path) {
    char resolved[256];
    if (!path || path[0] == '\0') return -RISCV64_ENOENT;
    if (!xv6fs_is_mounted()) return -RISCV64_EROFS;
    if (riscv64_fs_resolve_path(path, resolved, sizeof(resolved)) < 0) return -RISCV64_ENOENT;
    return riscv64_fs_unlink_resolved(resolved);
}

int sys_rmdir(const char* path) {
    char resolved[256];
    if (!path || path[0] == '\0') return -RISCV64_ENOENT;
    if (!xv6fs_is_mounted()) return -RISCV64_EROFS;
    if (riscv64_fs_resolve_path(path, resolved, sizeof(resolved)) < 0) return -RISCV64_ENOENT;
    return riscv64_fs_rmdir_resolved(resolved);
}

int sys_unlinkat(int dirfd, const char* path, int flags) {
    char resolved[256];
    if (!path || path[0] == '\0') return -RISCV64_ENOENT;
    if (!xv6fs_is_mounted()) return -RISCV64_EROFS;
    if (riscv64_fs_resolve_dirfd_path(dirfd, path, resolved, sizeof(resolved)) < 0) return -RISCV64_ENOENT;
    if (flags & RISCV64_AT_REMOVEDIR) return riscv64_fs_rmdir_resolved(resolved);
    return riscv64_fs_unlink_resolved(resolved);
}

int sys_link(const char* oldpath, const char* newpath) {
    char old_resolved[256];
    char new_resolved[256];
    struct kstat st;
    if (!oldpath || !newpath || oldpath[0] == '\0' || newpath[0] == '\0') return -RISCV64_ENOENT;
    if (!xv6fs_is_mounted()) return -RISCV64_EROFS;
    if (riscv64_fs_resolve_path(oldpath, old_resolved, sizeof(old_resolved)) < 0) return -RISCV64_ENOENT;
    if (riscv64_fs_resolve_path(newpath, new_resolved, sizeof(new_resolved)) < 0) return -RISCV64_ENOENT;
    if (sys_stat(new_resolved, &st) == 0) return -17; /* EEXIST */
    if (sys_stat(old_resolved, &st) < 0) return -RISCV64_ENOENT;
    return xv6fs_link_path(old_resolved, new_resolved) == 0 ? 0 : -RISCV64_EPERM;
}

int sys_linkat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath, int flags) {
    char old_resolved[256];
    char new_resolved[256];
    (void)flags; /* AT_SYMLINK_FOLLOW: xv6fs に symlink が無いので無視してよい */
    if (!oldpath || !newpath || oldpath[0] == '\0' || newpath[0] == '\0') return -RISCV64_ENOENT;
    if (riscv64_fs_resolve_dirfd_path(olddirfd, oldpath, old_resolved, sizeof(old_resolved)) < 0) return -RISCV64_ENOENT;
    if (riscv64_fs_resolve_dirfd_path(newdirfd, newpath, new_resolved, sizeof(new_resolved)) < 0) return -RISCV64_ENOENT;
    return sys_link(old_resolved, new_resolved);
}

int sys_mkdir(const char* path, int mode) {
    char resolved[256];
    struct kstat st;
    if (!path || path[0] == '\0') return -RISCV64_ENOENT;
    if (!xv6fs_is_mounted()) return -RISCV64_EROFS;
    if (riscv64_fs_resolve_path(path, resolved, sizeof(resolved)) < 0) return -RISCV64_ENOENT;
    if (sys_stat(resolved, &st) == 0) return -17; /* EEXIST */
    /* xv6fs_mkdir_path が弾くのは実質「親ディレクトリが無い」ケース */
    return xv6fs_mkdir_path(resolved, (mode & 07777) ? (mode & 07777) : 0755) == 0
               ? 0 : -RISCV64_ENOENT;
}

int sys_mkdirat(int dirfd, const char* path, int mode) {
    char resolved[256];
    if (!path || path[0] == '\0') return -RISCV64_ENOENT;
    if (riscv64_fs_resolve_dirfd_path(dirfd, path, resolved, sizeof(resolved)) < 0) return -RISCV64_ENOENT;
    return sys_mkdir(resolved, mode);
}

int sys_truncate(const char* path, uint64_t length) {
    char resolved[256];
    if (!path || path[0] == '\0') return -RISCV64_ENOENT;
    if (!xv6fs_is_mounted()) return -RISCV64_EROFS;
    if (riscv64_fs_resolve_path(path, resolved, sizeof(resolved)) < 0) return -RISCV64_ENOENT;
    return xv6fs_truncate_file(resolved, length) == 0 ? 0 : -RISCV64_ENOENT;
}

int sys_ftruncate(int fd, uint64_t length) {
    struct task* current = get_current_task();
    file_descriptor_t* f;

    if (!current) return -RISCV64_EPERM;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -RISCV64_EBADF;
    f = &current->fds[fd];
    if (f->type != FT_XV6FS || f->name[0] == '\0') return -RISCV64_EINVAL;
    if (!riscv64_fs_flags_writable(f->flags)) return -RISCV64_EBADF;
    if (xv6fs_truncate_file(f->name, length) < 0) return -RISCV64_EINVAL;
    f->size = (size_t)length;
    if (f->offset > f->size) f->offset = f->size;
    return 0;
}

int sys_chmod(const char* path, uint32_t mode) {
    char resolved[256];
    if (!path || path[0] == '\0') return -RISCV64_ENOENT;
    if (!xv6fs_is_mounted()) return -RISCV64_EROFS;
    if (riscv64_fs_resolve_path(path, resolved, sizeof(resolved)) < 0) return -RISCV64_ENOENT;
    return xv6fs_chmod_path(resolved, mode & 07777U) == 0 ? 0 : -RISCV64_ENOENT;
}

int sys_sync(void) {
    if (!xv6fs_is_mounted()) return 0;
    return xv6fs_sync();
}

void fs_release_fd(file_descriptor_t* desc) {
    struct task* readers_to_wake[FS_WAITQ_MAX];
    struct task* writers_to_wake[FS_WAITQ_MAX];
    int n_readers = 0;
    int n_writers = 0;
    int free_pipe = 0;

    if (!desc || !desc->in_use) return;

    if (desc->type == FT_PIPE) {
        pipe_t* pipe = (pipe_t*)desc->data;
        if (pipe) {
            uint64_t flags = spin_lock_irqsave(&pipe->lock);
            if (pipe->ref_count > 0) pipe->ref_count--;
            n_readers = fs_waitq_take_all(&pipe->read_wq, readers_to_wake, FS_WAITQ_MAX);
            n_writers = fs_waitq_take_all(&pipe->write_wq, writers_to_wake, FS_WAITQ_MAX);
            free_pipe = (pipe->ref_count == 0);
            spin_unlock_irqrestore(&pipe->lock, flags);
            riscv64_fs_wake_list(readers_to_wake, n_readers);
            riscv64_fs_wake_list(writers_to_wake, n_writers);
            if (free_pipe) {
                pmm_free((void*)VIRT_TO_PHYS((uint64_t)pipe), 1);
            }
        }
    } else if (desc->type == FT_DIR) {
        if (desc->data) {
            pmm_free((void*)VIRT_TO_PHYS((uint64_t)desc->data), (size_t)desc->aux0);
        }
    }

    desc->in_use = 0;
    desc->data = 0;
    desc->size = 0;
    desc->offset = 0;
    desc->fd_flags = 0;
    desc->name[0] = '\0';
}

int sys_close(int fd) {
    struct task* current = get_current_task();

    if (!current) return -RISCV64_EPERM;
    if (fd < 0 || fd >= MAX_FDS) return -RISCV64_EBADF;
    if (!current->fds[fd].in_use) return -RISCV64_EBADF;
    fs_release_fd(&current->fds[fd]);
    return 0;
}

int sys_pipe2(int* pipefd, int flags) {
    struct task* current = get_current_task();
    void* phys;
    pipe_t* pipe;
    int fd1 = -1;
    int fd2 = -1;

    if (!current) return -RISCV64_EPERM;
    if (!pipefd) return -RISCV64_EFAULT;
    /* O_CLOEXEC(0x80000)/O_NONBLOCK(0x800) 以外は未対応 */
    if (flags & ~(0x80000 | 0x800)) return -RISCV64_EINVAL;

    for (int i = 0; i < MAX_FDS; i++) {
        if (!current->fds[i].in_use) {
            if (fd1 == -1) fd1 = i;
            else { fd2 = i; break; }
        }
    }
    if (fd1 == -1 || fd2 == -1) return -RISCV64_EMFILE;

    phys = pmm_alloc(1);
    if (!phys) return -RISCV64_EMFILE;
    pipe = (pipe_t*)PHYS_TO_VIRT(phys);
    pipe->read_pos = 0;
    pipe->write_pos = 0;
    pipe->count = 0;
    pipe->ref_count = 2;
    fs_waitq_init(&pipe->read_wq);
    fs_waitq_init(&pipe->write_wq);
    spinlock_init(&pipe->lock);

    for (int end = 0; end < 2; end++) {
        int fd = end == 0 ? fd1 : fd2;
        current->fds[fd].type = FT_PIPE;
        current->fds[fd].data = pipe;
        current->fds[fd].size = 0;
        current->fds[fd].offset = 0;
        current->fds[fd].in_use = 1;
        current->fds[fd].flags = end == 0 ? O_RDONLY : O_WRONLY;
        current->fds[fd].fd_flags = (flags & 0x80000) ? FD_CLOEXEC : 0;
        current->fds[fd].aux0 = 0;
        current->fds[fd].aux1 = (uint32_t)end;
        current->fds[fd].name[0] = '\0';
    }
    pipefd[0] = fd1;
    pipefd[1] = fd2;
    return 0;
}

int sys_dup3(int oldfd, int newfd, int flags) {
    struct task* current = get_current_task();
    if (!current) return -RISCV64_EPERM;
    if (oldfd < 0 || oldfd >= MAX_FDS || !current->fds[oldfd].in_use) return -RISCV64_EBADF;
    if (newfd < 0 || newfd >= MAX_FDS) return -RISCV64_EBADF;
    if (oldfd == newfd) return newfd;
    if (current->fds[newfd].in_use) fs_release_fd(&current->fds[newfd]);
    if (fs_clone_fd(&current->fds[newfd], &current->fds[oldfd]) < 0) return -RISCV64_EBADF;
    current->fds[newfd].fd_flags = (flags & 0x80000) ? FD_CLOEXEC : 0;
    return newfd;
}

int sys_dup(int oldfd) {
    struct task* current = get_current_task();
    int newfd = -1;
    if (!current) return -RISCV64_EPERM;
    if (oldfd < 0 || oldfd >= MAX_FDS || !current->fds[oldfd].in_use) return -RISCV64_EBADF;
    for (int i = 0; i < MAX_FDS; i++) {
        if (!current->fds[i].in_use) { newfd = i; break; }
    }
    if (newfd == -1) return -RISCV64_EMFILE;
    if (fs_clone_fd(&current->fds[newfd], &current->fds[oldfd]) < 0) return -RISCV64_EBADF;
    current->fds[newfd].fd_flags = 0;
    return newfd;
}

/* fcntl コマンド (musl/Linux ABI) */
#define RISCV64_F_DUPFD          0
#define RISCV64_F_GETFD          1
#define RISCV64_F_SETFD          2
#define RISCV64_F_GETFL          3
#define RISCV64_F_SETFL          4
#define RISCV64_F_DUPFD_CLOEXEC  1030

int sys_fcntl(int fd, int cmd, uint64_t arg) {
    struct task* current = get_current_task();

    if (!current) return -RISCV64_EPERM;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -RISCV64_EBADF;

    switch (cmd) {
        case RISCV64_F_DUPFD:
        case RISCV64_F_DUPFD_CLOEXEC: {
            int minfd = (int)arg;
            if (minfd < 0 || minfd >= MAX_FDS) return -RISCV64_EINVAL;
            for (int newfd = minfd; newfd < MAX_FDS; newfd++) {
                if (current->fds[newfd].in_use) continue;
                if (fs_clone_fd(&current->fds[newfd], &current->fds[fd]) < 0) return -RISCV64_EBADF;
                current->fds[newfd].fd_flags =
                    (cmd == RISCV64_F_DUPFD_CLOEXEC) ? FD_CLOEXEC : 0;
                return newfd;
            }
            return -24; /* EMFILE */
        }
        case RISCV64_F_GETFD:
            return current->fds[fd].fd_flags;
        case RISCV64_F_SETFD:
            current->fds[fd].fd_flags = (int)arg & FD_CLOEXEC;
            return 0;
        case RISCV64_F_GETFL:
            return current->fds[fd].flags;
        case RISCV64_F_SETFL:
            /* アクセスモードは変更不可 (POSIX) */
            current->fds[fd].flags = (current->fds[fd].flags & 3) | ((int)arg & ~3);
            return 0;
        default:
            return -22; /* EINVAL */
    }
}

int fs_clone_fd(file_descriptor_t* dst, const file_descriptor_t* src) {
    if (!dst || !src) return -1;
    *dst = *src;
    if (src->in_use && src->type == FT_PIPE && src->data) {
        pipe_t* pipe = (pipe_t*)src->data;
        uint64_t flags = spin_lock_irqsave(&pipe->lock);
        pipe->ref_count++;
        spin_unlock_irqrestore(&pipe->lock, flags);
    }
    return 0;
}

void fs_close_cloexec_descriptors(struct task* task) {
    if (!task) return;
    for (int i = 0; i < MAX_FDS; i++) {
        if (task->fds[i].in_use && (task->fds[i].fd_flags & FD_CLOEXEC)) {
            fs_release_fd(&task->fds[i]);
        }
    }
}

int fs_init_console_fd(file_descriptor_t* fd, int flags) {
    if (!fd) return -1;
    fd->type = FT_CONSOLE;
    fd->data = 0;
    fd->size = 0;
    fd->offset = 0;
    fd->in_use = 1;
    fd->flags = flags;
    fd->fd_flags = 0;
    fd->aux0 = 0;
    fd->aux1 = 0;
    fd->name[0] = '\0';
    return 0;
}

// 埋め込み ELF は静的領域なので解放不要。xv6fs から読んだバッファは pmm 返却
void fs_free_exec_buffer(const char* path, void* data, size_t size) {
    void* embedded = 0;
    size_t embedded_size = 0;
    (void)path;
    if (!data || size == 0) return;
    if (riscv64_bootstrap_user_file_data("/bootstrap-user", &embedded, &embedded_size) == 0 &&
        data == embedded) {
        return;
    }
    {
        size_t npages = (size + PAGE_SIZE - 1U) / PAGE_SIZE;
        if (npages == 0) npages = 1;
        pmm_free((void*)VIRT_TO_PHYS((uint64_t)data), (int)npages);
    }
}

int sys_getdents(int fd, struct orth_dirent* dirp, size_t count) {
    struct task* current = get_current_task();
    file_descriptor_t* f;
    size_t remaining;
    size_t to_copy;

    if (!current) return -RISCV64_EPERM;
    if (!dirp) return -RISCV64_EFAULT;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -RISCV64_EBADF;
    f = &current->fds[fd];
    if (f->type != FT_DIR || !f->data) return -RISCV64_ENOTDIR;
    if (f->offset >= f->size) return 0;
    remaining = f->size - f->offset;
    to_copy = (count > remaining) ? remaining : count;
    for (size_t i = 0; i < to_copy; i++) {
        ((uint8_t*)dirp)[i] = ((uint8_t*)f->data)[f->offset + i];
    }
    f->offset += to_copy;
    return (int)to_copy;
}

int sys_getdents64(int fd, void* dirp, size_t count) {
    struct task* current = get_current_task();
    file_descriptor_t* f;
    struct orth_dirent* src;
    uint8_t* out = (uint8_t*)dirp;
    size_t out_used = 0;
    size_t index;

    if (!current) return -RISCV64_EPERM;
    if (!dirp) return -RISCV64_EFAULT;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -RISCV64_EBADF;
    f = &current->fds[fd];
    if (f->type != FT_DIR || !f->data) return -RISCV64_ENOTDIR;
    if (f->offset >= f->size) return 0;

    src = (struct orth_dirent*)f->data;
    index = f->offset / sizeof(struct orth_dirent);

    while ((index + 1) * sizeof(struct orth_dirent) <= f->size) {
        struct riscv64_linux_dirent64 ent;
        size_t name_len = 0;
        size_t reclen;
        size_t next_off;

        for (size_t i = 0; i < sizeof(ent); i++) ((uint8_t*)&ent)[i] = 0;
        while (src[index].name[name_len] && name_len + 1 < sizeof(ent.d_name)) {
            ent.d_name[name_len] = src[index].name[name_len];
            name_len++;
        }
        ent.d_name[name_len] = '\0';
        ent.d_ino = (uint64_t)index + 1;
        next_off = (index + 1) * sizeof(struct orth_dirent);
        ent.d_off = (int64_t)next_off;
        ent.d_type = riscv64_fs_dirent_type(src[index].mode);
        reclen = offsetof(struct riscv64_linux_dirent64, d_name) + name_len + 1;
        reclen = (reclen + 7U) & ~7U;
        ent.d_reclen = (uint16_t)reclen;

        if (out_used + reclen > count) break;
        for (size_t i = 0; i < reclen; i++) {
            out[out_used + i] = ((uint8_t*)&ent)[i];
        }
        out_used += reclen;
        index++;
    }

    f->offset = index * sizeof(struct orth_dirent);
    return (int)out_used;
}
