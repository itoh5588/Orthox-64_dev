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
/* **2026-08-28 に 0x10203040 から上げた。**inode に mtime を足して
 * dinode が 60 → 64 バイトになり、IPB が 17 → 16 に変わったため、
 * 旧イメージを新カーネルで読むと inode の位置がずれて**黙って化ける**。
 * マジックを変えて、旧イメージは mount の時点で撥ねる */
#define XV6FS_FSMAGIC     0x10203041U
#define XV6FS_FSMAGIC_V1  0x10203040U   /* mtime 以前。読めないが名指しで報せる */
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

/* 1 トランザクションで書ける最大バイト数。**大きな書き込みは必ず分割すること。**
 *
 * 分割しないと 1 回の write でログを使い切り、xv6log_write の
 * KASSERT(lg.lh.n < XV6FS_LOGBLOCKS) でカーネルパニックになる。
 * 112KB = 112 ブロックで、inode / ビットマップ / 間接ブロックを足しても
 * XV6FS_LOGBLOCKS (126) に収まる。
 *
 * x86 (kernel/fs.c) は元からこれで分割していたが、riscv64
 * (xv6fs_write_file) には分割が無く、Orthox 上の gcc が .o を書いた瞬間に
 * パニックしていた。同じ定数を両方から使って二度と離れないようにする。 */
#define XV6FS_WRITE_CHUNK_MAX (112U * 1024U)

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
    /* **この機械には RTC が無い。**時計は起動からの経過秒しか取れないので、
     * そのまま mtime にすると再起動のたびに時刻が巻き戻り、make が
     * 「出力のほうが古い」と誤判定する。**単調増加を跨いで保つための土台**を
     * ここに置き、mount のたびに前へ進めて書き戻す (xv6fs_now_sec) */
    uint32_t mtime_base;
};

/* on-disk inode: 64 bytes  (2026-08-28 に 60 から拡張)
 *   short type(2) + short major(2) + short minor(2) + short nlink(2)
 *   + uint size(4) + uint mtime(4) + uint addrs[12](48)
 *   addrs[0..8]=direct(9), [9]=indirect, [10]=dindirect, [11]=tindirect
 *
 * **60 バイトのままでは mtime を置く隙間が無かった** (1024/60 = 17 個で
 * 端数 4 バイト、1 個あたりでは 0)。64 にすると IPB が 17 → 16 になり、
 * inode 領域が 483 → 513 ブロックに伸びる。**イメージの配置が変わる**ので
 * XV6FS_FSMAGIC も一緒に上げてある */
struct xv6fs_dinode {
    int16_t  type;
    int16_t  major;
    int16_t  minor;
    int16_t  nlink;
    uint32_t size;
    uint32_t mtime;                     /* 秒。0 = 不明 */
    uint32_t addrs[XV6FS_NDIRECT + 3];  /* 9直接 + 1間接 + 1二重間接 + 1三重間接 */
};

/* inodes per block */
#define XV6FS_IPB  (XV6FS_BSIZE / sizeof(struct xv6fs_dinode))      /* 16 */

/* **配置は mkfs (scripts/build_rootfs_xv6fs.py) と 1 バイトも違ってはいけない。**
 * 詰め物が入った瞬間に inode の位置がずれるので、ここで組み立てを止める */
typedef char xv6fs_dinode_size_check[(sizeof(struct xv6fs_dinode) == 64) ? 1 : -1];
typedef char xv6fs_ipb_check[(XV6FS_IPB == 16) ? 1 : -1];

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
    uint32_t mtime;
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

/* ---- 連続ブロックを 1 コマンドで運ぶ (P-5、2026-08-29) ------------------
 *
 * **これまで全部の転送が 1 ブロック (1 KB) ずつだった。**計器の分布が
 * 一度も 2 セクタ以外を出さなかった (日報2026-08-29 §19)。1 回あたり
 * 読み 0.94 ms / 書き 2.27 ms の固定費が、そのままブロック数だけ掛かる。
 * 同じ SD で Linux は読み 22.9 MB/s / 書き 10.4 MB/s を出している。
 *
 * **キャッシュを迂回する**ので、呼ぶ側が整合に責任を持つこと:
 *   - 読み: 区間にキャッシュ済みブロックがあってはいけない
 *           (xv6bio_range_cached で確かめる)
 *   - 書き: 書いた内容とキャッシュの中身が一致していること
 *
 * 戻り値: 0 = 成功 / 負 = 失敗 (呼び出し側は 1 ブロックずつに退くこと) */
int  xv6bio_rw_run(uint32_t dev, uint32_t blockno, uint32_t nblocks,
                   void *buf, int write);

/* 区間にキャッシュ済み (有効な) ブロックが 1 つでもあれば 1 を返す。 */
int  xv6bio_range_cached(uint32_t dev, uint32_t blockno, uint32_t nblocks);

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
/* path の inode 番号を返す。stat の st_ino に本物の値を入れるために要る。
 * (xv6fs_stat_path は呼び出し側が 8 箇所あるので引数を増やさず別関数にした) */
int xv6fs_ino_path(const char *path, uint64_t *out_ino);
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
/* mtime に入れる「いまの秒」。壁時計ではなく、再起動を跨いで単調に増える
 * 通し番号 (スーパーブロックの mtime_base + 起動からの経過秒) */
uint32_t xv6fs_now_sec(void);

#endif /* XV6FS_H */
