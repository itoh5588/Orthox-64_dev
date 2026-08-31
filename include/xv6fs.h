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

/* バッファキャッシュ数。1 個あたり XV6FS_BSIZE (1KB) + 管理領域。
 *
 * **2026-07-05 に 4096 (4MB) を試して逆効果だった。**cc1 13MB の exec が
 * 毎回キャッシュを洗い流す一方、**bget の線形走査コストが全アクセスに
 * 乗る**ため。「拡大するなら bget のハッシュ化とセットで」と書き残した。
 *
 * **2026-08-30 にハッシュ化した (P-3)。**bget の当たり判定が
 * O(NBUF) から O(1) になったので、拡大の前提が整った。
 *
 * 拡大の動機: P-6 で書き込みが 18 分の 1 になった結果、**律速が読みへ移った**
 * (実機の GCC configure で読み 28,399回/分・60 秒の窓の 53%)。
 *
 * 変えるときは実測で確かめること。物差しは**実機のネイティブカーネル
 * フルビルド** (46 本、P-5 後で 3 分 22 秒)。cc1 の exec を何度も踏むので
 * ここの効きがそのまま出る。
 *
 * 2026-08-30 実機 (Pi 4) で掃引した。物差しは GCC の下位 configure
 * (libiberty 相当、checking 175〜177 項目)。P-6 適用後、SD は同じ:
 *
 *     NBUF     .bss     checks   読み/分
 *      128   0.26MiB  110/106s  25,000〜30,000
 *      512   0.66MiB     70s     3,085
 *     1024   1.21MiB     68s     2,365
 *     2048   2.29MiB     66s     1,996
 *     4096   4.45MiB   ★64s      2,092
 *     8192   8.78MiB     64s     2,000
 *    16384  17.44MiB     70s     3,283
 *
 * **効くのは 128→1024 の区間だけ**で、読みが 1/12 に落ちてそこで SD が
 * 律速でなくなる。configure の作業集合が 4〜8MB に収まりきったということ。
 * 1024 以上は 68→66→64 秒と 4 秒しか縮まない。
 *
 * **16384 は逆に遅い** (70 秒)。バッファを増やしたのに読みが 3,283 回/分と
 * 増えている。手さげ袋 8192 個が CPU のキャッシュに乗らなくなったため。
 * **大きければ速いわけではない。**
 *
 * 4096 を採る。8192 と同速だが .bss が半分で済む (4.7MB 対 9.2MB)。
 *
 * なお 日報2026-07-05 の「4096 は逆効果」は**ハッシュを入れる前の話**で、
 * 当時は bget が O(NBUF) だったので大きくするほど探索が伸びた。P-3 で
 * 前提が変わったので、その結論はここで置き換わる。 */
#ifndef XV6FS_NBUF
#define XV6FS_NBUF        4096
#endif

/* bget の当たり判定に使う手さげ袋の数。**2 の冪にすること** (剰余を
 * & で済ませる)。NBUF より少し小さめにして、袋あたり 1〜2 個に収める */
#ifndef XV6FS_NBUF_HASH
#define XV6FS_NBUF_HASH   (XV6FS_NBUF / 2)
#endif
#define XV6FS_BHASH(dev, blockno) \
    ((((blockno) * 2654435761U) ^ (dev)) & (XV6FS_NBUF_HASH - 1))
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
#define XV6FS_T_SYMLINK 5  /* symlink (2026-08-31, N-6)。リンク先の文字列を
                            * 通常ファイルと同じ addrs[]/size に入れて持つ。
                            * 「速いインライン格納」は無い —— NDIRECT だけで
                            * どのみち十分な長さが入る */

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
    /* 手さげ袋の鎖 (P-3)。**LRU の鎖とは別物。**LRU は入れ替えの順序、
     * こちらは「この番号のブロックは在るか」を O(1) で引くためのもの */
    struct xv6buf *hnext;
    struct xv6buf **hprevp;
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
/* ---- xv6log_begin_op に申告する「この op が汚しうる最大ブロック数」(P-6) ----
 *
 * **溜めるようになったので、見込みを間違えるとログが溢れる。**
 * 2026-08-30 に「1 op あたり 10 ブロック」という古い見込みのまま溜めて、
 * lh.n が 116 まで育った状態に 112 ブロックの書き込みが入り、
 * xv6log_write の KASSERT(lg.lh.n < XV6FS_LOGBLOCKS) でパニックした。
 * あの 10 は**毎回コミットして lh.n が 0 に戻る前提**の数字だった。
 *
 *   XV6LOG_OP_WRITE(n)  データ n バイト + inode / ビットマップ / 間接ぶん
 *   XV6LOG_OP_SMALL     inode 更新とディレクトリ 1 件の付け外し
 *   XV6LOG_OP_FULL      **上限が読めないもの。**itrunc を踏みうる経路は
 *                       すべてこれ (unlink / rmdir / rename / truncate /
 *                       close)。xv6fs_itrunc_to は大きさに上限なく
 *                       1 トランザクションでブロックを解放する */
#define XV6LOG_OP_WRITE(bytes) \
    ((int)(((bytes) + XV6FS_BSIZE - 1) / XV6FS_BSIZE) + 8)
#define XV6LOG_OP_SMALL  16
#define XV6LOG_OP_FULL   XV6FS_LOGBLOCKS

void xv6log_begin_op(int max_blocks);
void xv6log_end_op(int max_blocks);
/* 溜めたログを必ず出す。sync(2) / fsync(2) の実体 (P-6) */
void xv6log_flush(void);
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
/* begin_op から自前で呼ぶ側 (fs_release など、iput しかしない側) 用。
 * itrunc の申告ブロック数を実測値に近づけて、必要なら安全に取り直す
 * (P-12 手2)。**iput の前に他の書き込みがある呼び出しでは使わないこと** */
void                 xv6fs_iput_op(struct xv6fs_inode *ip);
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
/* lstat/readlink 用。最後の要素がシンボリックリンクでも辿らずそれ自体を
 * 返す (途中の要素は普通に辿る) (N-6, 2026-08-31) */
struct xv6fs_inode*  xv6fs_namei_nofollow(const char *path);
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
/* symlinkat(2) (N-6, 2026-08-31)。linkpath が既にあれば -1 (EEXIST 相当) */
int xv6fs_symlink_path(const char *target, const char *linkpath);
/* readlink(2)。null 終端しない。戻り値は書いたバイト数か負 */
int xv6fs_readlink_path(const char *path, char *buf, size_t bufsiz);
/* lstat(2) 相当。xv6fs_stat_path と違い、最後の要素がシンボリックリンク
 * でも辿らずそれ自体を見る */
int xv6fs_lstat_path(const char *path, uint32_t *mode, uint64_t *size,
                     int64_t *mtime, uint32_t *rdev);
int xv6fs_truncate_file(const char *path, uint64_t length);
int xv6fs_unlink_path(const char *path);
int xv6fs_link_path(const char *oldpath, const char *newpath);
int xv6fs_rmdir_path(const char *path);
/* rename(2)。0 か負の errno を返す。呼ぶ前に絶対パスへ正規化し、
 * 自分の子孫への移動を弾いておくこと (詳細は kernel/xv6fs.c) */
int xv6fs_rename_path(const char *oldpath, const char *newpath);
int xv6fs_mkdir_path(const char *path, int mode);
int xv6fs_chmod_path(const char *path, uint32_t mode);
/* utimensat(2) の実体。mtime を「いま」に進める */
int xv6fs_touch_path(const char *path);
/* statfs(2) の材料。**単位はブロック** (bsize バイト) */
struct xv6fs_statfs {
    uint64_t bsize;
    uint64_t blocks;
    uint64_t bfree;
    uint64_t files;
    uint64_t ffree;
    uint64_t namelen;
};
int xv6fs_statfs(struct xv6fs_statfs *out);

int xv6fs_sync(void);
/* mtime に入れる「いまの秒」。壁時計ではなく、再起動を跨いで単調に増える
 * 通し番号 (スーパーブロックの mtime_base + 起動からの経過秒) */
uint32_t xv6fs_now_sec(void);

#endif /* XV6FS_H */
