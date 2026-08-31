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
/* **O_DIRECTORY だけはアーキで値が違う。**
 *
 *   asm-generic (x86_64 / riscv64)   0200000 = 0x10000
 *   aarch64 / arm                    0040000 = 0x04000
 *
 * Linux の arch/arm64/include/uapi/asm/fcntl.h が asm-generic の既定を
 * 上書きしているため。musl もそれに合わせている (bits/fcntl.h を実測)。
 *
 * **システムコール番号と struct のレイアウトが同じでも、O_* フラグは
 * 別物**という例外。合っていないと musl の opendir() が O_DIRECTORY 無しの
 * open に見え、ディレクトリを開こうとして EISDIR で弾かれる。
 * 症状はディレクトリが読めないことで、**GNU make は readdir の結果で
 * ファイルの有無を判断する**ので「Makefile が無い」と言って止まる。
 *
 * 両方受けても害は無い (どちらの値も他のフラグと衝突しない) ので、
 * arch ごとに切らずに両方を見る */
#define O_DIRECTORY      0x10000    /* asm-generic (x86_64 / riscv64) */
#define O_DIRECTORY_ARM  0x04000    /* aarch64 / arm */
#define FD_CLOEXEC  1

/* **POSIX の PIPE_BUF (4096) を下回ってはいけない。**
 *
 * PIPE_BUF 以下の write は「詰まらない」ことが保証されているので、
 * busybox ash はヒアドキュメントがそれ以下なら**読み手を用意する前に
 * 親プロセスで書き込む** (openhere)。ここが 4000 だったころは
 * 4001〜4096 バイトのヒアドキュメントが恒久デッドロックした。
 *
 * 実機で刻んで確認 (2026-08-29):
 *   本体 3900 B ok / 4000 B ok / **4050 B 死 (60 秒後も 0 バイト、CPU 0%)**
 *   / 4100 B ok (PIPE_BUF 超なので ash が書き手を fork する) / 20000 B ok */
#define PIPE_BUF_SIZE 4096

/*
 * 待ち行列。以前は待ち手を 1 つしか覚えられず、後から待った側が前を上書きして
 * いた。上書きされた側は誰にも起こされないので、複数タスクが同じ fd を待つと
 * (ppoll が典型) 取り残される。
 *
 * リストではなく固定長の配列にしてあるのは、待ち行列のノードを struct task に
 * 持たせずに済ませるため。1 つの fd を待つタスクがこの数を超えることは現状の
 * 規模では無く、溢れた場合は追加が失敗する (呼び出し側が期限付きで寝る)。
 *
 * 出し入れは必ず所有者のロック (pipe->lock / コンソールロック) の中で行うこと。
 * 起こすのはロックの外 (task_wake は g_task_lock と IPI を伴う)。
 */
/* pipe_t は pmm_alloc(1) の 1 ページに収める前提で、旧レイアウト (待ち手 2 本)
 * の時点で残りは 56 バイトしかない。ここを大きくするとページを溢れて隣を
 * 壊すので、下の _Static_assert で必ず気付けるようにしてある。
 * 1 つの fd を 4 タスク以上が同時に待つことは現状の規模では無い */
#define FS_WAITQ_MAX 4
typedef struct {
    struct task* waiters[FS_WAITQ_MAX];
} fs_waitq_t;

/* 登録済みなら何もしない。空きが無ければ 0 を返す */
static inline int fs_waitq_add(fs_waitq_t* q, struct task* t) {
    int free_slot = -1;
    if (!q || !t) return 0;
    for (int i = 0; i < FS_WAITQ_MAX; i++) {
        if (q->waiters[i] == t) return 1;
        if (!q->waiters[i] && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return 0;
    q->waiters[free_slot] = t;
    return 1;
}

static inline void fs_waitq_remove(fs_waitq_t* q, struct task* t) {
    if (!q || !t) return;
    for (int i = 0; i < FS_WAITQ_MAX; i++) {
        if (q->waiters[i] == t) q->waiters[i] = 0;
    }
}

/* 待ち手を全部取り出して空にする。戻り値は取り出した数。
 * 起床はロックを離してから out[] に対して行うこと */
static inline int fs_waitq_take_all(fs_waitq_t* q, struct task** out, int max) {
    int n = 0;
    if (!q || !out) return 0;
    for (int i = 0; i < FS_WAITQ_MAX; i++) {
        if (!q->waiters[i]) continue;
        if (n < max) out[n++] = q->waiters[i];
        q->waiters[i] = 0;
    }
    return n;
}

static inline void fs_waitq_init(fs_waitq_t* q) {
    if (!q) return;
    for (int i = 0; i < FS_WAITQ_MAX; i++) q->waiters[i] = 0;
}

typedef struct {
    spinlock_t lock;
    char buffer[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;
    int ref_count;
    /* ref_count は読み端と書き端の合計なので、これだけでは EOF も EPIPE も
     * 判定できない。1R+1W ちょうどのときしか ref_count<2 が成立しないため、
     * 片側が 2 本になると書き手が全滅しても reader が寝たままになる。
     * 端ごとの本数は下の 2 つで数える。EOF は writers==0、EPIPE は readers==0
     * で判定すること。ref_count はページ解放の判定にだけ使う。
     * 数え方はアーキテクチャで違う:
     *   kernel/riscv64/fs.c  fd ごとに数える (fd->aux1 が 0=読み / 1=書き)
     *   kernel/fs.c (x86)    file object ごとに数える (file->aux0 の
     *                        bit0=読み手 / bit1=書き手。FIFO の O_RDWR は
     *                        両方立つので 1 参照で 1R+1W と数える)
     * pipe_t は 1 ページ上限が厳しいので uint16_t にしてある (fd 数の上限は
     * MAX_FDS なので 16 bit で足りる)。 */
    uint16_t readers;
    uint16_t writers;
    fs_waitq_t read_wq;
    fs_waitq_t write_wq;
} pipe_t;

/* pipe_t は PIPE_PAGES ページ丸ごとで確保する (kernel/fs.c と
 * kernel/riscv64/fs.c の pmm_alloc(PIPE_PAGES))。溢れると隣のページを
 * 踏み潰し、パイプを使った瞬間に固まる。
 *
 * **1 ページに収めようとして buffer を 4000 にしていたのが元の不具合。**
 * POSIX の PIPE_BUF (4096) を下回れないので、枠のほうを 2 ページにした */
#define PIPE_PAGES 2
_Static_assert(sizeof(pipe_t) <= PIPE_PAGES * 4096,
               "pipe_t が PIPE_PAGES ページに収まらない");

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
#define KSTAT_MODE_LNK  0120000

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
/* symlinkat(2) (N-6, 2026-08-31)。xv6fs にのみ作れる (ramfs は未対応) */
int fs_symlink(const char* target, const char* linkpath);
int fs_symlinkat(const char* target, int dirfd, const char* linkpath);
int64_t fs_lseek(int fd, int64_t offset, int whence);
int fs_getdents(int fd, struct orth_dirent* dirp, size_t count);
int fs_getdents64(int fd, void* dirp, size_t count);
int fs_chdir(const char* path);
int fs_fchdir(int fd);
int fs_getcwd(char* buf, size_t size);
int fs_truncate(const char* path, uint64_t length);
int fs_ftruncate(int fd, uint64_t length);
int fs_utimensat(int dirfd, const char* path, const void* times, int flags);
/* statfs(2) の結果。**単位はブロック** (bsize バイト) */
struct orth_statfs {
    uint64_t bsize;
    uint64_t blocks;
    uint64_t bfree;
    uint64_t bavail;
    uint64_t files;
    uint64_t ffree;
    uint64_t namelen;
};
int fs_statfs(const char* path, struct orth_statfs* out);
int fs_sync(void);
int fs_unlink(const char* path);
int fs_unlinkat(int dirfd, const char* path, int flags);
int fs_rename(const char* oldpath, const char* newpath);
int fs_renameat(int olddirfd, const char* oldpath,
                int newdirfd, const char* newpath, unsigned int flags);
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
