#include "fs.h"
#include "kassert.h"
#include "task.h"
#include "limine.h"
#include "pmm.h"
#include "vmm.h"
#include "usb.h"
#include "lapic.h"
#include "net_socket.h"
#include "spinlock.h"
#include "storage.h"
#include "vfs.h"
#include "xv6fs.h"
#include "string.h"
#include "virtio_blk.h"

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 14        /* newlib-style (user/include) */
#define F_DUPFD_CLOEXEC_LINUX 1030 /* musl/Linux ABI (busybox, make, gcc) */
#define ORTH_O_ACCMODE 3
#define ORTH_AT_FDCWD (-100)
#define ORTH_LEGACY_AT_FDCWD (-2)
#define ORTH_AT_REMOVEDIR 0x200
#define ORTH_LINUX_O_CLOEXEC 0x80000
#define ORTH_LINUX_O_NONBLOCK 0x800

extern volatile struct limine_module_request module_request;
extern void puts(const char* s);
extern void puthex(uint64_t v);

/* O_CREAT/O_TRUNC/O_APPEND now come from fs.h with the correct Linux/musl
 * values; no separate ORTH_LINUX_* aliases are needed. */
#define ORTH_LEGACY_O_DIRECTORY 0x200000
/*
 * xv6fs log has 126 payload blocks. A worst-case allocating write logs roughly:
 * data blocks + bitmap block(s) + indirect metadata block(s) + inode.
 * 112 KiB leaves room for bitmap and triple-indirect metadata while restoring
 * most of the throughput lost by the earlier conservative 64 KiB cap.
 */
#define XV6FS_WRITE_CHUNK_MAX (112U * 1024U)
/*
 * syscall の戻り値は -errno 規約 (kernel/sys_fs.c が fs_* の戻り値をそのまま
 * ユーザーへ返し、user/syscalls.c が -4095..-1 を errno へ写す)。
 * 「よく分からない失敗はとりあえず -1」は -EPERM = "Operation not permitted"
 * としてユーザーに出るので、公開 fs_* からは返さないこと。
 * busybox は errno を見て挙動を変える箇所が多く (`rm -f` が典型)、
 * EPERM だと誤動作する。tests/x86_errno_smoke.sh が代表例を見張っている。
 *
 * 内部ヘルパ (FAT/USB/dirent 構築など) は「見つからない」を -1 で表す内部規約の
 * ままにしてあり、公開 fs_* の境界で errno に翻訳している。
 */
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef ESRCH
#define ESRCH 3
#endif
#ifndef EIO
#define EIO 5
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EACCES
#define EACCES 13
#endif
#ifndef EFAULT
#define EFAULT 14
#endif
#ifndef ENOTDIR
#define ENOTDIR 20
#endif
#ifndef ENFILE
#define ENFILE 23
#endif
#ifndef EMFILE
#define EMFILE 24
#endif
#ifndef ERANGE
#define ERANGE 34
#endif
#ifndef EBADF
#define EBADF 9
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EISDIR
#define EISDIR 21
#endif
#ifndef EFBIG
#define EFBIG 27
#endif
#ifndef EROFS
#define EROFS 30
#endif
#ifndef ENOSPC
#define ENOSPC 28
#endif
#ifndef EEXIST
#define EEXIST 17
#endif

static void pipe_wake_task(struct task* waiter) {
    if (!waiter) return;
    if (waiter->state == TASK_SLEEPING) {
        task_wake(waiter);
    }
}

static struct task* pipe_take_waiter_locked(struct task** waiter) {
    struct task* task;
    if (!waiter) return 0;
    task = *waiter;
    *waiter = 0;
    return task;
}

static void pipe_set_waiter(struct task** waiter, struct task* task) {
    if (!waiter) return;
    *waiter = task;
}

static void pipe_clear_waiter(struct task** waiter, struct task* task) {
    if (!waiter) return;
    if (*waiter == task) *waiter = 0;
}

static int fs_fd_has_private_copy(const file_descriptor_t* fd) {
    if (!fd) return 0;
    return fd->file == 0 && (fd->type == FT_DIR || fd->type == FT_XV6FS);
}

static fs_file_t* fs_alloc_file(void) {
    void* phys = pmm_alloc(1);
    fs_file_t* file;
    if (!phys) return 0;
    file = (fs_file_t*)PHYS_TO_VIRT(phys);
    memset(file, 0, sizeof(*file));
    file->ref_count = 1;
    return file;
}

static void fs_file_get(fs_file_t* file) {
    if (!file) return;
    file->ref_count++;
}

static void fs_file_put(fs_file_t* file) {
    if (!file) return;
    file->ref_count--;
    if (file->ref_count > 0) return;
    if (file->ops && file->ops->release) {
        file->ops->release(file);
    }
    pmm_free((void*)VIRT_TO_PHYS((uint64_t)file), 1);
}

static void fs_release_dir_file(fs_file_t* file) {
    if (!file) return;
    if (file->private_data && file->aux0) {
        pmm_free((void*)VIRT_TO_PHYS((uint64_t)file->private_data), (int)file->aux0);
    }
}

static void fs_release_xv6fs_file(fs_file_t* file) {
    if (!file) return;
    struct xv6fs_inode* ip = (struct xv6fs_inode*)file->private_data;
    if (ip) {
        xv6log_begin_op();
        xv6fs_iput(ip);
        xv6log_end_op();
        file->private_data = 0;
    }
}

static void fs_release_pipe_file(fs_file_t* file) {
    pipe_t* pipe;
    struct task* reader_to_wake;
    struct task* writer_to_wake;
    int free_pipe = 0;
    uint64_t flags;

    if (!file) return;
    pipe = (pipe_t*)file->private_data;
    if (!pipe) return;

    flags = spin_lock_irqsave(&pipe->lock);
    pipe->ref_count--;
    reader_to_wake = pipe_take_waiter_locked(&pipe->read_waiter);
    writer_to_wake = pipe_take_waiter_locked(&pipe->write_waiter);
    if (pipe->ref_count == 0) {
        free_pipe = 1;
    }
    spin_unlock_irqrestore(&pipe->lock, flags);

    pipe_wake_task(reader_to_wake);
    pipe_wake_task(writer_to_wake);
    if (free_pipe) {
        pmm_free((void*)VIRT_TO_PHYS((uint64_t)pipe), 1);
    }
}

static const fs_file_ops_t g_dir_file_ops = {
    .release = fs_release_dir_file,
};

static const fs_file_ops_t g_xv6fs_file_ops = {
    .release = fs_release_xv6fs_file,
};

static const fs_file_ops_t g_console_file_ops = {
    .release = 0,
};

static const fs_file_ops_t g_module_file_ops = {
    .release = 0,
};

static const fs_file_ops_t g_ramfs_file_ops = {
    .release = 0,
};

static const fs_file_ops_t g_pipe_file_ops = {
    .release = fs_release_pipe_file,
};

static const fs_file_ops_t g_usb_file_ops = {
    .release = 0,
};

/* Character devices backed by xv6fs T_DEVICE inodes.
 * major/minor follow the Linux convention:
 *   1:3 null, 1:5 zero, 1:8 random, 1:9 urandom, 5:0 tty, 5:1 console */
#define CHR_MAJOR_MEM     1U
#define CHR_MINOR_NULL    3U
#define CHR_MINOR_ZERO    5U
#define CHR_MINOR_RANDOM  8U
#define CHR_MINOR_URANDOM 9U
#define CHR_MAJOR_TTY     5U

static const fs_file_ops_t g_chardev_file_ops = {
    .release = 0,
};

/* ------------------------------------------------------------------ */
/* named pipe (FIFO) レジストリ                                        */
/*                                                                     */
/* xv6fs 上の T_FIFO inode を open すると、inum をキーに共有 pipe_t に  */
/* 接続する。fd は FT_PIPE として振る舞う (read/write/EOF は既存流用)。 */
/*   file->aux0: bit0=読み手 bit1=書き手                                */
/*   file->aux1: レジストリ index                                       */
/* pipe_t の ref_count は fifo file object 数と一致させる (レジストリは */
/* ref を持たない) ので、書き手全 close で読み手が EOF を得る既存条件    */
/* (count==0 && ref_count<2) が 1R+1W でそのまま成立する。              */
/* ------------------------------------------------------------------ */

#define FIFO_MAX 16

struct fifo_entry {
    int in_use;
    uint32_t inum;
    pipe_t* pipe;
    int readers;
    int writers;
    int opens;   /* この fifo を参照する file object 数 */
};

static struct fifo_entry g_fifo_table[FIFO_MAX];
static spinlock_t g_fifo_lock;
static struct wait_queue g_fifo_open_wait;   /* open ランデブー用 */

static void fs_release_fifo_file(fs_file_t* file) {
    uint64_t flags;
    if (!file) return;
    if (file->aux1 < FIFO_MAX) {
        struct fifo_entry* e = &g_fifo_table[file->aux1];
        flags = spin_lock_irqsave(&g_fifo_lock);
        if (file->aux0 & 1U) e->readers--;
        if (file->aux0 & 2U) e->writers--;
        e->opens--;
        if (e->opens <= 0) {
            e->in_use = 0;
            e->pipe = 0;
        }
        spin_unlock_irqrestore(&g_fifo_lock, flags);
    }
    /* O_RDWR open は ref_count を +2 しているので対称に余分の 1 を返す
     * (最後の 1 は fs_release_pipe_file が減らし、0 で解放する)。 */
    if ((file->aux0 & 3U) == 3U) {
        pipe_t* pipe = (pipe_t*)file->private_data;
        if (pipe) {
            flags = spin_lock_irqsave(&pipe->lock);
            pipe->ref_count--;
            spin_unlock_irqrestore(&pipe->lock, flags);
        }
    }
    fs_release_pipe_file(file);       /* pipe ref--, waiter 起床, 0 で解放 */
    wake_up_all(&g_fifo_open_wait);   /* ランデブー待ちへ状態変化を通知 */
}

static const fs_file_ops_t g_fifo_file_ops = {
    .release = fs_release_fifo_file,
};

int fs_init_console_fd(file_descriptor_t* fd, int flags) {
    fs_file_t* file;
    if (!fd) return -1;
    memset(fd, 0, sizeof(*fd));
    file = fs_alloc_file();
    if (!file) return -1;
    file->type = FT_CONSOLE;
    file->size = 0;
    file->offset = 0;
    file->ops = &g_console_file_ops;
    fd->type = FT_CONSOLE;
    fd->file = file;
    fd->in_use = 1;
    fd->flags = flags;
    return 0;
}

file_type_t fs_fd_type(const file_descriptor_t* fd) {
    if (!fd) return FT_UNUSED;
    return fd->file ? fd->file->type : fd->type;
}

void* fs_fd_data(const file_descriptor_t* fd) {
    if (!fd) return 0;
    return fd->file ? fd->file->private_data : fd->data;
}

size_t fs_fd_size(const file_descriptor_t* fd) {
    if (!fd) return 0;
    return fd->file ? fd->file->size : fd->size;
}

size_t fs_fd_offset(const file_descriptor_t* fd) {
    if (!fd) return 0;
    return fd->file ? fd->file->offset : fd->offset;
}

void fs_fd_set_offset(file_descriptor_t* fd, size_t offset) {
    if (!fd) return;
    if (fd->file) {
        fd->file->offset = offset;
    } else {
        fd->offset = offset;
    }
}

void fs_fd_set_size(file_descriptor_t* fd, size_t size) {
    if (!fd) return;
    if (fd->file) {
        fd->file->size = size;
    } else {
        fd->size = size;
    }
}

uint32_t fs_fd_aux0(const file_descriptor_t* fd) {
    if (!fd) return 0;
    return fd->file ? fd->file->aux0 : fd->aux0;
}

uint32_t fs_fd_aux1(const file_descriptor_t* fd) {
    if (!fd) return 0;
    return fd->file ? fd->file->aux1 : fd->aux1;
}

const char* fs_fd_name(const file_descriptor_t* fd) {
    if (!fd) return "";
    if (fd->file && fd->file->path[0]) return fd->file->path;
    return fd->name;
}

static void fs_assert_open_file_consistent(const file_descriptor_t* fd) {
    if (!fd || !fd->in_use || !fd->file) return;
    KASSERT(fd->file->ref_count > 0);
    KASSERT(fd->file->type == fd->type);
}

static void* fs_fd_data_required(const file_descriptor_t* fd, file_type_t type) {
    void* data;
    KASSERT(fd != 0);
    KASSERT(fd->in_use);
    fs_assert_open_file_consistent(fd);
    KASSERT(fs_fd_type(fd) == type);
    data = fs_fd_data(fd);
    KASSERT(data != 0);
    return data;
}

void fs_release_fd(file_descriptor_t* fd) {
    if (!fd || !fd->in_use) return;

    fs_assert_open_file_consistent(fd);
    if (fd->file) {
        fs_file_put(fd->file);
        fd->file = 0;
    } else {
        switch (fd->type) {
            case FT_DIR:
            case FT_XV6FS:
                if (fd->data && fd->aux0) {
                    pmm_free((void*)VIRT_TO_PHYS((uint64_t)fd->data), (int)fd->aux0);
                }
                break;
            case FT_UNUSED:
            case FT_CONSOLE:
            case FT_MODULE:
            case FT_RAMFS:
            case FT_PIPE:
            case FT_SOCKET:
            case FT_USB:
            default:
                break;
        }
    }

    fd->in_use = 0;
    fd->type = FT_UNUSED;
    fd->data = NULL;
    fd->size = 0;
    fd->offset = 0;
    fd->flags = 0;
    fd->fd_flags = 0;
    fd->aux0 = 0;
    fd->aux1 = 0;
    fd->name[0] = '\0';
}

int fs_clone_fd(file_descriptor_t* dst, const file_descriptor_t* src) {
    void* phys;
    size_t pages;

    if (!dst || !src) return -1;
    *dst = *src;
    if (!src->in_use) return 0;

    if (src->file) {
        fs_file_get(src->file);
        return 0;
    }

    if (fs_fd_has_private_copy(src) && src->data && src->aux0) {
        pages = src->aux0;
        phys = pmm_alloc((int)pages);
        if (!phys) {
            memset(dst, 0, sizeof(*dst));
            return -1;
        }
        dst->data = PHYS_TO_VIRT(phys);
        memcpy(dst->data, src->data, pages * PAGE_SIZE);
    }

    return 0;
}

int fs_dup_fd(file_descriptor_t* dst, const file_descriptor_t* src) {
    if (fs_clone_fd(dst, src) < 0) return -1;
    dst->fd_flags = 0;
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

static int strcmp_exact(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        if (*s1 != *s2) return 0;
        s1++; s2++;
    }
    return *s1 == *s2;
}

static int strcmp_suffix_fs(const char* full_path, const char* suffix) {
    int len_f = 0; while(full_path[len_f]) len_f++;
    int len_s = 0; while(suffix[len_s]) len_s++;
    if (len_f < len_s) return 0;
    for (int i = 0; i < len_s; i++) {
        if (full_path[len_f - len_s + i] != suffix[i]) return 0;
    }
    return 1;
}

static const char* normalize_fs_path(const char* path) {
    /* xv6fs は絶対パス ('/') 必須。先頭スラッシュは保持する。
     * './' プレフィックスのみ除去（resolve_task_path 未経由パス向け）。 */
    if (path[0] == '.' && path[1] == '/') {
        path += 2;
        while (*path == '/') path++;
    }
    return path;
}

static void canonicalize_path_inplace(char* path) {
    char tmp[256];
    size_t ti = 0;
    size_t i = 0;

    if (!path) return;
    if (path[0] != '/') {
        tmp[ti++] = '/';
    }

    while (path[i] && ti + 1 < sizeof(tmp)) {
        while (path[i] == '/') i++;
        if (!path[i]) break;

        if (path[i] == '.' && (path[i + 1] == '/' || path[i + 1] == '\0')) {
            i += (path[i + 1] == '/') ? 2 : 1;
            continue;
        }
        if (path[i] == '.' && path[i + 1] == '.' && (path[i + 2] == '/' || path[i + 2] == '\0')) {
            i += (path[i + 2] == '/') ? 3 : 2;
            if (ti > 1) {
                ti--;
                while (ti > 1 && tmp[ti - 1] != '/') ti--;
            }
            continue;
        }

        if (ti == 0 || tmp[ti - 1] != '/') tmp[ti++] = '/';
        while (path[i] && path[i] != '/' && ti + 1 < sizeof(tmp)) {
            tmp[ti++] = path[i++];
        }
    }

    if (ti == 0) tmp[ti++] = '/';
    tmp[ti] = '\0';
    i = 0;
    while (tmp[i]) {
        path[i] = tmp[i];
        i++;
    }
    path[i] = '\0';
}

static int starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++;
        prefix++;
    }
    return 1;
}

static int contains_char(const char* s, char c) {
    if (!s) return 0;
    while (*s) {
        if (*s == c) return 1;
        s++;
    }
    return 0;
}

static void resolve_task_path(const char* path, char* out, size_t size) {
    struct task* current = get_current_task();
    size_t i = 0;
    size_t j = 0;
    if (!out || size == 0) return;
    if (!path || path[0] == '\0') {
        out[0] = '\0';
        return;
    }
    if (path[0] == '/' || !current || current->cwd[0] == '\0') {
        while (path[j] && i + 1 < size) out[i++] = path[j++];
        out[i] = '\0';
        canonicalize_path_inplace(out);
        return;
    }
    if (current->cwd[0] == '/') {
        out[i++] = '/';
        j = 1;
    }
    while (current->cwd[j] && i + 1 < size) out[i++] = current->cwd[j++];
    if (i > 0 && out[i - 1] != '/' && i + 1 < size) out[i++] = '/';
    j = 0;
    while (path[j] && i + 1 < size) out[i++] = path[j++];
    out[i] = '\0';
    canonicalize_path_inplace(out);
}

static int resolve_dirfd_path(int dirfd, const char* path, char* out, size_t size) {
    struct task* current = get_current_task();
    const char* base = 0;
    size_t i = 0;
    size_t j = 0;
    if (!out || size == 0 || !path) return -1;
    if (path[0] == '/') {
        resolve_task_path(path, out, size);
        return 0;
    }
    if (dirfd == ORTH_AT_FDCWD || dirfd == ORTH_LEGACY_AT_FDCWD) {
        resolve_task_path(path, out, size);
        return 0;
    }
    if (!current || dirfd < 0 || dirfd >= MAX_FDS || !current->fds[dirfd].in_use) {
        return -1;
    }
    if (fs_fd_type(&current->fds[dirfd]) != FT_DIR || fs_fd_name(&current->fds[dirfd])[0] == '\0') {
        return -1;
    }
    base = fs_fd_name(&current->fds[dirfd]);
    while (base[j] && i + 1 < size) out[i++] = base[j++];
    if (i > 0 && out[i - 1] != '/' && i + 1 < size) out[i++] = '/';
    j = 0;
    while (path[j] && i + 1 < size) out[i++] = path[j++];
    out[i] = '\0';
    canonicalize_path_inplace(out);
    return 0;
}

struct fat_boot_info {
    uint32_t part_start;
    uint32_t part_sectors;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fats;
    uint32_t fat_size;
    uint32_t root_cluster;
    uint32_t data_start_lba;
};

struct fat_dir_entry_info {
    char name[256];
    uint8_t attr;
    uint32_t first_cluster;
    uint32_t size;
};

struct fat_lfn_state {
    char name[256];
    int valid;
};

#define USB_READ_RETRIES 3

static int usb_read_blocks_safe(uint32_t lba, void* buf, uint32_t count) {
    uint8_t* dst = (uint8_t*)buf;
    for (int attempt = 0; attempt < USB_READ_RETRIES; attempt++) {
        if (usb_read_block(lba, dst, count) == 0) return 0;
    }
    for (uint32_t done = 0; done < count; done++) {
        for (int attempt = 0; attempt < USB_READ_RETRIES; attempt++) {
            if (usb_read_block(lba + done, dst + done * 512U, 1) == 0) break;
            if (attempt == USB_READ_RETRIES - 1) return -1;
        }
    }
    return 0;
}

static void fat_copy_entry(struct fat_dir_entry_info* dst, const struct fat_dir_entry_info* src) {
    if (!dst || !src) return;
    for (int i = 0; i < (int)sizeof(dst->name); i++) {
        dst->name[i] = src->name[i];
        if (src->name[i] == '\0') break;
    }
    dst->name[sizeof(dst->name) - 1] = '\0';
    dst->attr = src->attr;
    dst->first_cluster = src->first_cluster;
    dst->size = src->size;
}

static uint32_t read_le32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void format_83_name(const uint8_t* e, char* name) {
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) name[n++] = (char)e[i];
    if (e[8] != ' ') {
        name[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) name[n++] = (char)e[i];
    }
    name[n] = '\0';
}

static void fat_lfn_reset(struct fat_lfn_state* st) {
    if (!st) return;
    st->name[0] = '\0';
    st->valid = 0;
}

static void fat_lfn_append_chunk(struct fat_lfn_state* st, const uint8_t* e) {
    static const uint8_t offsets[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    int order;
    int base;
    if (!st || !e) return;
    order = (e[0] & 0x1F);
    if (order <= 0) return;
    if (e[0] & 0x40) {
        fat_lfn_reset(st);
        st->valid = 1;
    }
    if (!st->valid) return;
    base = (order - 1) * 13;
    if (base < 0 || base >= (int)sizeof(st->name)) {
        fat_lfn_reset(st);
        return;
    }
    for (int i = 0; i < 13 && (base + i) < (int)sizeof(st->name) - 1; i++) {
        uint16_t ch = read_le16(&e[offsets[i]]);
        if (ch == 0x0000 || ch == 0xFFFF) {
            st->name[base + i] = '\0';
            return;
        }
        st->name[base + i] = (ch < 0x80) ? (char)ch : '?';
    }
}

static void fat_decode_entry(const uint8_t* e, struct fat_lfn_state* lfn, struct fat_dir_entry_info* ent) {
    ent->attr = e[11];
    format_83_name(e, ent->name);
    if (lfn && lfn->valid && lfn->name[0] != '\0') {
        for (int i = 0; i < (int)sizeof(ent->name) - 1; i++) {
            ent->name[i] = lfn->name[i];
            if (lfn->name[i] == '\0') break;
        }
        ent->name[sizeof(ent->name) - 1] = '\0';
    }
    ent->first_cluster = ((uint32_t)read_le16(&e[20]) << 16) | read_le16(&e[26]);
    ent->size = read_le32(&e[28]);
    if (lfn) fat_lfn_reset(lfn);
}

static int usb_get_first_partition(uint32_t* start, uint32_t* sectors) {
    uint8_t sector[512];
    for (int attempt = 0; attempt < USB_READ_RETRIES; attempt++) {
        if (usb_read_blocks_safe(0, sector, 1) == 0) break;
        if (attempt == USB_READ_RETRIES - 1) return -1;
    }
    *start = read_le32(&sector[446 + 8]);
    *sectors = read_le32(&sector[446 + 12]);
    return (*start && *sectors) ? 0 : -1;
}

static int usb_load_fat_boot(struct fat_boot_info* info) {
    uint8_t sector[512];
    if (!info) return -1;
    if (usb_get_first_partition(&info->part_start, &info->part_sectors) < 0) return -1;
    for (int attempt = 0; attempt < USB_READ_RETRIES; attempt++) {
        if (usb_read_blocks_safe(info->part_start, sector, 1) == 0) break;
        if (attempt == USB_READ_RETRIES - 1) return -1;
    }
    if (sector[510] != 0x55 || sector[511] != 0xAA) return -1;
    info->bytes_per_sector = read_le16(&sector[11]);
    info->sectors_per_cluster = sector[13];
    info->reserved_sectors = read_le16(&sector[14]);
    info->fats = sector[16];
    info->fat_size = read_le32(&sector[36]);
    info->root_cluster = read_le32(&sector[44]);
    if (info->bytes_per_sector != 512 || info->sectors_per_cluster == 0) return -1;
    info->data_start_lba = info->part_start + info->reserved_sectors + (uint32_t)info->fats * info->fat_size;
    return 0;
}

static uint32_t fat_cluster_to_lba(const struct fat_boot_info* info, uint32_t cluster) {
    return info->data_start_lba + (cluster - 2U) * (uint32_t)info->sectors_per_cluster;
}

static uint32_t fat_read_next_cluster(const struct fat_boot_info* info, uint32_t cluster) {
    static uint32_t cached_lba = 0xFFFFFFFFU;
    static uint8_t cached_sector[512];
    static int cache_valid = 0;
    uint8_t sector[512];
    uint32_t fat_offset = cluster * 4U;
    uint32_t fat_lba = info->part_start + info->reserved_sectors + (fat_offset / 512U);
    uint32_t ent_off = fat_offset % 512U;
    if (!cache_valid || cached_lba != fat_lba) {
        if (usb_read_blocks_safe(fat_lba, sector, 1) < 0) return 0x0FFFFFFFU;
        for (int i = 0; i < 512; i++) cached_sector[i] = sector[i];
        cached_lba = fat_lba;
        cache_valid = 1;
    }
    return read_le32(&cached_sector[ent_off]) & 0x0FFFFFFFU;
}

static int fat_read_dir_entry(const struct fat_boot_info* info, uint32_t dir_cluster,
                              const char* target, struct fat_dir_entry_info* out) {
    uint8_t sector[4096];
    uint32_t cluster = dir_cluster;
    struct fat_lfn_state lfn;
    if (!info || !target || !out || cluster < 2) return -1;
    fat_lfn_reset(&lfn);
    while (cluster >= 2 && cluster < 0x0FFFFFF8U) {
        uint32_t lba = fat_cluster_to_lba(info, cluster);
        uint32_t total = (uint32_t)info->bytes_per_sector * info->sectors_per_cluster;
        if (total > sizeof(sector)) return -1;
        if (usb_read_blocks_safe(lba, sector, info->sectors_per_cluster) < 0) return -1;
        for (uint32_t off = 0; off < total; off += 32) {
            uint8_t* e = &sector[off];
            if (e[0] == 0x00) return -1;
            if (e[0] == 0xE5) {
                fat_lfn_reset(&lfn);
                continue;
            }
            if (e[11] == 0x0F) {
                fat_lfn_append_chunk(&lfn, e);
                continue;
            }
            fat_decode_entry(e, &lfn, out);
            if (!strcmp_exact(out->name, target)) continue;
            return 0;
        }
        cluster = fat_read_next_cluster(info, cluster);
    }
    return -1;
}

static int fat_resolve_path(const char* path, struct fat_dir_entry_info* out) {
    struct fat_boot_info info;
    char path_buf[128];
    char* p;
    uint32_t dir_cluster;
    struct fat_dir_entry_info ent;
    if (!path || !out) return -1;
    while (*path == '/') path++;
    if (!starts_with(path, "usb/")) return -1;
    path += 4;
    if (*path == '\0') return -1;
    if (usb_load_fat_boot(&info) < 0) return -1;
    for (int i = 0; i < 127; i++) {
        path_buf[i] = path[i];
        if (path[i] == '\0') break;
    }
    path_buf[127] = '\0';
    dir_cluster = info.root_cluster;
    p = path_buf;
    while (*p == '/') p++;
    while (*p) {
        char* part = p;
        while (*p && *p != '/') p++;
        if (*p) {
            *p = '\0';
            p++;
            while (*p == '/') p++;
        }
        if (fat_read_dir_entry(&info, dir_cluster, part, &ent) < 0) return -1;
        dir_cluster = ent.first_cluster;
        fat_copy_entry(out, &ent);
    }
    return 0;
}

#define MAX_RAMFS_FILES 256

struct ramfs_file {
    char name[64];
    uint8_t* data;
    size_t size;
    size_t capacity;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint64_t atime_sec;
    uint64_t mtime_sec;
    uint64_t ctime_sec;
    int in_use;
};

static struct ramfs_file ramfs_table[MAX_RAMFS_FILES];

enum root_source_type {
    ROOT_SOURCE_MODULE = 0,
    ROOT_SOURCE_XV6FS  = 1,
};

#define DIR_BUF_PAGES 64
#define DIR_BUF_BYTES (DIR_BUF_PAGES * PAGE_SIZE)

static enum root_source_type g_root_source = ROOT_SOURCE_MODULE;

static uint64_t fs_now_sec(void) {
    return lapic_get_ticks_ms() / 1000U;
}

enum {
    FS_DEV_CONSOLE = 1,
    FS_DEV_PIPE = 2,
    FS_DEV_RAMFS = 3,
    FS_DEV_ROOT_ARCHIVE = 4,
    FS_DEV_MODULE = 5,
    FS_DEV_USB_FAT = 6,
    FS_DEV_SYNTH = 7,
};

static uint64_t fs_hash_name(const char* s) {
    uint64_t h = 1469598103934665603ULL;
    if (!s) return 0;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

static uint32_t fs_default_mode_for_type(uint32_t type_bits) {
    if ((type_bits & 0170000U) == KSTAT_MODE_DIR) return KSTAT_MODE_DIR | 0555U;
    if ((type_bits & 0170000U) == KSTAT_MODE_CHR) return KSTAT_MODE_CHR | 0666U;
    return KSTAT_MODE_FILE | 0444U;
}

static void kstat_set_defaults(struct kstat* st, uint32_t mode, int64_t size) {
    if (!st) return;
    st->dev = 0;
    st->ino = 0;
    st->nlink = ((mode & 0170000U) == KSTAT_MODE_DIR) ? 2U : 1U;
    st->mode = mode;
    st->uid = 0;
    st->gid = 0;
    st->pad0 = 0;
    st->rdev = 0;
    st->size = size;
    st->blksize = 512;
    st->blocks = (size > 0) ? ((size + 511) / 512) : 0;
    st->atime_sec = 0;
    st->atime_nsec = 0;
    st->mtime_sec = 0;
    st->mtime_nsec = 0;
    st->ctime_sec = 0;
    st->ctime_nsec = 0;
    st->unused[0] = 0;
    st->unused[1] = 0;
    st->unused[2] = 0;
}

static void kstat_from_ramfs(struct kstat* st, const struct ramfs_file* rf) {
    if (!st || !rf) return;
    st->dev = FS_DEV_RAMFS;
    st->ino = fs_hash_name(rf->name);
    st->mode = rf->mode;
    st->uid = rf->uid;
    st->gid = rf->gid;
    st->nlink = rf->nlink;
    st->pad0 = 0;
    st->rdev = 0;
    st->size = (int64_t)rf->size;
    st->blksize = 512;
    st->blocks = (rf->size + 511U) / 512U;
    st->atime_sec = (int64_t)rf->atime_sec;
    st->atime_nsec = 0;
    st->mtime_sec = (int64_t)rf->mtime_sec;
    st->mtime_nsec = 0;
    st->ctime_sec = (int64_t)rf->ctime_sec;
    st->ctime_nsec = 0;
    st->unused[0] = 0;
    st->unused[1] = 0;
    st->unused[2] = 0;
}

static int fs_try_active_root_lookup(const char* path, void** data, size_t* size, uint32_t* mode);
static int fs_try_active_root_stat(const char* path, struct kstat* st);
static int fs_stat_normalized_path(const char* path, struct kstat* st);

static int fs_is_mount_dir(const char* path) {
    const char* subpath = 0;
    return vfs_find_mountpoint(path, &subpath) && subpath && *subpath == '\0';
}

static int dirent_name_exists(const struct orth_dirent* dirents, size_t count, const char* name) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp_exact(dirents[i].name, name)) return 1;
    }
    return 0;
}

static int dirent_append(struct orth_dirent* dirents, size_t max_entries, size_t* count,
                         const char* name, uint32_t mode, uint32_t size) {
    size_t idx;
    if (!dirents || !count || !name || name[0] == '\0') return -1;
    if (dirent_name_exists(dirents, *count, name)) return 0;
    if (*count >= max_entries) return -1;
    idx = *count;
    dirents[idx].mode = mode;
    dirents[idx].size = size;
    for (int i = 0; i < (int)sizeof(dirents[idx].name) - 1; i++) {
        dirents[idx].name[i] = name[i];
        if (name[i] == '\0') break;
    }
    dirents[idx].name[sizeof(dirents[idx].name) - 1] = '\0';
    (*count)++;
    return 0;
}

static int build_ramfs_subdir_dirents(const char* path, struct orth_dirent* dirents,
                                      size_t max_entries, size_t* out_count) {
    const char* norm = normalize_fs_path(path);
    size_t count = out_count ? *out_count : 0;
    size_t prefix_len = 0;
    int found_dir = 0;
    int found_child = 0;

    while (norm[prefix_len]) prefix_len++;

    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        struct ramfs_file* rf;
        const char* rest;
        char child[248];
        int child_len = 0;
        uint32_t mode;

        if (!ramfs_table[i].in_use) continue;
        rf = &ramfs_table[i];
        if (!strcmp_exact(rf->name, norm) && ((rf->mode & 0170000U) == KSTAT_MODE_DIR)) {
            found_dir = 1;
            continue;
        }
        if (prefix_len == 0) continue;
        for (size_t j = 0; j < prefix_len; j++) {
            if (rf->name[j] != norm[j]) goto next_entry;
        }
        if (rf->name[prefix_len] != '/') goto next_entry;
        rest = rf->name + prefix_len + 1;
        if (*rest == '\0') goto next_entry;

        while (*rest && *rest != '/' && child_len < (int)sizeof(child) - 1) {
            child[child_len++] = *rest++;
        }
        child[child_len] = '\0';
        if (child[0] == '\0') goto next_entry;
        mode = (*rest == '/' || ((rf->mode & 0170000U) == KSTAT_MODE_DIR)) ? KSTAT_MODE_DIR : rf->mode;
        if (dirent_append(dirents, max_entries, &count, child, mode, (uint32_t)rf->size) < 0) return -1;
        found_child = 1;
next_entry:
        ;
    }

    *out_count = count;
    return (found_dir || found_child) ? 0 : -1;
}

static void split_child_path(const char* full_path, const char* dir_path,
                             char* child, uint32_t* mode, uint32_t file_mode) {
    const char* norm;
    const char* rest;
    int i = 0;
    if (!child || !mode) return;
    child[0] = '\0';
    norm = normalize_fs_path(full_path);
    rest = norm;
    if (dir_path && dir_path[0] != '\0') {
        const char* dir = normalize_fs_path(dir_path);
        while (*dir && *rest && *dir == *rest) {
            dir++;
            rest++;
        }
        if (*dir != '\0') return;
        if (*rest != '\0' && *rest != '/') return;
        if (*rest == '/') rest++;
        if (*rest == '\0') return;
    }
    while (*rest && *rest != '/' && i < 247) child[i++] = *rest++;
    child[i] = '\0';
    if (child[0] == '\0') return;
    *mode = (*rest == '/' || (file_mode & 0170000U) == KSTAT_MODE_DIR) ? KSTAT_MODE_DIR : KSTAT_MODE_FILE;
}

static int build_module_dirents(const char* dir_path, struct orth_dirent* dirents,
                                size_t max_entries, size_t* out_count) {
    size_t count = out_count ? *out_count : 0;
    int found = 0;

    if (!module_request.response) return -1;

    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
        struct limine_file* m = module_request.response->modules[i];
        char child[248];
        uint32_t mode = KSTAT_MODE_FILE;

        split_child_path(m->path, dir_path, child, &mode, KSTAT_MODE_FILE);
        if (child[0] == '\0') continue;
        if (dirent_append(dirents, max_entries, &count, child, mode, (uint32_t)m->size) < 0) return -1;
        found = 1;
    }

    if (out_count) *out_count = count;
    return found ? 0 : -1;
}

static int module_dir_exists(const char* dir_path) {
    if (!module_request.response) return 0;

    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
        struct limine_file* m = module_request.response->modules[i];
        char child[248];
        uint32_t mode = KSTAT_MODE_FILE;

        split_child_path(m->path, dir_path, child, &mode, KSTAT_MODE_FILE);
        if (child[0] != '\0') return 1;
    }

    return 0;
}

static int build_root_dirents(struct orth_dirent* dirents, size_t max_entries, size_t* out_count) {
    size_t count = 0;
    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        if (!ramfs_table[i].in_use) continue;
        if (contains_char(ramfs_table[i].name, '/')) continue;
        if (dirent_append(dirents, max_entries, &count, ramfs_table[i].name, ramfs_table[i].mode, (uint32_t)ramfs_table[i].size) < 0) return -1;
    }
    if (g_root_source == ROOT_SOURCE_XV6FS && xv6fs_is_mounted()) {
        if (xv6fs_list_dir("", dirents, max_entries, &count) < 0) return -1;
    } else if (module_request.response) {
        if (build_module_dirents("", dirents, max_entries, &count) < 0 && count == 0) return -1;
    }
    if (usb_block_device_ready()) {
        const struct vfs_mountpoint* mounts[VFS_MAX_MOUNTPOINTS];
        size_t mount_count = vfs_list_mountpoints(mounts, VFS_MAX_MOUNTPOINTS);
        for (size_t i = 0; i < mount_count; i++) {
            if (dirent_append(dirents, max_entries, &count, mounts[i]->path, KSTAT_MODE_DIR, 0) < 0) return -1;
        }
    }
    *out_count = count;
    return 0;
}

static int build_usb_dirents(const char* path, struct orth_dirent* dirents, size_t max_entries, size_t* out_count) {
    struct fat_boot_info info;
    struct fat_dir_entry_info ent;
    uint32_t dir_cluster;
    uint8_t sector[4096];
    uint32_t cluster;
    struct fat_lfn_state lfn;
    size_t count = 0;
    const char* usb_path = 0;
    const struct vfs_mountpoint* mp = vfs_find_mountpoint(path, &usb_path);
    if (!mp || mp->kind != VFS_MOUNT_USB_FAT) return -1;
    if (usb_load_fat_boot(&info) < 0) return -1;
    if (*usb_path == '\0') {
        dir_cluster = info.root_cluster;
    } else {
        char usb_full_path[320];
        int pos = 0;
        for (int i = 0; mp->path[i] && pos < (int)sizeof(usb_full_path) - 1; i++) usb_full_path[pos++] = mp->path[i];
        if (pos < (int)sizeof(usb_full_path) - 1) usb_full_path[pos++] = '/';
        for (int i = 0; usb_path[i] && pos < (int)sizeof(usb_full_path) - 1; i++) usb_full_path[pos++] = usb_path[i];
        usb_full_path[pos] = '\0';
        if (fat_resolve_path(usb_full_path, &ent) < 0) return -1;
        if ((ent.attr & 0x10U) == 0) return -1;
        dir_cluster = ent.first_cluster;
    }
    fat_lfn_reset(&lfn);
    cluster = dir_cluster;
    while (cluster >= 2 && cluster < 0x0FFFFFF8U) {
        uint32_t lba = fat_cluster_to_lba(&info, cluster);
        uint32_t total = (uint32_t)info.bytes_per_sector * info.sectors_per_cluster;
        if (total > sizeof(sector)) return -1;
        if (usb_read_blocks_safe(lba, sector, info.sectors_per_cluster) < 0) return -1;
        for (uint32_t off = 0; off < total; off += 32) {
            uint8_t* e = &sector[off];
            struct fat_dir_entry_info item;
            uint32_t mode;
            if (e[0] == 0x00) {
                *out_count = count;
                return 0;
            }
            if (e[0] == 0xE5) {
                fat_lfn_reset(&lfn);
                continue;
            }
            if (e[11] == 0x0F) {
                fat_lfn_append_chunk(&lfn, e);
                continue;
            }
            fat_decode_entry(e, &lfn, &item);
            if (strcmp_exact(item.name, ".") || strcmp_exact(item.name, "..")) continue;
            mode = (item.attr & 0x10U) ? KSTAT_MODE_DIR : KSTAT_MODE_FILE;
            if (dirent_append(dirents, max_entries, &count, item.name, mode, item.size) < 0) return -1;
        }
        cluster = fat_read_next_cluster(&info, cluster);
    }
    *out_count = count;
    return 0;
}

static struct ramfs_file* find_ramfs(const char* name);

static int build_active_root_dirents(const char* path, struct orth_dirent* dirents, size_t max_entries, size_t* out_count) {
    const char* norm = normalize_fs_path(path);
    size_t count = out_count ? *out_count : 0;
    size_t dir_size = 0;
    uint32_t dir_mode = 0;
    int explicit_dir = 0;
    int found = 0;
    if (norm[0] == '\0') return build_root_dirents(dirents, max_entries, out_count);
    if (build_ramfs_subdir_dirents(norm, dirents, max_entries, &count) == 0) {
        found = 1;
    }
    if (fs_try_active_root_lookup(norm, 0, &dir_size, &dir_mode) == 0) {
        if ((dir_mode & 0170000U) != KSTAT_MODE_DIR) return -1;
        explicit_dir = 1;
        if (g_root_source == ROOT_SOURCE_XV6FS && xv6fs_is_mounted()) {
            if (xv6fs_list_dir(norm, dirents, max_entries, &count) < 0) return -1;
            found = 1;
        }
        else if (module_request.response) {
            if (build_module_dirents(norm, dirents, max_entries, &count) == 0) found = 1;
        }
    }
    if (!found && !explicit_dir) {
        if (g_root_source == ROOT_SOURCE_XV6FS && xv6fs_is_mounted()) {
            size_t before = count;
            if (xv6fs_list_dir(norm, dirents, max_entries, &count) < 0) return -1;
            if (count > before) found = 1;
        } else if (module_request.response) {
            size_t before = count;
            if (build_module_dirents(norm, dirents, max_entries, &count) == 0 && count > before) found = 1;
        }
    }
    if (out_count) *out_count = count;
    return found ? 0 : -1;
}

void fs_init(void) {
    storage_init();
    spinlock_init(&g_fifo_lock);
    /* g_exec_cache_lock は static ゼロ初期化 (locked=0) で初期化済み。
     * 定義がこの関数より後方にあるためここでは初期化しない。 */
    wait_queue_init(&g_fifo_open_wait);
    for (int i = 0; i < FIFO_MAX; i++) {
        g_fifo_table[i].in_use = 0;
        g_fifo_table[i].pipe = 0;
    }
    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        ramfs_table[i].in_use = 0;
        ramfs_table[i].data = NULL;
        ramfs_table[i].mode = 0;
        ramfs_table[i].uid = 0;
        ramfs_table[i].gid = 0;
        ramfs_table[i].nlink = 0;
        ramfs_table[i].atime_sec = 0;
        ramfs_table[i].mtime_sec = 0;
        ramfs_table[i].ctime_sec = 0;
    }
    vfs_init();
    vfs_register_mountpoint("usb", VFS_MOUNT_USB_FAT, 0);
}

static int ramfs_grow(struct ramfs_file* rf, size_t needed) {
    if (needed <= rf->capacity) return 0;
    size_t new_cap = rf->capacity ? rf->capacity * 2 : (size_t)PAGE_SIZE;
    while (new_cap < needed) new_cap *= 2;
    void* phys = pmm_alloc((int)(new_cap / PAGE_SIZE));
    if (!phys) return -1;
    uint8_t* nd = (uint8_t*)PHYS_TO_VIRT(phys);
    for (size_t i = 0; i < new_cap; i++) nd[i] = 0;
    if (rf->data) {
        for (size_t i = 0; i < rf->size; i++) nd[i] = rf->data[i];
        pmm_free((void*)VIRT_TO_PHYS((uint64_t)rf->data), (int)(rf->capacity / PAGE_SIZE));
    }
    rf->data = nd;
    rf->capacity = new_cap;
    return 0;
}

static struct ramfs_file* find_ramfs(const char* name) {
    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        if (ramfs_table[i].in_use && strcmp_exact(ramfs_table[i].name, name)) {
            return &ramfs_table[i];
        }
    }
    return NULL;
}

static struct ramfs_file* create_ramfs(const char* name, uint32_t mode) {
    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        if (!ramfs_table[i].in_use) {
            ramfs_table[i].in_use = 1;
            for (int j = 0; j < 63; j++) {
                ramfs_table[i].name[j] = name[j];
                if (name[j] == '\0') break;
            }
            ramfs_table[i].name[63] = '\0';
            ramfs_table[i].data = NULL;
            ramfs_table[i].size = 0;
            ramfs_table[i].capacity = 0;
            ramfs_table[i].mode = KSTAT_MODE_FILE | (mode & 07777U);
            ramfs_table[i].uid = 0;
            ramfs_table[i].gid = 0;
            ramfs_table[i].nlink = 1;
            ramfs_table[i].atime_sec = fs_now_sec();
            ramfs_table[i].mtime_sec = ramfs_table[i].atime_sec;
            ramfs_table[i].ctime_sec = ramfs_table[i].atime_sec;
            return &ramfs_table[i];
        }
    }
    return NULL;
}

static struct ramfs_file* create_ramfs_dir(const char* name, uint32_t mode) {
    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        if (!ramfs_table[i].in_use) {
            ramfs_table[i].in_use = 1;
            for (int j = 0; j < 63; j++) {
                ramfs_table[i].name[j] = name[j];
                if (name[j] == '\0') break;
            }
            ramfs_table[i].name[63] = '\0';
            ramfs_table[i].data = NULL;
            ramfs_table[i].size = 0;
            ramfs_table[i].mode = KSTAT_MODE_DIR | (mode & 07777U);
            ramfs_table[i].uid = 0;
            ramfs_table[i].gid = 0;
            ramfs_table[i].nlink = 2;
            ramfs_table[i].atime_sec = fs_now_sec();
            ramfs_table[i].mtime_sec = ramfs_table[i].atime_sec;
            ramfs_table[i].ctime_sec = ramfs_table[i].atime_sec;
            return &ramfs_table[i];
        }
    }
    return NULL;
}

static int fs_try_active_root_lookup(const char* path, void** data, size_t* size, uint32_t* mode) {
    if (g_root_source == ROOT_SOURCE_XV6FS && xv6fs_is_mounted()) {
        uint32_t xv6_mode = 0;
        uint64_t xv6_size = 0;
        if (xv6fs_stat_path(path, &xv6_mode, &xv6_size, 0, 0) < 0) return -1;
        if (mode) *mode = xv6_mode;
        if (size) *size = (size_t)xv6_size;
        if (data) {
            if ((xv6_mode & 0170000U) == KSTAT_MODE_DIR) {
                *data = 0;
                return 0;
            }
            /* inode ポインタを返す (FT_XV6FS の private_data として使用) */
            struct xv6fs_inode* ip = xv6fs_namei(path);
            if (!ip) return -1;
            *data = ip;
        }
        return 0;
    }
    return -1;
}

static int fs_try_active_root_stat(const char* path, struct kstat* st) {
    if (g_root_source == ROOT_SOURCE_XV6FS && xv6fs_is_mounted()) {
        uint32_t mode = 0;
        uint64_t size = 0;
        uint32_t rdev = 0;
        if (xv6fs_stat_path(path, &mode, &size, 0, &rdev) < 0) return -1;
        kstat_set_defaults(st, mode, (int64_t)size);
        st->dev = FS_DEV_ROOT_ARCHIVE;
        st->ino = fs_hash_name(path);
        st->rdev = rdev;
        return 0;
    }
    return -1;
}

static int fs_root_is_usb_only(void) {
    return 0;
}

int fs_mount_module_root(void) {
    g_root_source = ROOT_SOURCE_MODULE;
    return 0;
}

int fs_mount_xv6fs_root(void) {
    if (!xv6fs_is_mounted()) return -1;
    g_root_source = ROOT_SOURCE_XV6FS;
    return 0;
}

int fs_get_mount_status(char* buf, size_t size) {
    const char* module_root = "root=module:boot/rootfs.img\n/usb -> usb-fat";
    const char* xv6fs_root  = storage_find_device("vblk0") ?
                               "root=xv6fs:vblk0" : "root=xv6fs:bootimg0";
    const char* src = (g_root_source == ROOT_SOURCE_MODULE) ? module_root : xv6fs_root;
    size_t i = 0;
    if (!buf || size == 0) return -EFAULT;
    while (src[i] && i + 1 < size) { buf[i] = src[i]; i++; }
    buf[(i < size) ? i : (size - 1)] = '\0';
    return 0;
}

/* FIFO の open ランデブー条件 (wait_event 用ヒント。外側で再確認する) */
struct fifo_wait_arg {
    struct fifo_entry* e;
    int need_writer;
};

static int fs_fifo_counterpart_hint(void* arg) {
    struct fifo_wait_arg* w = (struct fifo_wait_arg*)arg;
    return w->need_writer ? (w->e->writers > 0) : (w->e->readers > 0);
}

/* xv6fs T_FIFO inode の open。inum をキーに共有 pipe_t へ接続し、
 * FT_PIPE として fd を組み立てる。POSIX の open ランデブー
 * (読み手は書き手の出現まで待つ / 逆も) と O_NONBLOCK に対応。 */
static int fs_open_install_fifo(int fd, const char* path, int flags, uint32_t inum) {
    struct task* current = get_current_task();
    struct fifo_entry* e = 0;
    pipe_t* pipe = 0;
    fs_file_t* file;
    uint64_t irqf;
    int acc = flags & 3;
    int want_r = (acc == O_RDONLY || acc == O_RDWR) ? 1 : 0;
    int want_w = (acc == O_WRONLY || acc == O_RDWR) ? 1 : 0;
    int idx = -1;

    if (!current) return -1;

retry:
    irqf = spin_lock_irqsave(&g_fifo_lock);
    idx = -1;
    for (int i = 0; i < FIFO_MAX; i++) {
        if (g_fifo_table[i].in_use && g_fifo_table[i].inum == inum) { idx = i; break; }
    }
    if (idx < 0) {
        for (int i = 0; i < FIFO_MAX; i++) {
            if (!g_fifo_table[i].in_use) { idx = i; break; }
        }
        if (idx < 0) {
            spin_unlock_irqrestore(&g_fifo_lock, irqf);
            return -1;
        }
        /* スロットを確保してから pipe をロック外で割り当てる */
        g_fifo_table[idx].in_use = 1;
        g_fifo_table[idx].inum = inum;
        g_fifo_table[idx].pipe = 0;
        g_fifo_table[idx].readers = 0;
        g_fifo_table[idx].writers = 0;
        g_fifo_table[idx].opens = 0;
        spin_unlock_irqrestore(&g_fifo_lock, irqf);
        {
            void* phys = pmm_alloc(1);
            if (!phys) {
                irqf = spin_lock_irqsave(&g_fifo_lock);
                g_fifo_table[idx].in_use = 0;
                spin_unlock_irqrestore(&g_fifo_lock, irqf);
                return -1;
            }
            pipe = (pipe_t*)PHYS_TO_VIRT(phys);
            spinlock_init(&pipe->lock);
            pipe->read_pos = 0;
            pipe->write_pos = 0;
            pipe->count = 0;
            pipe->ref_count = 0;
            pipe->read_waiter = 0;
            pipe->write_waiter = 0;
        }
        irqf = spin_lock_irqsave(&g_fifo_lock);
        g_fifo_table[idx].pipe = pipe;
    }
    e = &g_fifo_table[idx];
    if (!e->pipe) {
        /* 他タスクが pipe 割り当て中: 少し譲って引き直す */
        spin_unlock_irqrestore(&g_fifo_lock, irqf);
        kernel_yield();
        goto retry;
    }
    pipe = e->pipe;
    e->opens++;
    e->readers += want_r;
    e->writers += want_w;
    spin_unlock_irqrestore(&g_fifo_lock, irqf);
    wake_up_all(&g_fifo_open_wait);

    if (flags & ORTH_LINUX_O_NONBLOCK) {
        if (want_w && !want_r && e->readers <= 0) {
            irqf = spin_lock_irqsave(&g_fifo_lock);
            e->writers -= want_w;
            e->opens--;
            if (e->opens <= 0) { e->in_use = 0; e->pipe = 0; }
            spin_unlock_irqrestore(&g_fifo_lock, irqf);
            wake_up_all(&g_fifo_open_wait);
            return -6; /* ENXIO */
        }
    } else if (want_r != want_w) {
        struct fifo_wait_arg warg;
        warg.e = e;
        warg.need_writer = want_r;
        while (!fs_fifo_counterpart_hint(&warg)) {
            wait_event(&g_fifo_open_wait, fs_fifo_counterpart_hint, &warg);
        }
    }

    file = fs_alloc_file();
    if (!file) {
        irqf = spin_lock_irqsave(&g_fifo_lock);
        e->readers -= want_r;
        e->writers -= want_w;
        e->opens--;
        if (e->opens <= 0) { e->in_use = 0; e->pipe = 0; }
        spin_unlock_irqrestore(&g_fifo_lock, irqf);
        wake_up_all(&g_fifo_open_wait);
        return -1;
    }
    /* O_RDWR は読み手役+書き手役の 2 参照として数える。1 参照だと
     * 既存 EOF 条件 (count==0 && ref_count<2) が成立してしまい、
     * O_RDWR で fifo を保持する GNU make のジョブサーバが空読みで
     * 偽 EOF を受ける (POSIX では自分が書き手なのでブロックが正)。 */
    irqf = spin_lock_irqsave(&pipe->lock);
    pipe->ref_count += want_r + want_w;
    spin_unlock_irqrestore(&pipe->lock, irqf);

    file->type = FT_PIPE;
    file->size = 0;
    file->offset = 0;
    file->ops = &g_fifo_file_ops;
    file->private_data = pipe;
    file->aux0 = (uint32_t)(want_r | (want_w << 1));
    file->aux1 = (uint32_t)idx;
    for (int j = 0; j < 63; j++) {
        file->path[j] = path[j];
        if (path[j] == '\0') break;
    }
    file->path[63] = '\0';
    current->fds[fd].type = FT_PIPE;
    current->fds[fd].file = file;
    current->fds[fd].data = 0;
    current->fds[fd].size = 0;
    current->fds[fd].offset = 0;
    current->fds[fd].in_use = 1;
    current->fds[fd].flags = flags;
    current->fds[fd].aux0 = 0;
    current->fds[fd].aux1 = 0;
    for (int j = 0; j < 63; j++) {
        current->fds[fd].name[j] = path[j];
        if (path[j] == '\0') break;
    }
    current->fds[fd].name[63] = '\0';
    return fd;
}

/* Install an FT_CHARDEV descriptor for an xv6fs T_DEVICE node.
 * The backing inode is not retained: major/minor fully describe the device. */
static int fs_open_install_chardev(int fd, const char* path, int flags,
                                   uint32_t major, uint32_t minor) {
    struct task* current = get_current_task();
    fs_file_t* file;
    if (!current) return -1;
    file = fs_alloc_file();
    if (!file) return -1;
    file->type = FT_CHARDEV;
    file->size = 0;
    file->offset = 0;
    file->ops = &g_chardev_file_ops;
    file->private_data = 0;
    file->aux0 = major;
    file->aux1 = minor;
    for (int j = 0; j < 63; j++) {
        file->path[j] = path[j];
        if (path[j] == '\0') break;
    }
    file->path[63] = '\0';
    current->fds[fd].type = FT_CHARDEV;
    current->fds[fd].file = file;
    current->fds[fd].data = 0;
    current->fds[fd].size = 0;
    current->fds[fd].offset = 0;
    current->fds[fd].in_use = 1;
    current->fds[fd].flags = flags;
    current->fds[fd].aux0 = major;
    current->fds[fd].aux1 = minor;
    for (int j = 0; j < 63; j++) {
        current->fds[fd].name[j] = path[j];
        if (path[j] == '\0') break;
    }
    current->fds[fd].name[63] = '\0';
    return fd;
}

/* ------------------------------------------------------------------ */
/* exec イメージキャッシュ                                              */
/*                                                                     */
/* fs_get_file_data は exec 専用 (task_exec が実行イメージと ELF        */
/* インタプリタを読むためだけに呼ぶ)。cc1 は 1 ビルドで ~70 回 exec さ  */
/* れ、毎回 13MB をディスクから読み返す (計 ~900MB)。パス+サイズをキー  */
/* に読み込んだイメージを RAM に保持し、同一ファイルの再 exec を無 I/O  */
/* にする。                                                             */
/*                                                                     */
/* 所有権: キャッシュ済みバッファは cache が所有し、fs_free_exec_buffer  */
/* では解放せず in_use 参照数だけ減らす。get→elf_load→free は BKL 下で   */
/* 同期完結するため、利用中バッファが別 exec に追い出される事はない。    */
/* ファイル書換 (write / truncate / unlink / rename) で該当エントリを   */
/* 無効化する (使用中なら stale 化し、最後の利用者の free で解放)。      */
/* ------------------------------------------------------------------ */

#define EXEC_CACHE_MAX     12
#define EXEC_CACHE_BUDGET  (128UL * 1024UL * 1024UL)   /* 合計上限 128MB */

struct exec_cache_entry {
    int      valid;      /* スロット占有 (buf 確保済み) */
    int      in_use;     /* elf_load 中の利用者数 (get - free) */
    int      stale;      /* 無効化済み。ヒットさせず、in_use==0 で解放 */
    size_t   size;
    size_t   npages;
    void*    buf;        /* pmm バッファ (virt) */
    uint64_t lru;        /* 最終使用時刻 */
    char     path[256];
};

static struct exec_cache_entry g_exec_cache[EXEC_CACHE_MAX];
static spinlock_t g_exec_cache_lock;
static uint64_t   g_exec_cache_clock;
static size_t     g_exec_cache_bytes;

static void exec_cache_set_path(char* dst, const char* src) {
    int i = 0;
    for (; i < 255 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* lock 保持で呼ぶ。エントリを解放してスロットを空ける。 */
static void exec_cache_drop_locked(struct exec_cache_entry* e) {
    if (e->buf && e->npages) {
        pmm_free((void*)VIRT_TO_PHYS((uint64_t)e->buf), (int)e->npages);
        if (g_exec_cache_bytes >= e->npages * PAGE_SIZE)
            g_exec_cache_bytes -= e->npages * PAGE_SIZE;
    }
    e->valid = 0; e->stale = 0; e->in_use = 0; e->buf = 0;
    e->size = 0; e->npages = 0; e->lru = 0; e->path[0] = '\0';
}

/* path+size 一致の有効エントリがあれば in_use++ して buf を返す。無ければ 0。 */
static void* exec_cache_lookup(const char* norm, size_t size) {
    void* ret = 0;
    uint64_t flags = spin_lock_irqsave(&g_exec_cache_lock);
    for (int i = 0; i < EXEC_CACHE_MAX; i++) {
        struct exec_cache_entry* e = &g_exec_cache[i];
        if (e->valid && !e->stale && e->size == size &&
            strcmp_exact(e->path, norm)) {
            e->in_use++;
            e->lru = ++g_exec_cache_clock;
            ret = e->buf;
            break;
        }
    }
    spin_unlock_irqrestore(&g_exec_cache_lock, flags);
    return ret;
}

/* buf をキャッシュに登録し in_use=1 にする。載れば 1 (以後 cache 所有)、
 * 予算/本数を確保できなければ 0 (呼び出し側が従来どおり所有・解放)。 */
static int exec_cache_insert(const char* norm, void* buf, size_t size, size_t npages) {
    size_t need = npages * PAGE_SIZE;
    if (need > EXEC_CACHE_BUDGET) return 0;
    uint64_t flags = spin_lock_irqsave(&g_exec_cache_lock);
    for (;;) {
        int slot = -1;
        for (int i = 0; i < EXEC_CACHE_MAX; i++)
            if (!g_exec_cache[i].valid) { slot = i; break; }
        if (slot >= 0 && g_exec_cache_bytes + need <= EXEC_CACHE_BUDGET) {
            struct exec_cache_entry* e = &g_exec_cache[slot];
            e->valid = 1; e->stale = 0; e->in_use = 1;
            e->buf = buf; e->size = size; e->npages = npages;
            e->lru = ++g_exec_cache_clock;
            exec_cache_set_path(e->path, norm);
            g_exec_cache_bytes += need;
            spin_unlock_irqrestore(&g_exec_cache_lock, flags);
            return 1;
        }
        /* 空き無し or 予算超過 → in_use==0 の LRU を追い出す */
        int victim = -1; uint64_t oldest = ~0ULL;
        for (int i = 0; i < EXEC_CACHE_MAX; i++) {
            struct exec_cache_entry* e = &g_exec_cache[i];
            if (e->valid && e->in_use == 0 && e->lru < oldest) {
                oldest = e->lru; victim = i;
            }
        }
        if (victim < 0) {   /* 全て使用中 → 諦める (呼び出し側所有のまま) */
            spin_unlock_irqrestore(&g_exec_cache_lock, flags);
            return 0;
        }
        exec_cache_drop_locked(&g_exec_cache[victim]);
    }
}

/* raw_path のキャッシュを無効化する (ファイル書換時に呼ぶ)。使用中なら
 * stale 化して最後の利用者の free に解放を委ねる。 */
static void exec_cache_invalidate(const char* raw_path) {
    if (!raw_path || !raw_path[0]) return;
    if (g_exec_cache_bytes == 0) return;   /* 未投入なら即 return (BKL 下) */
    char resolved[256];
    resolve_task_path(raw_path, resolved, sizeof(resolved));
    const char* norm = normalize_fs_path(resolved);
    uint64_t flags = spin_lock_irqsave(&g_exec_cache_lock);
    for (int i = 0; i < EXEC_CACHE_MAX; i++) {
        struct exec_cache_entry* e = &g_exec_cache[i];
        if (e->valid && !e->stale && strcmp_exact(e->path, norm)) {
            if (e->in_use == 0) exec_cache_drop_locked(e);
            else e->stale = 1;
        }
    }
    spin_unlock_irqrestore(&g_exec_cache_lock, flags);
}

int fs_open(const char* path, int flags, int mode) {
    struct task* current = get_current_task();
    int want_dir = (flags & (O_DIRECTORY | ORTH_LEGACY_O_DIRECTORY)) != 0;
    int want_creat = (flags & O_CREAT) != 0;
    int want_trunc = (flags & O_TRUNC) != 0;
    const char* mount_subpath = 0;
    const struct vfs_mountpoint* mount = 0;
    char resolved_path[256];
    if (!current) return -ESRCH;

    int fd = -1;
    /* POSIX: 最小の空き fd を割り当てる (0-2 も対象。busybox ash の
     * バックグラウンドジョブは close(0) 後の open が fd 0 を返すことを要求) */
    for (int i = 0; i < MAX_FDS; i++) {
        if (!current->fds[i].in_use) {
            fd = i;
            break;
        }
    }
    if (fd == -1) return -EMFILE;

    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    path = normalize_fs_path(resolved_path);
    /* 書き込み目的の open は exec キャッシュを無効化 (内容が変わり得る)。 */
    if (((flags & ORTH_O_ACCMODE) != O_RDONLY) || want_creat || want_trunc) {
        exec_cache_invalidate(path);
    }
    mount = vfs_find_mountpoint(path, &mount_subpath);

    if (want_dir) {
        struct orth_dirent* dirents;
        size_t entry_count = 0;
        fs_file_t* file;
        void* phys = pmm_alloc(DIR_BUF_PAGES);
        if (!phys) return -ENOMEM;
        dirents = (struct orth_dirent*)PHYS_TO_VIRT(phys);
        if (*path == '\0') {
            if (build_root_dirents(dirents, DIR_BUF_BYTES / sizeof(struct orth_dirent), &entry_count) < 0) {
                pmm_free(phys, DIR_BUF_PAGES);
                return -ENOENT;
            }
        } else if (mount && mount->kind == VFS_MOUNT_USB_FAT) {
            if (build_usb_dirents(path, dirents, DIR_BUF_BYTES / sizeof(struct orth_dirent), &entry_count) < 0) {
                pmm_free(phys, DIR_BUF_PAGES);
                return -ENOENT;
            }
        } else {
            if (build_active_root_dirents(path, dirents, DIR_BUF_BYTES / sizeof(struct orth_dirent), &entry_count) < 0) {
                pmm_free(phys, DIR_BUF_PAGES);
                return -ENOENT;
            }
        }
        file = fs_alloc_file();
        if (!file) {
            pmm_free(phys, DIR_BUF_PAGES);
            return -ENFILE;
        }
        file->type = FT_DIR;
        file->size = entry_count * sizeof(struct orth_dirent);
        file->offset = 0;
        file->ops = &g_dir_file_ops;
        file->private_data = dirents;
        file->aux0 = DIR_BUF_PAGES;
        current->fds[fd].type = FT_DIR;
        current->fds[fd].file = file;
        current->fds[fd].data = 0;
        current->fds[fd].size = 0;
        current->fds[fd].offset = 0;
        current->fds[fd].in_use = 1;
        current->fds[fd].flags = flags;
        current->fds[fd].aux0 = 0;
        current->fds[fd].aux1 = 0;
        if (*path == '\0') {
            file->path[0] = '/';
            file->path[1] = '\0';
            current->fds[fd].name[0] = '/';
            current->fds[fd].name[1] = '\0';
        } else {
            for (int j = 0; j < 63; j++) {
                file->path[j] = resolved_path[j];
                current->fds[fd].name[j] = resolved_path[j];
                if (resolved_path[j] == '\0') break;
            }
            file->path[63] = '\0';
            current->fds[fd].name[63] = '\0';
        }
        return fd;
    }

    if (mount && mount->kind == VFS_MOUNT_USB_FAT) {
        struct fat_dir_entry_info ent;
        fs_file_t* file;
        if ((flags & O_WRONLY) || (flags & O_RDWR) || want_creat) return -EROFS;
        if (fat_resolve_path(path, &ent) < 0) return -ENOENT;
        if (ent.attr & 0x10U) return -EISDIR;
        file = fs_alloc_file();
        if (!file) return -ENFILE;
        file->type = FT_USB;
        file->size = ent.size;
        file->offset = 0;
        file->ops = &g_usb_file_ops;
        file->private_data = 0;
        file->aux0 = ent.first_cluster;
        file->aux1 = ent.attr;
        for (int j = 0; j < 63; j++) {
            file->path[j] = path[j];
            if (path[j] == '\0') break;
        }
        file->path[63] = '\0';
        current->fds[fd].type = FT_USB;
        current->fds[fd].file = file;
        current->fds[fd].data = NULL;
        current->fds[fd].size = 0;
        current->fds[fd].offset = 0;
        current->fds[fd].in_use = 1;
        current->fds[fd].flags = flags;
        current->fds[fd].aux0 = 0;
        current->fds[fd].aux1 = 0;
        for (int j = 0; j < 63; j++) {
            current->fds[fd].name[j] = path[j];
            if (path[j] == '\0') break;
        }
        current->fds[fd].name[63] = '\0';
        return fd;
    }

    struct ramfs_file* rf = find_ramfs(path);
    if (rf) {
        fs_file_t* file;
        if ((rf->mode & 0170000U) == KSTAT_MODE_DIR) return -EISDIR;
        if (want_trunc) {
            rf->size = 0;
            rf->mtime_sec = fs_now_sec();
            rf->ctime_sec = rf->mtime_sec;
        }
        file = fs_alloc_file();
        if (!file) return -ENFILE;
        file->type = FT_RAMFS;
        file->size = rf->size;
        file->offset = 0;
        file->ops = &g_ramfs_file_ops;
        file->private_data = rf->data;
        for (int j = 0; j < 63; j++) {
            file->path[j] = rf->name[j];
            if (rf->name[j] == '\0') break;
        }
        file->path[63] = '\0';
        current->fds[fd].type = FT_RAMFS;
        current->fds[fd].file = file;
        current->fds[fd].data = 0;
        current->fds[fd].size = 0;
        current->fds[fd].offset = 0;
        current->fds[fd].in_use = 1;
        current->fds[fd].flags = flags;
        for(int j=0; j<63; j++) {
            current->fds[fd].name[j] = rf->name[j];
            if(rf->name[j] == '\0') break;
        }
        return fd;
    }

    /* /dev/kout: raw write-only output device (second virtio-blk) */
    if ((path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' && path[4]=='/' &&
         path[5]=='k' && path[6]=='o' && path[7]=='u' && path[8]=='t' && path[9]=='\0') ||
        (path[0]=='d' && path[1]=='e' && path[2]=='v' && path[3]=='/' &&
         path[4]=='k' && path[5]=='o' && path[6]=='u' && path[7]=='t' && path[8]=='\0')) {
        fs_file_t* file = fs_alloc_file();
        if (!file) return -ENFILE;
        file->type = FT_RAWDEV;
        file->size = 0;
        file->offset = 0;
        file->ops = 0;
        file->private_data = 0;
        file->path[0] = '\0';
        current->fds[fd].type = FT_RAWDEV;
        current->fds[fd].file = file;
        current->fds[fd].data = 0;
        current->fds[fd].size = 0;
        current->fds[fd].offset = 0;
        current->fds[fd].in_use = 1;
        current->fds[fd].flags = flags;
        current->fds[fd].name[0] = '\0';
        return fd;
    }

    if (want_creat) {
        if (g_root_source == ROOT_SOURCE_XV6FS && xv6fs_is_mounted()) {
            struct xv6fs_inode* ip = 0;
            if (xv6fs_create_file(path, mode, &ip) == 0 && ip) {
                fs_file_t* file;
                int16_t ip_type;
                uint32_t dev_major, dev_minor;
                xv6fs_ilock(ip);
                ip_type = ip->type;
                dev_major = (uint32_t)ip->major & 0xFFU;
                dev_minor = (uint32_t)ip->minor & 0xFFU;
                xv6fs_iunlock(ip);
                if (ip_type == XV6FS_T_DEVICE) {
                    /* e.g. "> /dev/null": O_TRUNC is a no-op on devices */
                    xv6fs_iput(ip);
                    return fs_open_install_chardev(fd, path, flags,
                                                   dev_major, dev_minor);
                }
                if (ip_type == XV6FS_T_FIFO) {
                    uint32_t fifo_inum = ip->inum;
                    xv6fs_iput(ip);
                    return fs_open_install_fifo(fd, path, flags, fifo_inum);
                }
                if (want_trunc && xv6fs_truncate_file(path, 0) < 0) {
                    xv6fs_iput(ip);
                    return -EIO;
                }
                file = fs_alloc_file();
                if (!file) {
                    xv6fs_iput(ip);
                    return -ENFILE;
                }
                file->type = FT_XV6FS;
                file->size = 0;
                file->offset = 0;
                file->ops = &g_xv6fs_file_ops;
                file->private_data = ip;
                file->aux0 = 0;
                xv6fs_ilock(ip);
                file->size = ip->size;
                xv6fs_iunlock(ip);
                for (int j = 0; j < 63; j++) {
                    file->path[j] = path[j];
                    if (path[j] == '\0') break;
                }
                file->path[63] = '\0';
                current->fds[fd].type = FT_XV6FS;
                current->fds[fd].file = file;
                current->fds[fd].data = 0;
                current->fds[fd].size = 0;
                current->fds[fd].offset = 0;
                current->fds[fd].in_use = 1;
                current->fds[fd].flags = flags;
                current->fds[fd].aux0 = 0;
                current->fds[fd].aux1 = 0;
                for (int j = 0; j < 63; j++) {
                    current->fds[fd].name[j] = path[j];
                    if (path[j] == '\0') break;
                }
                current->fds[fd].name[63] = '\0';
                return fd;
            }
        }
        rf = create_ramfs(path, (uint32_t)mode);
        if (rf) {
            fs_file_t* file = fs_alloc_file();
            if (!file) return -ENFILE;
            file->type = FT_RAMFS;
            file->size = 0;
            file->offset = 0;
            file->ops = &g_ramfs_file_ops;
            file->private_data = rf->data;
            for (int j = 0; j < 63; j++) {
                file->path[j] = rf->name[j];
                if (rf->name[j] == '\0') break;
            }
            file->path[63] = '\0';
            current->fds[fd].type = FT_RAMFS;
            current->fds[fd].file = file;
            current->fds[fd].data = 0;
            current->fds[fd].size = 0;
            current->fds[fd].offset = 0;
            current->fds[fd].in_use = 1;
            current->fds[fd].flags = flags;
            for(int j=0; j<63; j++) {
                current->fds[fd].name[j] = rf->name[j];
                if(rf->name[j] == '\0') break;
            }
            return fd;
        }
    }

    {
        void* root_data = 0;
        size_t root_size = 0;
        uint32_t root_mode = 0;
        if (fs_try_active_root_lookup(path, &root_data, &root_size, &root_mode) == 0) {
            fs_file_t* file;
            if ((root_mode & 0170000U) == KSTAT_MODE_DIR) {
                return -EISDIR;
            }
            if ((root_mode & 0170000U) == KSTAT_MODE_CHR) {
                struct xv6fs_inode* ip = (struct xv6fs_inode*)root_data;
                uint32_t dev_major = 0, dev_minor = 0;
                if (ip) {
                    xv6fs_ilock(ip);
                    dev_major = (uint32_t)ip->major & 0xFFU;
                    dev_minor = (uint32_t)ip->minor & 0xFFU;
                    xv6fs_iunlock(ip);
                    xv6fs_iput(ip);
                }
                return fs_open_install_chardev(fd, path, flags,
                                               dev_major, dev_minor);
            }
            if ((root_mode & 0170000U) == KSTAT_MODE_FIFO) {
                struct xv6fs_inode* ip = (struct xv6fs_inode*)root_data;
                uint32_t fifo_inum = 0;
                if (ip) {
                    fifo_inum = ip->inum;
                    xv6fs_iput(ip);
                }
                return fs_open_install_fifo(fd, path, flags, fifo_inum);
            }
            file = fs_alloc_file();
            if (!file) {
                if (root_data && root_size) {
                    pmm_free((void*)VIRT_TO_PHYS((uint64_t)root_data),
                             (int)((root_size + PAGE_SIZE - 1U) / PAGE_SIZE));
                }
                return -ENFILE;
            }
            file->type = FT_XV6FS;
            file->size = root_size;
            file->offset = 0;
            file->ops = &g_xv6fs_file_ops;
            file->private_data = root_data;   /* struct xv6fs_inode* */
            file->aux0 = 0;                   /* inode は iput で解放、pmm 不要 */
            for (int j = 0; j < 63; j++) {
                file->path[j] = path[j];
                if (path[j] == '\0') break;
            }
            file->path[63] = '\0';
            current->fds[fd].type = FT_XV6FS;
            current->fds[fd].file = file;
            current->fds[fd].data = 0;
            current->fds[fd].size = 0;
            current->fds[fd].offset = 0;
            current->fds[fd].in_use = 1;
            current->fds[fd].flags = flags;
            current->fds[fd].aux0 = 0;
            for (int j = 0; j < 63; j++) {
                current->fds[fd].name[j] = path[j];
                if (path[j] == '\0') break;
            }
            current->fds[fd].name[63] = '\0';
            return fd;
        }
    }

    if (fs_root_is_usb_only()) return -ENOENT;

    if (!module_request.response) return -ENOENT;

    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
        struct limine_file* m = module_request.response->modules[i];
        if (strcmp_suffix_fs(m->path, path)) {
            fs_file_t* file = fs_alloc_file();
            if (!file) return -ENFILE;
            file->type = FT_MODULE;
            file->size = m->size;
            file->offset = 0;
            file->ops = &g_module_file_ops;
            file->private_data = m->address;
            for (int j = 0; j < 63; j++) {
                file->path[j] = path[j];
                if (path[j] == '\0') break;
            }
            file->path[63] = '\0';
            current->fds[fd].type = FT_MODULE;
            current->fds[fd].file = file;
            current->fds[fd].data = 0;
            current->fds[fd].size = 0;
            current->fds[fd].offset = 0;
            current->fds[fd].in_use = 1;
            current->fds[fd].flags = flags;
            for (int j = 0; j < 63; j++) {
                current->fds[fd].name[j] = path[j];
                if (path[j] == '\0') break;
            }
            current->fds[fd].name[63] = '\0';
            return fd;
        }
    }
    return -ENOENT;
}

int fs_openat(int dirfd, const char* path, int flags, int mode) {
    char resolved_path[256];
    if (resolve_dirfd_path(dirfd, path, resolved_path, sizeof(resolved_path)) < 0) return -ENOENT;
    return fs_open(resolved_path, flags, mode);
}

int64_t fs_write(int fd, const void* buf, size_t count) {
    struct task* current = get_current_task();
    if (!current) return -ESRCH;
    if (fd >= 0 && fd < MAX_FDS && current->fds[fd].in_use
        && fs_fd_type(&current->fds[fd]) == FT_CONSOLE) {
        extern int64_t sys_write_serial(const char* buf, size_t count);
        return sys_write_serial((const char*)buf, count);
    }
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -EBADF;
    file_descriptor_t* f = &current->fds[fd];
    fs_assert_open_file_consistent(f);

    if (fs_fd_type(f) == FT_PIPE) {
        pipe_t* pipe = (pipe_t*)fs_fd_data_required(f, FT_PIPE);
        size_t written = 0;
        const char* src = (const char*)buf;
        while (written < count) {
            struct task* reader_to_wake = 0;
            uint64_t flags = spin_lock_irqsave(&pipe->lock);
            if (pipe->count == PIPE_BUF_SIZE) {
                task_mark_sleeping(current);
                pipe_set_waiter(&pipe->write_waiter, current);
                spin_unlock_irqrestore(&pipe->lock, flags);
                kernel_yield();
                flags = spin_lock_irqsave(&pipe->lock);
                pipe_clear_waiter(&pipe->write_waiter, current);
                spin_unlock_irqrestore(&pipe->lock, flags);
                continue;
            }
            while (written < count && pipe->count < PIPE_BUF_SIZE) {
                pipe->buffer[pipe->write_pos] = src[written];
                pipe->write_pos = (pipe->write_pos + 1) % PIPE_BUF_SIZE;
                pipe->count++;
                written++;
            }
            reader_to_wake = pipe_take_waiter_locked(&pipe->read_waiter);
            spin_unlock_irqrestore(&pipe->lock, flags);
            pipe_wake_task(reader_to_wake);
        }
        return (int64_t)written;
    }

    if (fs_fd_type(f) == FT_SOCKET) {
        (void)fs_fd_data_required(f, FT_SOCKET);
        return net_socket_write_fd(f, buf, count);
    }

    if (fs_fd_type(f) == FT_RAWDEV) {
        size_t off = fs_fd_offset(f);
        if (virtio_kout_write_raw((uint64_t)off, buf, count) < 0) return -EIO;
        fs_fd_set_offset(f, off + count);
        return (int64_t)count;
    }

    if (fs_fd_type(f) == FT_CHARDEV) {
        if (fs_fd_aux0(f) == CHR_MAJOR_TTY) {
            extern int64_t sys_write_serial(const char* wbuf, size_t wcount);
            return sys_write_serial((const char*)buf, count);
        }
        /* null / zero / random: writes are discarded */
        return (int64_t)count;
    }

    if (fs_fd_type(f) == FT_XV6FS) {
        struct xv6fs_inode* ip = (struct xv6fs_inode*)fs_fd_data_required(f, FT_XV6FS);
        const uint8_t* src = (const uint8_t*)buf;
        size_t offset_now = fs_fd_offset(f);
        size_t written = 0;
        if (f->flags & O_APPEND) {
            /* POSIX O_APPEND: seek to end of file before writing. */
            xv6fs_ilock(ip);
            offset_now = ip->size;
            xv6fs_iunlock(ip);
        }
        while (written < count) {
            size_t chunk = count - written;
            int n;
            if (chunk > XV6FS_WRITE_CHUNK_MAX) chunk = XV6FS_WRITE_CHUNK_MAX;
            xv6log_begin_op();
            xv6fs_ilock(ip);
            n = xv6fs_writei(ip, src + written,
                             (uint32_t)(offset_now + written),
                             (uint32_t)chunk);
            if (ip->size > fs_fd_size(f)) fs_fd_set_size(f, ip->size);
            xv6fs_iunlock(ip);
            xv6log_end_op();
            if (n < 0) return written ? (int64_t)written : -EIO;
            if (n == 0) break;
            written += (size_t)n;
            if ((size_t)n < chunk) break;
        }
        fs_fd_set_offset(f, offset_now + written);
        return (int64_t)written;
    }

    if (fs_fd_type(f) != FT_RAMFS) return -EBADF;
    {
        struct ramfs_file* rf_w = find_ramfs(fs_fd_name(f));
        if (!rf_w || !rf_w->in_use) return -ENOENT;
        size_t off_w = fs_fd_offset(f);
        if (f->flags & O_APPEND) off_w = rf_w->size;  /* POSIX O_APPEND */
        if (ramfs_grow(rf_w, off_w + count) < 0) return -ENOSPC;
        if (f->file) f->file->private_data = rf_w->data;
        uint8_t* dest = rf_w->data + off_w;
        const uint8_t* src = (const uint8_t*)buf;
        for (size_t i = 0; i < count; i++) dest[i] = src[i];
        fs_fd_set_offset(f, off_w + count);
        if (off_w + count > rf_w->size) {
            rf_w->size = off_w + count;
            fs_fd_set_size(f, rf_w->size);
            rf_w->mtime_sec = fs_now_sec();
            rf_w->ctime_sec = rf_w->mtime_sec;
        }
        return (int64_t)count;
    }
}

int fs_ftruncate(int fd, uint64_t length) {
    struct task* current = get_current_task();
    file_descriptor_t* f;

    if (!current) return -EINVAL;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -EBADF;
    f = &current->fds[fd];
    fs_assert_open_file_consistent(f);
    exec_cache_invalidate(fs_fd_name(f));

    if (fs_fd_type(f) == FT_RAMFS) {
        struct ramfs_file* rf = find_ramfs(fs_fd_name(f));
        if (!rf) return -ENOENT;
        if (length > rf->size) {
            if (ramfs_grow(rf, (size_t)length) < 0) return -ENOSPC;
            if (f->file) f->file->private_data = rf->data;
        }
        rf->size = (size_t)length;
        rf->mtime_sec = fs_now_sec();
        rf->ctime_sec = rf->mtime_sec;
        fs_fd_set_size(f, rf->size);
        if (fs_fd_offset(f) > rf->size) fs_fd_set_offset(f, rf->size);
        return 0;
    }

    if (fs_fd_type(f) == FT_XV6FS) {
        if (xv6fs_truncate_file(fs_fd_name(f), length) < 0) return -ENOENT;
        fs_fd_set_size(f, (size_t)length);
        if (fs_fd_offset(f) > (size_t)length) fs_fd_set_offset(f, (size_t)length);
        return 0;
    }

    return -EINVAL;
}

int fs_truncate(const char* path, uint64_t length) {
    char resolved_path[256];
    const char* norm;
    struct ramfs_file* rf;

    if (!path) return -EINVAL;
    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    norm = normalize_fs_path(resolved_path);
    exec_cache_invalidate(norm);

    rf = find_ramfs(norm);
    if (rf) {
        if (length > rf->size) {
            if (ramfs_grow(rf, (size_t)length) < 0) return -ENOSPC;
        }
        rf->size = (size_t)length;
        rf->mtime_sec = fs_now_sec();
        rf->ctime_sec = rf->mtime_sec;
        return 0;
    }

    if (xv6fs_truncate_file(norm, length) == 0) {
        return 0;
    }

    return -ENOENT;
}

/* Blocking console read shared by /dev/tty and /dev/console (no echo). */
static int fs_console_read_blocking(struct task* current, char* buf, size_t count) {
    extern int kb_read(char* buf, int count);
    extern void kb_set_waiter(struct task* t);
    extern void kb_clear_waiter(struct task* t);
    int read_bytes = 0;
    while (read_bytes == 0) {
        read_bytes = kb_read(buf, (int)count);
        if (read_bytes == 0) {
            task_mark_sleeping(current);
            kb_set_waiter(current);
            kernel_yield();
            kb_clear_waiter(current);
        }
    }
    return read_bytes;
}

int64_t fs_read(int fd, void* buf, size_t count) {
    struct task* current = get_current_task();
    if (!current) return -ESRCH;
    if (fd >= 0 && fd < MAX_FDS && current->fds[fd].in_use
        && fs_fd_type(&current->fds[fd]) == FT_CONSOLE) {
        extern int kb_read(char* buf, int count);
        extern void kb_set_waiter(struct task* t);
        extern void kb_clear_waiter(struct task* t);
        extern int64_t sys_write_serial(const char* buf, size_t count);
        int read_bytes = 0;
        while (read_bytes == 0) {
            read_bytes = kb_read((char*)buf, count);
            if (read_bytes == 0) {
                task_mark_sleeping(current);
                kb_set_waiter(current);
                kernel_yield();
                kb_clear_waiter(current);
            }
        }
        if (fd == 0 && read_bytes > 0) {
            const char* src = (const char*)buf;
            for (int i = 0; i < read_bytes; i++) {
                if (src[i] == '\r' || src[i] == '\n') {
                    sys_write_serial("\r\n", 2);
                } else if (src[i] == '\b' || src[i] == 0x7F) {
                    sys_write_serial("\b \b", 3);
                } else {
                    sys_write_serial(&src[i], 1);
                }
            }
        }
        return read_bytes;
    }
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -EBADF;
    file_descriptor_t* f = &current->fds[fd];
    fs_assert_open_file_consistent(f);

    if (fs_fd_type(f) == FT_PIPE) {
        pipe_t* pipe = (pipe_t*)fs_fd_data_required(f, FT_PIPE);
        while (1) {
            size_t to_read;
            struct task* writer_to_wake = 0;
            uint64_t flags = spin_lock_irqsave(&pipe->lock);
            if (pipe->count == 0 && pipe->ref_count < 2) {
                spin_unlock_irqrestore(&pipe->lock, flags);
                return 0; // 書き込み側が閉じられた
            }
            if (pipe->count == 0) {
                task_mark_sleeping(current);
                pipe_set_waiter(&pipe->read_waiter, current);
                spin_unlock_irqrestore(&pipe->lock, flags);
                kernel_yield();
                flags = spin_lock_irqsave(&pipe->lock);
                pipe_clear_waiter(&pipe->read_waiter, current);
                spin_unlock_irqrestore(&pipe->lock, flags);
                continue;
            }
            to_read = (count > pipe->count) ? pipe->count : count;
            char* dest = (char*)buf;
            for (size_t i = 0; i < to_read; i++) {
                dest[i] = pipe->buffer[pipe->read_pos];
                pipe->read_pos = (pipe->read_pos + 1) % PIPE_BUF_SIZE;
                pipe->count--;
            }
            writer_to_wake = pipe_take_waiter_locked(&pipe->write_waiter);
            spin_unlock_irqrestore(&pipe->lock, flags);
            pipe_wake_task(writer_to_wake);
            return (int64_t)to_read;
        }
    }

    if (fs_fd_type(f) == FT_SOCKET) {
        (void)fs_fd_data_required(f, FT_SOCKET);
        return net_socket_read_fd(f, buf, count);
    }

    if (fs_fd_type(f) == FT_USB) {
        struct fat_boot_info info;
        uint8_t sector[4096];
        size_t done = 0;
        uint32_t cluster;
        uint32_t cluster_size;
        size_t skip;
        size_t offset_now = fs_fd_offset(f);
        size_t size_now = fs_fd_size(f);
        if (usb_load_fat_boot(&info) < 0) return -EIO;
        cluster = fs_fd_aux0(f);
        cluster_size = (uint32_t)info.bytes_per_sector * info.sectors_per_cluster;
        if (cluster_size > sizeof(sector)) return -EIO;
        skip = offset_now;
        while (skip >= cluster_size && cluster >= 2 && cluster < 0x0FFFFFF8U) {
            cluster = fat_read_next_cluster(&info, cluster);
            skip -= cluster_size;
        }
        while (done < count && offset_now < size_now && cluster >= 2 && cluster < 0x0FFFFFF8U) {
            size_t chunk;
            uint32_t lba = fat_cluster_to_lba(&info, cluster);
            if (usb_read_blocks_safe(lba, sector, info.sectors_per_cluster) < 0) return (done > 0) ? (int64_t)done : -EIO;
            chunk = cluster_size - skip;
            if (chunk > count - done) chunk = count - done;
            if (chunk > size_now - offset_now) chunk = size_now - offset_now;
            for (size_t i = 0; i < chunk; i++) ((uint8_t*)buf)[done + i] = sector[skip + i];
            done += chunk;
            offset_now += chunk;
            skip = 0;
            if (done >= count || offset_now >= size_now) break;
            cluster = fat_read_next_cluster(&info, cluster);
        }
        fs_fd_set_offset(f, offset_now);
        return (int64_t)done;
    }

    if (fs_fd_type(f) == FT_DIR) return -EISDIR;

    if (fs_fd_type(f) == FT_CHARDEV) {
        uint32_t major = fs_fd_aux0(f);
        uint32_t minor = fs_fd_aux1(f);
        if (major == CHR_MAJOR_TTY) {
            return fs_console_read_blocking(current, (char*)buf, count);
        }
        if (major == CHR_MAJOR_MEM) {
            if (minor == CHR_MINOR_NULL) return 0;
            if (minor == CHR_MINOR_ZERO) {
                char* dest = (char*)buf;
                for (size_t i = 0; i < count; i++) dest[i] = 0;
                return (int64_t)count;
            }
            if (minor == CHR_MINOR_RANDOM || minor == CHR_MINOR_URANDOM) {
                extern int64_t sys_getrandom(void* buf, size_t len, unsigned flags);
                return sys_getrandom(buf, count, 0);
            }
        }
        return -EIO;
    }

    if (fs_fd_type(f) == FT_XV6FS) {
        struct xv6fs_inode* ip = (struct xv6fs_inode*)fs_fd_data_required(f, FT_XV6FS);
        xv6fs_ilock(ip);
        uint32_t off = (uint32_t)fs_fd_offset(f);
        int n = xv6fs_readi(ip, buf, off, (uint32_t)count);
        xv6fs_iunlock(ip);
        if (n < 0) return -EIO;
        fs_fd_set_offset(f, fs_fd_offset(f) + (size_t)n);
        return (int64_t)n;
    }

    if (fs_fd_offset(f) >= fs_fd_size(f)) return 0;
    size_t remaining = fs_fd_size(f) - fs_fd_offset(f);
    size_t to_read = (count > remaining) ? remaining : count;
    char* dest = (char*)buf;
    KASSERT(fs_fd_data(f) != 0);
    char* src = (char*)fs_fd_data(f) + fs_fd_offset(f);
    for (size_t i = 0; i < to_read; i++) dest[i] = src[i];
    fs_fd_set_offset(f, fs_fd_offset(f) + to_read);
    if (fs_fd_type(f) == FT_RAMFS) {
        struct ramfs_file* rf = find_ramfs(fs_fd_name(f));
        if (rf) rf->atime_sec = fs_now_sec();
    }
    return (int64_t)to_read;
}

int fs_close(int fd) {
    struct task* current = get_current_task();
    if (!current) return -ESRCH;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -EBADF;

    fs_assert_open_file_consistent(&current->fds[fd]);
    fs_release_fd(&current->fds[fd]);
    return 0;
}

int fs_dup2(int oldfd, int newfd) {
    struct task* current = get_current_task();
    if (!current) return -ESRCH;
    if (oldfd < 0 || oldfd >= MAX_FDS || !current->fds[oldfd].in_use) return -EBADF;
    if (newfd < 0 || newfd >= MAX_FDS) return -EBADF;
    if (oldfd == newfd) return newfd;

    if (current->fds[newfd].in_use) {
        fs_close(newfd);
    }

    if (fs_dup_fd(&current->fds[newfd], &current->fds[oldfd]) < 0) return -EMFILE;
    return newfd;
}

int fs_fcntl(int fd, int cmd, uint64_t arg) {
    struct task* current = get_current_task();
    if (!current) return -ESRCH;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -EBADF;

    switch (cmd) {
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_DUPFD_CLOEXEC_LINUX: {
            int minfd = (int)arg;
            if (minfd < 0) return -EINVAL;
            for (int newfd = minfd; newfd < MAX_FDS; newfd++) {
                if (!current->fds[newfd].in_use) {
                    if (fs_dup2(fd, newfd) < 0) return -EMFILE;
                    current->fds[newfd].fd_flags =
                        (cmd != F_DUPFD) ? FD_CLOEXEC : 0;
                    return newfd;
                }
            }
            return -EMFILE;
        }
        case F_GETFD:
            return current->fds[fd].fd_flags;
        case F_SETFD:
            current->fds[fd].fd_flags = (int)arg & FD_CLOEXEC;
            return 0;
        case F_GETFL:
            return current->fds[fd].flags;
        case F_SETFL:
            current->fds[fd].flags = (current->fds[fd].flags & ORTH_O_ACCMODE)
                                   | ((int)arg & ~ORTH_O_ACCMODE);
            return 0;
        default:
            return -EINVAL;
    }
}

int fs_pipe(int pipefd[2]) {
    struct task* current = get_current_task();
    if (!current) return -ESRCH;

    int fd1 = -1, fd2 = -1;
    /* POSIX: 最小の空き fd を割り当てる */
    for (int i = 0; i < MAX_FDS; i++) {
        if (!current->fds[i].in_use) {
            if (fd1 == -1) fd1 = i;
            else if (fd2 == -1) {
                fd2 = i;
                break;
            }
        }
    }
    if (fd1 == -1 || fd2 == -1) return -EMFILE;

    void* phys = pmm_alloc(1);
    if (!phys) return -ENOMEM;
    pipe_t* pipe = (pipe_t*)PHYS_TO_VIRT(phys);
    pipe->read_pos = 0;
    pipe->write_pos = 0;
    pipe->count = 0;
    pipe->ref_count = 2;
    pipe->read_waiter = 0;
    pipe->write_waiter = 0;
    spinlock_init(&pipe->lock);

    fs_file_t* read_file = fs_alloc_file();
    fs_file_t* write_file = fs_alloc_file();
    if (!read_file || !write_file) {
        if (read_file) fs_file_put(read_file);
        if (write_file) fs_file_put(write_file);
        pmm_free((void*)VIRT_TO_PHYS((uint64_t)pipe), 1);
        return -ENOMEM;
    }
    read_file->type = FT_PIPE;
    read_file->ops = &g_pipe_file_ops;
    read_file->private_data = pipe;
    write_file->type = FT_PIPE;
    write_file->ops = &g_pipe_file_ops;
    write_file->private_data = pipe;

    current->fds[fd1].type = FT_PIPE;
    current->fds[fd1].file = read_file;
    current->fds[fd1].data = 0;
    current->fds[fd1].in_use = 1;
    current->fds[fd1].flags = O_RDONLY;
    current->fds[fd1].name[0] = '\0';

    current->fds[fd2].type = FT_PIPE;
    current->fds[fd2].file = write_file;
    current->fds[fd2].data = 0;
    current->fds[fd2].in_use = 1;
    current->fds[fd2].flags = O_WRONLY;
    current->fds[fd2].name[0] = '\0';

    pipefd[0] = fd1;
    pipefd[1] = fd2;
    return 0;
}

int fs_pipe2(int pipefd[2], int flags) {
    int ret;
    if (flags & ~(ORTH_LINUX_O_CLOEXEC | ORTH_LINUX_O_NONBLOCK)) return -EINVAL;
    ret = fs_pipe(pipefd);
    if (ret < 0) return ret;
    if (flags & ORTH_LINUX_O_CLOEXEC) {
        struct task* current = get_current_task();
        if (current) {
            current->fds[pipefd[0]].fd_flags |= FD_CLOEXEC;
            current->fds[pipefd[1]].fd_flags |= FD_CLOEXEC;
        }
    }
    return 0;
}

int fs_fstat(int fd, struct kstat* st) {
    struct task* current = get_current_task();
    if (!current || !st) return -EFAULT;
    
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -EBADF;
    
    file_descriptor_t* f = &current->fds[fd];
    fs_assert_open_file_consistent(f);
    if (fs_fd_type(f) == FT_CONSOLE) {
        kstat_set_defaults(st, fs_default_mode_for_type(KSTAT_MODE_CHR), 0);
        st->dev = FS_DEV_CONSOLE;
        st->ino = (uint64_t)fd + 1U;
        st->rdev = FS_DEV_CONSOLE;
        return 0;
    }
    if (fs_fd_type(f) == FT_PIPE) {
        (void)fs_fd_data_required(f, FT_PIPE);
        kstat_set_defaults(st, KSTAT_MODE_FILE | 0600U, 0);
        st->dev = FS_DEV_PIPE;
        st->ino = ((uint64_t)(uintptr_t)fs_fd_data(f)) >> 4;
        return 0;
    }
    if (fs_fd_type(f) == FT_SOCKET) {
        (void)fs_fd_data_required(f, FT_SOCKET);
        kstat_set_defaults(st, fs_default_mode_for_type(KSTAT_MODE_CHR), 0);
        st->dev = FS_DEV_PIPE;
        st->ino = ((uint64_t)(uintptr_t)fs_fd_data(f)) >> 4;
        return 0;
    }
    if (fs_fd_type(f) == FT_CHARDEV) {
        kstat_set_defaults(st, fs_default_mode_for_type(KSTAT_MODE_CHR), 0);
        st->dev = FS_DEV_ROOT_ARCHIVE;
        st->ino = fs_hash_name(fs_fd_name(f));
        st->rdev = ((fs_fd_aux0(f) & 0xFFU) << 8) | (fs_fd_aux1(f) & 0xFFU);
        return 0;
    }
    if (fs_fd_type(f) == FT_DIR) {
        if (fs_fd_name(f)[0]) return fs_stat_normalized_path(normalize_fs_path(fs_fd_name(f)), st);
        kstat_set_defaults(st, fs_default_mode_for_type(KSTAT_MODE_DIR), (int64_t)fs_fd_size(f));
        st->dev = FS_DEV_SYNTH;
        st->ino = (uint64_t)fd + 1U;
        return 0;
    }
    if (fs_fd_type(f) == FT_USB) {
        if (fs_fd_name(f)[0]) return fs_stat_normalized_path(normalize_fs_path(fs_fd_name(f)), st);
        kstat_set_defaults(st,
                           fs_default_mode_for_type((fs_fd_aux1(f) & 0x10U) ? KSTAT_MODE_DIR : KSTAT_MODE_FILE),
                           (int64_t)fs_fd_size(f));
        st->dev = FS_DEV_USB_FAT;
        st->ino = fs_fd_name(f)[0] ? fs_hash_name(fs_fd_name(f)) : ((uint64_t)fs_fd_aux0(f) + 1U);
        return 0;
    }
    if ((fs_fd_type(f) == FT_RAMFS || fs_fd_type(f) == FT_MODULE || fs_fd_type(f) == FT_XV6FS) && fs_fd_name(f)[0]) {
        if (fs_stat_normalized_path(normalize_fs_path(fs_fd_name(f)), st) == 0) return 0;
        /* path lookup failed (e.g. fd->name truncated to 63 chars).
           fall back to fd-local metadata so fstat does not break dlopen. */
    }
    kstat_set_defaults(st, fs_default_mode_for_type(KSTAT_MODE_FILE), (int64_t)fs_fd_size(f));
    if (fs_fd_type(f) == FT_XV6FS) {
        struct xv6fs_inode* ip = (struct xv6fs_inode*)fs_fd_data_required(f, FT_XV6FS);
        st->dev = FS_DEV_ROOT_ARCHIVE;
        st->ino = ip ? ip->inum : fs_hash_name(fs_fd_name(f));
    } else if (fs_fd_type(f) == FT_RAMFS) {
        st->dev = FS_DEV_RAMFS;
        st->ino = fs_hash_name(fs_fd_name(f));
    } else if (fs_fd_type(f) == FT_MODULE) {
        st->dev = FS_DEV_MODULE;
        st->ino = fs_hash_name(fs_fd_name(f));
    }
    return 0;
}

int fs_getdents(int fd, struct orth_dirent* dirp, size_t count) {
    struct task* current = get_current_task();
    file_descriptor_t* f;
    size_t remaining;
    size_t to_copy;
    if (!current || !dirp) return -EFAULT;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -EBADF;
    f = &current->fds[fd];
    if (fs_fd_type(f) != FT_DIR) return -ENOTDIR;
    fs_assert_open_file_consistent(f);
    if (fs_fd_offset(f) >= fs_fd_size(f)) return 0;
    (void)fs_fd_data_required(f, FT_DIR);
    remaining = fs_fd_size(f) - fs_fd_offset(f);
    to_copy = (count > remaining) ? remaining : count;
    for (size_t i = 0; i < to_copy; i++) {
        ((uint8_t*)dirp)[i] = ((uint8_t*)fs_fd_data(f))[fs_fd_offset(f) + i];
    }
    fs_fd_set_offset(f, fs_fd_offset(f) + to_copy);
    return (int)to_copy;
}

struct linux_dirent_compat {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
};

static uint8_t dirent_type_from_mode(uint32_t mode) {
    if ((mode & 0170000U) == KSTAT_MODE_DIR) return 4;
    if ((mode & 0170000U) == KSTAT_MODE_FILE) return 8;
    if ((mode & 0170000U) == KSTAT_MODE_CHR) return 2;
    return 0;
}

int fs_getdents64(int fd, void* dirp, size_t count) {
    struct task* current = get_current_task();
    file_descriptor_t* f;
    struct orth_dirent* src;
    uint8_t* out = (uint8_t*)dirp;
    size_t out_used = 0;
    size_t index;

    if (!current || !dirp) return -EFAULT;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -EBADF;
    f = &current->fds[fd];
    if (fs_fd_type(f) != FT_DIR) return -ENOTDIR;
    fs_assert_open_file_consistent(f);
    if (fs_fd_offset(f) >= fs_fd_size(f)) return 0;

    src = (struct orth_dirent*)fs_fd_data_required(f, FT_DIR);
    index = fs_fd_offset(f) / sizeof(struct orth_dirent);

    while ((index + 1) * sizeof(struct orth_dirent) <= fs_fd_size(f)) {
        struct linux_dirent_compat ent;
        size_t name_len = 0;
        size_t reclen;
        size_t next_off;

        for (size_t i = 0; i < sizeof(ent); i++) {
            ((uint8_t*)&ent)[i] = 0;
        }
        while (src[index].name[name_len] && name_len + 1 < sizeof(ent.d_name)) {
            ent.d_name[name_len] = src[index].name[name_len];
            name_len++;
        }
        ent.d_name[name_len] = '\0';
        ent.d_ino = (uint64_t)index + 1;
        next_off = (index + 1) * sizeof(struct orth_dirent);
        ent.d_off = (int64_t)next_off;
        ent.d_type = dirent_type_from_mode(src[index].mode);
        reclen = offsetof(struct linux_dirent_compat, d_name) + name_len + 1;
        reclen = (reclen + 7U) & ~7U;
        ent.d_reclen = (uint16_t)reclen;

        if (out_used + reclen > count) break;
        for (size_t i = 0; i < reclen; i++) {
            out[out_used + i] = ((uint8_t*)&ent)[i];
        }
        out_used += reclen;
        index++;
    }

    fs_fd_set_offset(f, index * sizeof(struct orth_dirent));
    return (int)out_used;
}

static int fs_stat_normalized_path(const char* path, struct kstat* st) {
    const char* mount_subpath = 0;
    const struct vfs_mountpoint* mount = 0;
    if (!st || !path) return -EINVAL;
    mount = vfs_find_mountpoint(path, &mount_subpath);

    if (*path == '\0') {
        kstat_set_defaults(st, fs_default_mode_for_type(KSTAT_MODE_DIR), 0);
        st->dev = FS_DEV_SYNTH;
        st->ino = 1;
        return 0;
    }

    if (fs_is_mount_dir(path)) {
        kstat_set_defaults(st, fs_default_mode_for_type(KSTAT_MODE_DIR), 0);
        st->dev = FS_DEV_SYNTH;
        st->ino = fs_hash_name(path);
        return usb_block_device_ready() ? 0 : -ENOENT;
    }
    
    struct ramfs_file* rf = find_ramfs(path);
    if (rf) {
        kstat_from_ramfs(st, rf);
        return 0;
    }

    if (mount && mount->kind == VFS_MOUNT_USB_FAT) {
        struct fat_dir_entry_info ent;
        if (fat_resolve_path(path, &ent) < 0) return -ENOENT;
        kstat_set_defaults(st,
                           fs_default_mode_for_type((ent.attr & 0x10U) ? KSTAT_MODE_DIR : KSTAT_MODE_FILE),
                           (int64_t)ent.size);
        st->dev = FS_DEV_USB_FAT;
        st->ino = fs_hash_name(path);
        return 0;
    }

    {
        if (fs_try_active_root_stat(path, st) == 0) {
            return 0;
        }
        if (fs_root_is_usb_only()) return -ENOENT;
    }

    if (module_dir_exists(path)) {
        kstat_set_defaults(st, fs_default_mode_for_type(KSTAT_MODE_DIR), 0);
        st->dev = FS_DEV_MODULE;
        st->ino = fs_hash_name(path);
        return 0;
    }
    
    if (module_request.response) {
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file* m = module_request.response->modules[i];
            if (strcmp_suffix_fs(m->path, path)) {
                kstat_set_defaults(st, fs_default_mode_for_type(KSTAT_MODE_FILE), (int64_t)m->size);
                st->dev = FS_DEV_MODULE;
                st->ino = fs_hash_name(path);
                return 0;
            }
        }
    }
    return -ENOENT;
}

int fs_stat(const char* path, struct kstat* st) {
    char resolved_path[256];
    if (!st || !path) return -EINVAL;
    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    return fs_stat_normalized_path(normalize_fs_path(resolved_path), st);
}

int fs_fstatat(int dirfd, const char* path, struct kstat* st, int flags) {
    char resolved_path[256];
    (void)flags;
    if (resolve_dirfd_path(dirfd, path, resolved_path, sizeof(resolved_path)) < 0) {
        return -ENOENT;
    }
    {
        int ret = fs_stat(resolved_path, st);
        return ret;
    }
}

#ifndef F_OK
#define F_OK 0
#endif
#ifndef X_OK
#define X_OK 1
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef AT_EACCESS
#define AT_EACCESS 0x0001
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x0002
#endif
int fs_access(const char* path, int mode) {
    return fs_faccessat(-100, path, mode, 0);
}

int fs_faccessat(int dirfd, const char* path, int mode, int flags) {
    struct kstat st;
    uint32_t perm_bits;
    char resolved_path[256];
    if (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW)) return -EINVAL;
    if (resolve_dirfd_path(dirfd, path, resolved_path, sizeof(resolved_path)) < 0) return -ENOENT;
    if (fs_stat(resolved_path, &st) < 0) {
        return -ENOENT;
    }
    if (mode == F_OK) return 0;
    perm_bits = st.mode & 0777U;
    /* The kernel currently exposes only uid/gid 0, so match root access rules. */
    if ((mode & X_OK) == 0) return 0;
    if ((st.mode & 0170000U) == KSTAT_MODE_DIR) return 0;
    if (perm_bits & 0111U) return 0;
    return -EACCES;
}

int fs_utimensat(int dirfd, const char* path, const void* times, int flags) {
    char resolved_path[256];
    struct kstat st;
    struct ramfs_file* rf;
    const char* norm;
    uint64_t now;
    (void)times;

    if (flags & ~AT_SYMLINK_NOFOLLOW) return -EINVAL;

    if (!path) {
        struct task* current = get_current_task();
        file_descriptor_t* f;
        if (!current) return -EINVAL;
        if (dirfd < 0 || dirfd >= MAX_FDS || !current->fds[dirfd].in_use) return -EINVAL;
        f = &current->fds[dirfd];
        if (!fs_fd_name(f)[0]) return -EINVAL;
        if (fs_stat_normalized_path(normalize_fs_path(fs_fd_name(f)), &st) < 0) return -ENOENT;
        rf = find_ramfs(fs_fd_name(f));
        if (rf) {
            now = fs_now_sec();
            rf->atime_sec = now;
            rf->mtime_sec = now;
            rf->ctime_sec = now;
        }
        return 0;
    }

    if (resolve_dirfd_path(dirfd, path, resolved_path, sizeof(resolved_path)) < 0) return -ENOENT;
    if (fs_stat(resolved_path, &st) < 0) return -ENOENT;
    norm = normalize_fs_path(resolved_path);
    rf = find_ramfs(norm);
    if (rf) {
        now = fs_now_sec();
        rf->atime_sec = now;
        rf->mtime_sec = now;
        rf->ctime_sec = now;
    }
    return 0;
}

int64_t fs_readlink(const char* path, char* buf, size_t bufsiz) {
    return fs_readlinkat(-100, path, buf, bufsiz);
}

int64_t fs_readlinkat(int dirfd, const char* path, char* buf, size_t bufsiz) {
    struct kstat dummy;
    (void)buf;
    (void)bufsiz;
    if (!path) return -EFAULT;
    if (fs_fstatat(dirfd, path, &dummy, AT_SYMLINK_NOFOLLOW) < 0) return -ENOENT;
    return -EINVAL;
}

int64_t fs_lseek(int fd, int64_t offset, int whence) {
    struct task* current = get_current_task();
    if (!current) return -ESRCH;
    
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -EBADF;

    file_descriptor_t* f = &current->fds[fd];
    /* console は seek 不可だが従来互換で 0 を返す (fd 番号ではなく型で判定) */
    if (fs_fd_type(f) == FT_CONSOLE) return 0;
    int64_t new_offset = 0;
    
    if (whence == 0) { // SEEK_SET
        new_offset = offset;
    } else if (whence == 1) { // SEEK_CUR
        new_offset = (int64_t)fs_fd_offset(f) + offset;
    } else if (whence == 2) { // SEEK_END
        new_offset = (int64_t)fs_fd_size(f) + offset;
    } else {
        return -EINVAL;
    }
    
    if (new_offset < 0) return -EINVAL;
    fs_fd_set_offset(f, (size_t)new_offset);
    return new_offset;
}

int fs_unlink(const char* path) {
    char resolved_path[256];
    if (!path) return -EFAULT;
    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    path = normalize_fs_path(resolved_path);
    
    exec_cache_invalidate(path);
    if (g_root_source == ROOT_SOURCE_XV6FS && xv6fs_is_mounted()) {
        if (xv6fs_unlink_path(path) == 0) return 0;
    }

    // 現在の簡易実装では Ramfs のファイルのみ削除可能とする
    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        if (ramfs_table[i].in_use && strcmp_exact(ramfs_table[i].name, path)) {
            if ((ramfs_table[i].mode & 0170000U) == KSTAT_MODE_DIR) return -EISDIR;
            ramfs_table[i].in_use = 0;
            if (ramfs_table[i].data && ramfs_table[i].capacity) {
                void* phys_addr = (void*)VIRT_TO_PHYS((uint64_t)ramfs_table[i].data);
                pmm_free(phys_addr, (int)(ramfs_table[i].capacity / PAGE_SIZE));
                ramfs_table[i].data = NULL;
            }
            ramfs_table[i].size = 0;
            ramfs_table[i].capacity = 0;
            ramfs_table[i].mode = 0;
            ramfs_table[i].uid = 0;
            ramfs_table[i].gid = 0;
            ramfs_table[i].nlink = 0;
            ramfs_table[i].atime_sec = 0;
            ramfs_table[i].mtime_sec = 0;
            ramfs_table[i].ctime_sec = 0;
            ramfs_table[i].name[0] = '\0';
            return 0; // 成功
        }
    }
    
    return -ENOENT; // ファイルが見つからないか、TAR 内など削除できないファイル
}

int fs_rename(const char* oldpath, const char* newpath) {
    char old_resolved[256];
    char new_resolved[256];
    const char* old_norm;
    const char* new_norm;
    struct ramfs_file* rf;
    if (!oldpath || !newpath) return -EFAULT;
    exec_cache_invalidate(oldpath);
    exec_cache_invalidate(newpath);
    resolve_task_path(oldpath, old_resolved, sizeof(old_resolved));
    resolve_task_path(newpath, new_resolved, sizeof(new_resolved));
    old_norm = normalize_fs_path(old_resolved);
    new_norm = normalize_fs_path(new_resolved);
    if (*old_norm == '\0' || *new_norm == '\0') return -ENOENT;
    if (strcmp_exact(old_norm, new_norm)) return 0;
    if (find_ramfs(new_norm)) return -EEXIST;
    rf = find_ramfs(old_norm);
    if (!rf) return -ENOENT;
    int i = 0;
    for (; new_norm[i] && i + 1 < (int)sizeof(rf->name); i++) {
        rf->name[i] = new_norm[i];
    }
    rf->name[i] = '\0';
    rf->ctime_sec = fs_now_sec();
    return 0;
}

int fs_chmod(const char* path, uint32_t mode) {
    char resolved_path[256];
    const char* norm;
    struct ramfs_file* rf;
    if (!path) return -EFAULT;
    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    norm = normalize_fs_path(resolved_path);
    if (*norm == '\0') return -ENOENT;
    rf = find_ramfs(norm);
    if (rf) {
        rf->mode = (rf->mode & 0170000U) | (mode & 07777U);
        rf->ctime_sec = fs_now_sec();
        return 0;
    }
    if (g_root_source == ROOT_SOURCE_XV6FS && xv6fs_is_mounted()) {
        return xv6fs_chmod_path(norm, mode) == 0 ? 0 : -ENOENT;
    }
    return -ENOENT;
}

/* mknod: 現状は S_IFIFO (named pipe) のみ対応。 */
int fs_mknod(const char* path, uint32_t mode, uint64_t dev) {
    char resolved_path[256];
    const char* norm;
    (void)dev;
    if (!path || path[0] == '\0') return -ENOENT;
    if ((mode & 0170000U) != KSTAT_MODE_FIFO) return -22; /* EINVAL */
    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    norm = normalize_fs_path(resolved_path);
    if (*norm == '\0') return -17; /* EEXIST */
    if (find_ramfs(norm)) return -17;
    if (g_root_source == ROOT_SOURCE_XV6FS && xv6fs_is_mounted()) {
        return xv6fs_mknod_fifo(norm, (int)(mode & 07777U));
    }
    return -ENOENT;
}

int fs_mkdir(const char* path, int mode) {
    char resolved_path[256];
    const char* norm;
    struct kstat st;
    if (!path || path[0] == '\0') return -ENOENT;
    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    norm = normalize_fs_path(resolved_path);
    if (*norm == '\0') return -EEXIST;

    if (find_ramfs(norm)) return -EEXIST;
    if (g_root_source == ROOT_SOURCE_XV6FS && xv6fs_is_mounted()) {
        if (fs_try_active_root_stat(norm, &st) == 0) {
            if ((st.mode & 0170000U) == KSTAT_MODE_DIR) return -EEXIST;
            return -EEXIST;
        }
        return xv6fs_mkdir_path(norm, mode) == 0 ? 0 : -ENOENT;
    }
    return create_ramfs_dir(norm, (uint32_t)mode) ? 0 : -ENOENT;
}

int fs_mkdirat(int dirfd, const char* path, int mode) {
    char resolved_path[256];
    if (resolve_dirfd_path(dirfd, path, resolved_path, sizeof(resolved_path)) < 0) return -ENOENT;
    return fs_mkdir(resolved_path, mode);
}

int fs_rmdir(const char* path) {
    char resolved_path[256];
    const char* norm;
    if (!path || path[0] == '\0') return -ENOENT;
    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    norm = normalize_fs_path(resolved_path);
    if (*norm == '\0') return -ENOENT;

    if (g_root_source == ROOT_SOURCE_XV6FS && xv6fs_is_mounted()) {
        if (xv6fs_rmdir_path(norm) == 0) return 0;
    }

    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        if (!ramfs_table[i].in_use) continue;
        if (!strcmp_exact(ramfs_table[i].name, norm)) continue;
        if ((ramfs_table[i].mode & 0170000U) != KSTAT_MODE_DIR) return -ENOTDIR;
        ramfs_table[i].in_use = 0;
        ramfs_table[i].size = 0;
        ramfs_table[i].data = NULL;
        ramfs_table[i].mode = 0;
        ramfs_table[i].uid = 0;
        ramfs_table[i].gid = 0;
        ramfs_table[i].nlink = 0;
        ramfs_table[i].atime_sec = 0;
        ramfs_table[i].mtime_sec = 0;
        ramfs_table[i].ctime_sec = 0;
        ramfs_table[i].name[0] = '\0';
        return 0;
    }
    return -ENOENT;
}

int fs_sync(void) {
    if (g_root_source == ROOT_SOURCE_XV6FS && xv6fs_is_mounted()) {
        return xv6fs_sync();
    }
    return 0;
}

int fs_unlinkat(int dirfd, const char* path, int flags) {
    char resolved_path[256];
    if (resolve_dirfd_path(dirfd, path, resolved_path, sizeof(resolved_path)) < 0) return -ENOENT;
    if (flags & ORTH_AT_REMOVEDIR) return fs_rmdir(resolved_path);
    return fs_unlink(resolved_path);
}

int fs_chdir(const char* path) {
    struct task* current = get_current_task();
    struct kstat st;
    char resolved_path[256];
    const char* norm;
    size_t i = 0;
    if (!current || !path || path[0] == '\0') return -ENOENT;
    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    if (fs_stat(resolved_path, &st) < 0) return -ENOENT;
    if ((st.mode & 0170000U) != KSTAT_MODE_DIR) return -ENOTDIR;
    norm = normalize_fs_path(resolved_path);
    current->cwd[i++] = '/';
    while (*norm && i + 1 < sizeof(current->cwd)) {
        current->cwd[i++] = *norm++;
    }
    current->cwd[i] = '\0';
    return 0;
}

int fs_fchdir(int fd) {
    struct task* current = get_current_task();
    struct kstat st;
    file_descriptor_t* f;
    const char* norm;
    size_t i = 0;
    if (!current) return -ESRCH;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -EBADF;
    f = &current->fds[fd];
    if (fs_fd_type(f) != FT_DIR) return -ENOTDIR;
    if (fs_fd_name(f)[0] == '\0') return -EBADF;
    norm = normalize_fs_path(fs_fd_name(f));
    if (fs_stat_normalized_path(norm, &st) < 0) return -ENOENT;
    if ((st.mode & 0170000U) != KSTAT_MODE_DIR) return -ENOTDIR;
    current->cwd[i++] = '/';
    while (*norm && i + 1 < sizeof(current->cwd)) {
        current->cwd[i++] = *norm++;
    }
    current->cwd[i] = '\0';
    return 0;
}

int fs_getcwd(char* buf, size_t size) {
    struct task* current = get_current_task();
    size_t i = 0;
    if (!current || !buf || size == 0) return -EFAULT;
    while (current->cwd[i] && i + 1 < size) {
        buf[i] = current->cwd[i];
        i++;
    }
    if (current->cwd[i] != '\0' && i + 1 >= size) return -ERANGE;
    buf[i] = '\0';
    return (int)(i + 1);
}

int fs_get_file_data(const char* path, void** data, size_t* size) {
    char resolved_path[256];
    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    path = normalize_fs_path(resolved_path);

    struct ramfs_file* rf = find_ramfs(path);
    if (rf) {
        *data = rf->data;
        *size = rf->size;
        return 0;
    }

    if (g_root_source == ROOT_SOURCE_XV6FS && xv6fs_is_mounted()) {
        uint32_t xv6_mode = 0;
        uint64_t xv6_size = 0;
        if (xv6fs_stat_path(path, &xv6_mode, &xv6_size, 0, 0) < 0) goto try_module;
        if ((xv6_mode & 0170000U) != KSTAT_MODE_FILE) return -1;
        if (data) {
            /* exec キャッシュヒット: ディスク I/O を回避 */
            void* cached = exec_cache_lookup(path, (size_t)xv6_size);
            if (cached) {
                *data = cached;
                if (size) *size = (size_t)xv6_size;
                return 0;
            }
            size_t npages = (xv6_size + PAGE_SIZE - 1U) / PAGE_SIZE;
            if (npages == 0) npages = 1;
            void* buf = PHYS_TO_VIRT(pmm_alloc((int)npages));
            if (!buf) return -1;
            struct xv6fs_inode* ip = xv6fs_namei(path);
            if (!ip) { pmm_free((void*)VIRT_TO_PHYS((uint64_t)buf), (int)npages); return -1; }
            xv6fs_ilock(ip);
            xv6fs_readi(ip, buf, 0, (uint32_t)xv6_size);
            xv6fs_iunlock(ip);
            xv6fs_iput(ip);
            /* 登録できれば cache 所有 (in_use=1)。載らなければ buf は呼び出し側
             * 所有のまま (fs_free_exec_buffer が pmm_free する)。 */
            exec_cache_insert(path, buf, (size_t)xv6_size, npages);
            *data = buf;
        }
        if (size) *size = (size_t)xv6_size;
        return 0;
    }
try_module:
    if (fs_root_is_usb_only()) return -1;

    if (module_request.response) {
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file* m = module_request.response->modules[i];
            if (strcmp_suffix_fs(m->path, path)) {
                *data = m->address;
                *size = m->size;
                return 0;
            }
        }
    }
    return -1;
}

/* Free a buffer returned by fs_get_file_data if it was heap-allocated.
 * RAMFS and module pointers are not freed (they are not owned by the caller).
 * xv6fs content buffers (pmm-allocated by fs_get_file_data) are freed here. */
void fs_free_exec_buffer(const char* path, void* data, size_t size) {
    if (!data || size == 0) return;
    /* exec キャッシュ所有のバッファは解放しない。in_use を減らし、
     * 無効化済み (stale) かつ最後の利用者ならここで解放する。 */
    {
        uint64_t cflags = spin_lock_irqsave(&g_exec_cache_lock);
        for (int i = 0; i < EXEC_CACHE_MAX; i++) {
            struct exec_cache_entry* e = &g_exec_cache[i];
            if (e->valid && e->buf == data) {
                if (e->in_use > 0) e->in_use--;
                if (e->stale && e->in_use == 0) exec_cache_drop_locked(e);
                spin_unlock_irqrestore(&g_exec_cache_lock, cflags);
                return;
            }
        }
        spin_unlock_irqrestore(&g_exec_cache_lock, cflags);
    }
    char resolved[256];
    resolve_task_path(path, resolved, sizeof(resolved));
    const char* norm = normalize_fs_path(resolved);
    if (find_ramfs(norm)) return;
    /* Module pointers are direct references to Limine memory — skip them. */
    if (module_request.response) {
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file* m = module_request.response->modules[i];
            if (m->address == data) return;
        }
    }
    size_t pages = (size + PAGE_SIZE - 1U) / PAGE_SIZE;
    if (pages) pmm_free((void*)VIRT_TO_PHYS((uint64_t)data), (int)pages);
}

void sys_ls(void) {
    puts("--- Files in Ramfs ---\n");
    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        if (ramfs_table[i].in_use) {
            puts(ramfs_table[i].name);
            puts(" (Ramfs, size: 0x"); puthex(ramfs_table[i].size); puts(")\n");
        }
    }
    if (module_request.response) {
        puts("--- Files in Modules ---\n");
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file* m = module_request.response->modules[i];
            puts(m->path); puts(" (Module)\n");
        }
    }
    if (usb_block_device_ready()) {
        const struct vfs_mountpoint* mounts[VFS_MAX_MOUNTPOINTS];
        size_t mount_count = vfs_list_mountpoints(mounts, VFS_MAX_MOUNTPOINTS);
        for (size_t i = 0; i < mount_count; i++) {
            puts(mounts[i]->path);
            puts(" (");
            if (mounts[i]->kind == VFS_MOUNT_USB_FAT) puts("USB mount");
            else puts("mount");
            puts(")\n");
        }
    }
}
