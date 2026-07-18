#include <stddef.h>
#include <stdint.h>
#include "fs.h"
#include "pmm.h"
#include "riscv64/boot.h"
#include "riscv64/bootstrap_user.h"
#include "task.h"
#include "vmm.h"

struct riscv64_linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
};

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
    if (!riscv64_fs_path_eq(resolved, "/")) return -1;
    if (riscv64_fs_append_dirent(dirents, max_count, &count, ".", KSTAT_MODE_DIR | 0755U, 0) < 0) return -1;
    if (riscv64_fs_append_dirent(dirents, max_count, &count, "..", KSTAT_MODE_DIR | 0755U, 0) < 0) return -1;
    if (riscv64_fs_append_dirent(dirents, max_count, &count, "bootstrap-user", KSTAT_MODE_FILE | 0644U, 0) < 0) return -1;
    *out_count = count;
    return 0;
}

static int riscv64_fs_resolve_path(const char* path, char* out, size_t size) {
    struct task* current = get_current_task();
    size_t i = 0;
    size_t j = 0;

    if (!path || !out || size == 0) return -1;
    if (path[0] == '/') {
        while (path[j] && i + 1 < size) out[i++] = path[j++];
        out[i] = '\0';
        return 0;
    }
    if (!current || current->cwd[0] == '\0') return -1;
    while (current->cwd[j] && i + 1 < size) out[i++] = current->cwd[j++];
    if (i > 0 && out[i - 1] != '/' && i + 1 < size) out[i++] = '/';
    j = 0;
    while (path[j] && i + 1 < size) out[i++] = path[j++];
    out[i] = '\0';
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
    (void)mode;

    if (!current || !path) return -1;
    if (riscv64_fs_resolve_path(path, resolved, sizeof(resolved)) < 0) return -1;

    for (int i = 3; i < MAX_FDS; i++) {
        if (!current->fds[i].in_use) {
            fd = i;
            break;
        }
    }
    if (fd < 0) return -1;

    if ((flags & O_DIRECTORY) != 0) {
        if (!riscv64_fs_path_eq(resolved, "/")) return -1;
        {
            void* dir_page = pmm_alloc(1);
            struct orth_dirent* dirents;
            size_t count = 0;
            if (!dir_page) return -1;
            dirents = (struct orth_dirent*)PHYS_TO_VIRT(dir_page);
            for (size_t i = 0; i < PAGE_SIZE; i++) ((uint8_t*)dirents)[i] = 0;
            if (riscv64_fs_build_dirents(resolved, dirents, PAGE_SIZE / sizeof(struct orth_dirent), &count) < 0) {
                pmm_free(dir_page, 1);
                return -1;
            }
            current->fds[fd].data = dirents;
            current->fds[fd].size = count * sizeof(struct orth_dirent);
            current->fds[fd].aux0 = 1;
        }
        current->fds[fd].type = FT_DIR;
        current->fds[fd].offset = 0;
        current->fds[fd].in_use = 1;
        current->fds[fd].flags = flags;
        current->fds[fd].aux1 = 0;
        current->fds[fd].name[0] = '/';
        current->fds[fd].name[1] = '\0';
        return fd;
    }

    if (fs_get_file_data(resolved, &file_data, &file_size) < 0) return -1;

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
    if (riscv64_fs_resolve_dirfd_path(dirfd, path, resolved, sizeof(resolved)) < 0) return -1;
    return sys_open(resolved, flags, mode);
}

int64_t sys_write(int fd, const void* buf, size_t count) {
    struct task* current = get_current_task();
    const uint8_t* src = (const uint8_t*)buf;

    if (!current || !buf) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    if (current->fds[fd].type != FT_CONSOLE) return -1;

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

    if (!current || !buf) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    f = &current->fds[fd];
    if (f->type == FT_CONSOLE) {
        uint8_t* dst = (uint8_t*)buf;
        size_t read_count = 0;
        if (count == 0) return 0;
        while (read_count == 0) {
            int got;
            riscv64_console_poll_input();
            got = riscv64_console_read((char*)dst, (int)count);
            if (got <= 0) {
                task_mark_sleeping(current);
                riscv64_console_set_waiter(current);
                kernel_yield();
                riscv64_console_clear_waiter(current);
                continue;
            }
            read_count = (size_t)got;
            if (fd == 0) {
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
    if (f->type == FT_DIR) return -1;
    if (f->type != FT_MODULE && f->type != FT_TAR) return -1;
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

    if (!path || !st) return -1;
    if (riscv64_fs_resolve_path(path, resolved, sizeof(resolved)) < 0) return -1;

    if (riscv64_fs_path_eq(resolved, "/")) {
        riscv64_fs_kstat_defaults(st, KSTAT_MODE_DIR | 0755U, 0);
        st->ino = 1;
        return 0;
    }

    if (fs_get_file_data(resolved, &file_data, &file_size) < 0) return -1;
    riscv64_fs_kstat_defaults(st, KSTAT_MODE_FILE | 0644U, (int64_t)file_size);
    st->ino = ((uint64_t)(uintptr_t)file_data) >> 4;
    return 0;
}

int sys_fstat(int fd, struct kstat* st) {
    struct task* current = get_current_task();
    file_descriptor_t* f;

    if (!current || !st) return -1;
    if (fd == 0 || fd == 1 || fd == 2) {
        riscv64_fs_kstat_defaults(st, KSTAT_MODE_CHR | 0666U, 0);
        st->dev = 1;
        st->ino = (uint64_t)fd + 1U;
        st->rdev = 1;
        return 0;
    }
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    f = &current->fds[fd];

    if (f->type == FT_CONSOLE) {
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
    if ((f->type == FT_MODULE || f->type == FT_TAR) && f->name[0] != '\0') {
        return sys_stat(f->name, st);
    }
    return -1;
}

int sys_fstatat(int dirfd, const char* path, struct kstat* st, int flags) {
    char resolved[256];
    (void)flags;
    if (riscv64_fs_resolve_dirfd_path(dirfd, path, resolved, sizeof(resolved)) < 0) return -1;
    return sys_stat(resolved, st);
}

int sys_chdir(const char* path) {
    struct task* current = get_current_task();
    struct kstat st;
    char resolved[256];

    if (!current || !path || path[0] == '\0') return -1;
    if (riscv64_fs_resolve_path(path, resolved, sizeof(resolved)) < 0) return -1;
    if (sys_stat(resolved, &st) < 0) return -1;
    if ((st.mode & 0170000U) != KSTAT_MODE_DIR) return -1;
    riscv64_fs_strcpy(current->cwd, resolved, sizeof(current->cwd));
    return 0;
}

int sys_fchdir(int fd) {
    struct task* current = get_current_task();
    file_descriptor_t* f;

    if (!current) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    f = &current->fds[fd];
    if (f->type != FT_DIR || f->name[0] == '\0') return -1;
    return sys_chdir(f->name);
}

int sys_close(int fd) {
    struct task* current = get_current_task();
    file_descriptor_t* desc;
    struct task* read_waiter = 0;
    struct task* write_waiter = 0;
    int free_pipe = 0;

    if (!current) return -1;
    if (fd < 0 || fd >= MAX_FDS) return -1;
    desc = &current->fds[fd];
    if (!desc->in_use) return -1;

    if (desc->type == FT_PIPE) {
        pipe_t* pipe = (pipe_t*)desc->data;
        if (pipe) {
            uint64_t flags = spin_lock_irqsave(&pipe->lock);
            if (pipe->ref_count > 0) pipe->ref_count--;
            read_waiter = pipe->read_waiter;
            write_waiter = pipe->write_waiter;
            pipe->read_waiter = 0;
            pipe->write_waiter = 0;
            free_pipe = (pipe->ref_count == 0);
            spin_unlock_irqrestore(&pipe->lock, flags);
            if (read_waiter && read_waiter->state == TASK_SLEEPING) task_wake(read_waiter);
            if (write_waiter && write_waiter->state == TASK_SLEEPING) task_wake(write_waiter);
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
    desc->name[0] = '\0';
    return 0;
}

int sys_getdents(int fd, struct orth_dirent* dirp, size_t count) {
    struct task* current = get_current_task();
    file_descriptor_t* f;
    size_t remaining;
    size_t to_copy;

    if (!current || !dirp) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    f = &current->fds[fd];
    if (f->type != FT_DIR || !f->data) return -1;
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

    if (!current || !dirp) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    f = &current->fds[fd];
    if (f->type != FT_DIR || !f->data) return -1;
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
