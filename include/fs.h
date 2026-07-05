#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>
#include "spinlock.h"

#define MAX_FDS 256

struct task;

typedef enum {
    FT_UNUSED,
    FT_CONSOLE, // stdin/stdout/stderr
    FT_MODULE,  // Limine 直接ロード
    FT_RAMFS,   // メモリ上の書き込み可能ファイル
    FT_PIPE,    // パイプ
    FT_SOCKET,  // lwIP-backed socket
    FT_USB,     // USB FAT file
    FT_XV6FS,   // file inside xv6fs root image
    FT_RAWDEV,  // raw output device (e.g. /dev/kout)
    FT_CHARDEV, // character device backed by an xv6fs T_DEVICE inode (aux0=major, aux1=minor)
    FT_DIR      // synthesized directory listing
} file_type_t;

struct fs_file;

typedef struct fs_file_ops {
    void (*release)(struct fs_file* file);
} fs_file_ops_t;

typedef struct fs_file {
    int ref_count;
    file_type_t type;
    size_t size;
    size_t offset;
    const fs_file_ops_t* ops;
    void* private_data;
    uint32_t aux0;
    uint32_t aux1;
    char path[64];
} fs_file_t;

/* Linux/musl ABI values. The userland is musl-only, so these must match
 * what musl passes; the old newlib values (O_APPEND=0x8, O_CREAT=0x200,
 * O_TRUNC=0x400) collided with musl's bits (musl O_APPEND=0x400 aliased
 * the old O_TRUNC, so `>>` truncated the file). */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_DIRECTORY 0x10000
#define FD_CLOEXEC  1

#define PIPE_BUF_SIZE 4000

typedef struct {
    spinlock_t lock;
    char buffer[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;
    int ref_count;
    struct task* read_waiter;
    struct task* write_waiter;
} pipe_t;

typedef struct {
    file_type_t type;
    fs_file_t* file; // shared open-file object for backends migrated in Phase 2
    void* data;      // ファイルデータへのポインタ (FT_RAMFS の場合は malloc 領域, FT_PIPE の場合は pipe_t)
    size_t size;     // ファイルサイズ
    size_t offset;   // 現在の読み取り/書き込みオフセット
    int in_use;      // 使用中フラグ
    int flags;       // O_RDONLY, O_WRONLY, O_RDWR
    int fd_flags;    // FD_CLOEXEC などの descriptor flags
    char name[64];   // ファイル名 (Ramfs用)
    uint32_t aux0;   // backend-specific metadata
    uint32_t aux1;   // backend-specific metadata
} file_descriptor_t;

#define KSTAT_MODE_FILE 0100000
#define KSTAT_MODE_DIR  0040000
#define KSTAT_MODE_CHR  0020000
#define KSTAT_MODE_FIFO 0010000

struct kstat {
    uint64_t dev;
    uint64_t ino;
    uint64_t nlink;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t pad0;
    uint64_t rdev;
    int64_t size;
    int64_t blksize;
    int64_t blocks;
    int64_t atime_sec;
    int64_t atime_nsec;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    int64_t ctime_sec;
    int64_t ctime_nsec;
    int64_t unused[3];
};

#ifndef ORTH_DIRENT_DEFINED
#define ORTH_DIRENT_DEFINED
struct orth_dirent {
    uint32_t mode;
    uint32_t size;
    char name[248];
};
#endif

void fs_init(void);
int fs_open(const char* path, int flags, int mode);
int fs_openat(int dirfd, const char* path, int flags, int mode);
int64_t fs_read(int fd, void* buf, size_t count);
int64_t fs_write(int fd, const void* buf, size_t count);
int fs_close(int fd);
int fs_fcntl(int fd, int cmd, uint64_t arg);
int fs_pipe(int pipefd[2]);
int fs_pipe2(int pipefd[2], int flags);
int fs_dup2(int oldfd, int newfd);
int fs_fstat(int fd, struct kstat* st);
int fs_stat(const char* path, struct kstat* st);
int fs_fstatat(int dirfd, const char* path, struct kstat* st, int flags);
int fs_access(const char* path, int mode);
int fs_faccessat(int dirfd, const char* path, int mode, int flags);
int64_t fs_readlink(const char* path, char* buf, size_t bufsiz);
int64_t fs_readlinkat(int dirfd, const char* path, char* buf, size_t bufsiz);
int64_t fs_lseek(int fd, int64_t offset, int whence);
int fs_getdents(int fd, struct orth_dirent* dirp, size_t count);
int fs_getdents64(int fd, void* dirp, size_t count);
int fs_chdir(const char* path);
int fs_fchdir(int fd);
int fs_getcwd(char* buf, size_t size);
int fs_truncate(const char* path, uint64_t length);
int fs_ftruncate(int fd, uint64_t length);
int fs_utimensat(int dirfd, const char* path, const void* times, int flags);
int fs_sync(void);
int fs_unlink(const char* path);
int fs_unlinkat(int dirfd, const char* path, int flags);
int fs_rename(const char* oldpath, const char* newpath);
int fs_chmod(const char* path, uint32_t mode);
int fs_mkdir(const char* path, int mode);
int fs_mknod(const char* path, uint32_t mode, uint64_t dev);
int fs_mkdirat(int dirfd, const char* path, int mode);
int fs_rmdir(const char* path);
void sys_ls(void);
int fs_clone_fd(file_descriptor_t* dst, const file_descriptor_t* src);
int fs_dup_fd(file_descriptor_t* dst, const file_descriptor_t* src);
void fs_release_fd(file_descriptor_t* fd);
void fs_close_cloexec_descriptors(struct task* task);
int fs_init_console_fd(file_descriptor_t* fd, int flags);
file_type_t fs_fd_type(const file_descriptor_t* fd);
void* fs_fd_data(const file_descriptor_t* fd);
size_t fs_fd_size(const file_descriptor_t* fd);
size_t fs_fd_offset(const file_descriptor_t* fd);
void fs_fd_set_offset(file_descriptor_t* fd, size_t offset);
void fs_fd_set_size(file_descriptor_t* fd, size_t size);
uint32_t fs_fd_aux0(const file_descriptor_t* fd);
uint32_t fs_fd_aux1(const file_descriptor_t* fd);
const char* fs_fd_name(const file_descriptor_t* fd);
int fs_mount_module_root(void);
int fs_mount_xv6fs_root(void);
int fs_get_mount_status(char* buf, size_t size);

#endif
