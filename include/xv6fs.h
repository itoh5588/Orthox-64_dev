#ifndef XV6FS_H
#define XV6FS_H

#include <stdint.h>
#include <stddef.h>
#include "spinlock.h"
#include "wait.h"

/* ------------------------------------------------------------------ */
/* sleeplock (xv6 の sleeplock 相当)                                   */
/*                                                                     */
/* ディスク I/O を跨いで保持されるロック (inode / バッファ) は          */
/* スピンではなく待機列で眠る。保持者が I/O 待ちで sleep しても、       */
/* 競合側は wait_event 経由で CPU を譲るためデッドロックしない。        */
/* ------------------------------------------------------------------ */

struct xv6_sleeplock {
    struct wait_queue wq;   /* wq.lock がフィールド保護を兼ねる */
    int locked;
    int holder_pid;         /* デバッグ用 */
};

void xv6_sleeplock_init(struct xv6_sleeplock *s);
void xv6_sleep_lock(struct xv6_sleeplock *s);
void xv6_sleep_unlock(struct xv6_sleeplock *s);

/* ------------------------------------------------------------------ */
/* FS定数 (Orthox-64拡張版)                                           */
/* ------------------------------------------------------------------ */
#define XV6FS_BSIZE       1024
#define XV6FS_FSMAGIC     0x10203040U
#define XV6FS_ROOTINO     1

#define XV6FS_NDIRECT     9
#define XV6FS_NINDIRECT   (XV6FS_BSIZE / sizeof(uint32_t))                      /* 256 */
#define XV6FS_NDINDIRECT  (XV6FS_NINDIRECT * XV6FS_NINDIRECT)                   /* 65536 */
#define XV6FS_NTINDIRECT  (XV6FS_NINDIRECT * XV6FS_NINDIRECT * XV6FS_NINDIRECT) /* 16777216 */
#define XV6FS_MAXFILE     (XV6FS_NDIRECT + XV6FS_NINDIRECT + XV6FS_NDINDIRECT + XV6FS_NTINDIRECT)

#define XV6FS_NBUF        128    /* バッファキャッシュ数 (128KB)。
                                  * 注: 4096 (4MB) への拡大は 2026-07-05 に実測で逆効果と判明
                                  * (cc1 13MB の exec が毎回キャッシュを洗い流す一方、
                                  *  bget の線形走査コストが全アクセスに乗るため)。
                                  * 拡大するなら bget のハッシュ化とセットで行うこと */
#define XV6FS_NINODES     8192
#define XV6FS_LOGBLOCKS   126

/* inode type */
#define XV6FS_T_DIR    1
#define XV6FS_T_FILE   2
#define XV6FS_T_DEVICE 3
#define XV6FS_T_FIFO   4   /* named pipe (mkfifo)。major/minor は T_FILE と同じ
                            * MODE_MAGIC 方式で permission を保存する */

/*
 * Orthox-64 stores POSIX permission bits for regular files and directories
 * in the otherwise unused minor field. The major field marks that minor is
 * valid, preserving compatibility with old images that left both fields zero.
 */
#define XV6FS_MODE_MAGIC 0x4f4d

/* directory entry name length (NULL終端なし)
 * dirent = inum(2) + name(62) = 64 bytes → BSIZE(1024)/64 = 16 entries/block */
#define XV6FS_DIRSIZ   62

/* ------------------------------------------------------------------ */
/* on-disk 構造体                                                      */
/* ------------------------------------------------------------------ */

struct xv6fs_superblock {
    uint32_t magic;
    uint32_t size;        /* FSサイズ (blocks) */
    uint32_t nblocks;     /* データブロック数 */
    uint32_t ninodes;
    uint32_t nlog;
    uint32_t logstart;
    uint32_t inodestart;
    uint32_t bmapstart;
};

/* on-disk inode: 60 bytes
 *   short type(2) + short major(2) + short minor(2) + short nlink(2)
 *   + uint size(4) + uint addrs[12](48)
 *   addrs[0..8]=direct(9), [9]=indirect, [10]=dindirect, [11]=tindirect
 */
struct xv6fs_dinode {
    int16_t  type;
    int16_t  major;
    int16_t  minor;
    int16_t  nlink;
    uint32_t size;
    uint32_t addrs[XV6FS_NDIRECT + 3];  /* 9直接 + 1間接 + 1二重間接 + 1三重間接 */
};

/* inodes per block */
#define XV6FS_IPB  (XV6FS_BSIZE / sizeof(struct xv6fs_dinode))      /* 17 */

/* block containing inode i */
#define XV6FS_IBLOCK(i, sb)  ((i) / XV6FS_IPB + (sb).inodestart)

/* bits per bitmap block */
#define XV6FS_BPB  (XV6FS_BSIZE * 8)

/* bitmap block for data block b */
#define XV6FS_BBLOCK(b, sb)  ((b) / XV6FS_BPB + (sb).bmapstart)

/* directory entry */
struct xv6fs_dirent {
    uint16_t inum;
    char     name[XV6FS_DIRSIZ];
};

/* ------------------------------------------------------------------ */
/* in-memory バッファ (sleeplock → spinlock)                          */
/* ------------------------------------------------------------------ */

struct xv6buf {
    int       valid;    /* ディスクから読み込み済み */
    int       disk;     /* ディスク転送中 */
    uint32_t  dev;
    uint32_t  blockno;
    struct xv6_sleeplock lock;
    uint32_t  refcnt;
    struct xv6buf *prev;
    struct xv6buf *next;
    uint8_t   data[XV6FS_BSIZE];
};

/* ------------------------------------------------------------------ */
/* in-memory inode                                                     */
/* ------------------------------------------------------------------ */

#define XV6FS_NINODE  512  /* inodeキャッシュ数 */

struct xv6fs_inode {
    uint32_t   dev;
    uint32_t   inum;
    int        ref;
    struct xv6_sleeplock lock;
    int        valid;   /* on-diskから読み込み済み */

    /* on-disk inode のコピー */
    int16_t  type;
    int16_t  major;
    int16_t  minor;
    int16_t  nlink;
    uint32_t size;
    uint32_t addrs[XV6FS_NDIRECT + 3];
};

/* ------------------------------------------------------------------ */
/* kstat (sys_fstat用) */
/* ------------------------------------------------------------------ */

struct xv6fs_stat {
    uint32_t dev;
    uint32_t ino;
    int16_t  type;
    int16_t  nlink;
    uint32_t size;
};

/* ------------------------------------------------------------------ */
/* xv6bio API                                                          */
/* ------------------------------------------------------------------ */

void           xv6bio_init(void);
struct xv6buf* xv6bread(uint32_t dev, uint32_t blockno);
void           xv6bwrite(struct xv6buf *b);
void           xv6brelse(struct xv6buf *b);
void           xv6bpin(struct xv6buf *b);
void           xv6bunpin(struct xv6buf *b);

/* ------------------------------------------------------------------ */
/* xv6log API                                                          */
/* ------------------------------------------------------------------ */

void xv6log_init(uint32_t dev, struct xv6fs_superblock *sb);
void xv6log_begin_op(void);
void xv6log_end_op(void);
void xv6log_write(struct xv6buf *b);
void xv6log_recover(void);

/* ------------------------------------------------------------------ */
/* xv6fs (fs.c相当) API                                               */
/* ------------------------------------------------------------------ */

int                  xv6fs_mount_storage(const char *devname);
void                 xv6fs_init(uint32_t dev);

struct xv6fs_inode*  xv6fs_ialloc(uint32_t dev, int16_t type);
struct xv6fs_inode*  xv6fs_iget(uint32_t dev, uint32_t inum);
void                 xv6fs_iput(struct xv6fs_inode *ip);
void                 xv6fs_ilock(struct xv6fs_inode *ip);
void                 xv6fs_iunlock(struct xv6fs_inode *ip);
void                 xv6fs_iunlockput(struct xv6fs_inode *ip);
void                 xv6fs_iupdate(struct xv6fs_inode *ip);

int                  xv6fs_readi(struct xv6fs_inode *ip, void *dst,
                                  uint32_t off, uint32_t n);
int                  xv6fs_writei(struct xv6fs_inode *ip, const void *src,
                                   uint32_t off, uint32_t n);

struct xv6fs_inode*  xv6fs_dirlookup(struct xv6fs_inode *dp,
                                      const char *name, uint32_t *poff);
int                  xv6fs_dirlink(struct xv6fs_inode *dp,
                                   const char *name, uint32_t inum);

struct xv6fs_inode*  xv6fs_namei(const char *path);
struct xv6fs_inode*  xv6fs_nameiparent(const char *path, char *name);

int                  xv6fs_stat(struct xv6fs_inode *ip, struct xv6fs_stat *st);

/* マウント済みデバイス名 (xv6bio.c の disk I/O で参照) */
extern char g_xv6fs_devname[16];

/* ------------------------------------------------------------------ */
/* VFS アダプタ API (kernel/fs.c から呼ばれる高レベル I/F)            */
/* ------------------------------------------------------------------ */

struct orth_dirent;   /* forward declaration (fs.h で定義) */

int xv6fs_is_mounted(void);
int xv6fs_stat_path(const char *path, uint32_t *mode, uint64_t *size,
                    int64_t *mtime, uint32_t *rdev);
int xv6fs_list_dir(const char *path, struct orth_dirent *dirents,
                   size_t max_entries, size_t *out_count);
int xv6fs_write_file(const char *path, uint64_t offset,
                     const void *buf, size_t n);
int xv6fs_create_file(const char *path, int mode, struct xv6fs_inode **out_ip);
int xv6fs_mknod_fifo(const char *path, int mode);
int xv6fs_truncate_file(const char *path, uint64_t length);
int xv6fs_unlink_path(const char *path);
int xv6fs_link_path(const char *oldpath, const char *newpath);
int xv6fs_rmdir_path(const char *path);
int xv6fs_mkdir_path(const char *path, int mode);
int xv6fs_chmod_path(const char *path, uint32_t mode);
int xv6fs_sync(void);

#endif /* XV6FS_H */
