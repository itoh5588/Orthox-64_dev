/*
 * Portions of this file are derived from xv6-riscv (MIT License):
 *   Copyright (c) 2006-2019 Frans Kaashoek, Robert Morris, Russ Cox,
 *                           Massachusetts Institute of Technology
 * The full xv6 MIT license text is reproduced in THIRD_PARTY_NOTICES.md.
 */
/*
 * xv6log.c — ジャーナリング層
 * xv6-riscv/kernel/log.c を Orthox-64 向けに移植。
 *
 * 変更点:
 *   struct proc / myproc() / sleep() / wakeup() を除去
 *   sleep 待機 → spinlock + スピン待機で代替
 *   initlock / acquire / release → spinlock_init / spin_lock / spin_unlock
 *   bread/bwrite/brelse/bpin/bunpin → xv6 プレフィックス版
 *   BSIZE / LOGBLOCKS → XV6FS_BSIZE / XV6FS_LOGBLOCKS
 */

#include "xv6fs.h"
#include "kassert.h"
#include "pmm.h"
#include "vmm.h"
#include <stdarg.h>

extern int vsnprintf(char *dst, size_t size, const char *fmt, va_list ap);
extern int64_t sys_write_serial(const char *buf, size_t count);
extern void *memcpy(void *dst, const void *src, size_t n);

static void xv6log_print(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) sys_write_serial(buf, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* on-disk ログヘッダ                                                  */
/* ------------------------------------------------------------------ */

struct logheader {
    int n;
    int block[XV6FS_LOGBLOCKS];
};

/* ------------------------------------------------------------------ */
/* in-memory ログ状態                                                  */
/* ------------------------------------------------------------------ */

struct xv6log {
    spinlock_t lock;
    int        start;        /* logstart ブロック番号 */
    int        outstanding;  /* 進行中の FS システムコール数 */
    int        reserved;     /* 進行中の op が申告したブロック数の合計 (P-6) */
    int        committing;   /* commit() 実行中フラグ */
    uint32_t   dev;
    struct logheader lh;
};

static struct xv6log lg;

/* ---- ログの中継バッファ (P-5、2026-08-29) --------------------------------
 *
 * **ログ領域は定義上つねに連続している** (lg.start+1 .. lg.start+n)。
 * ここを 1 ブロックずつ書いていたので、112 ブロックの commit で 112 回の
 * SD コマンドを発行していた。連続した平らなバッファに集めてから
 * **1 コマンド**で書く。
 *
 * 副産物として install_trans のログ読み戻しが要らなくなる —— **さっき
 * 書いた中身がこのバッファにそのまま在る。**n 回の読みが 0 回になる。
 *
 * mount のときに 1 回だけ確保する。XV6FS_LOGBLOCKS 126 で 126 KB。
 * 取れなければ従来どおり 1 ブロックずつに退く (g_stage == 0)。 */
static uint8_t *g_stage;
static int      g_stage_valid;   /* 中継バッファの中身が今のログと一致する */

/* begin_op がログ空きを待つための待機列。
 * 状態 (lg.*) は lg.lock が保護し、状態変更後に wake する。 */
static struct wait_queue lg_wait;

static void commit(void);
static void recover_from_log(void);

/* ---- コミットの計器 (2026-08-30) ----------------------------------------
 *
 * SD の書き込みが 1 回 15ms かかっていた (素の性能は 1〜2ms)。分布は
 * **2 セクタ = 1 ブロックが大半**で、束ね書き (P-5) が効いていない疑いが
 * あった。推し量らずに済ませるため、コミットの回数・1 回あたりのブロック
 * 数・書き戻しの区間長を数えて出す。
 *
 * **install_trans から触るので、それより前に置くこと。** */
static uint64_t g_commits;      /* commit() を呼んだ回数 */
static uint64_t g_log_blocks;   /* ログに載せたブロックの総数 */
static uint64_t g_install_runs; /* install_trans が発行した区間の数 */

void xv6log_commit_report(void);   /* timer から呼ぶ */

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void xv6log_init(uint32_t dev, struct xv6fs_superblock *sb) {
    KASSERT(sizeof(struct logheader) < XV6FS_BSIZE);

    spinlock_init(&lg.lock);
    wait_queue_init(&lg_wait);
    lg.start       = (int)sb->logstart;
    lg.dev         = dev;
    lg.outstanding = 0;
    lg.reserved = 0;
    lg.committing  = 0;
    lg.lh.n        = 0;
    g_stage_valid  = 0;

    if (!g_stage) {
        int pages = (XV6FS_LOGBLOCKS * XV6FS_BSIZE + PAGE_SIZE - 1) / PAGE_SIZE;
        void *phys = pmm_alloc(pages);
        g_stage = phys ? (uint8_t *)PHYS_TO_VIRT(phys) : 0;
        if (!g_stage)
            xv6log_print("xv6log: could not get relay buffer. falling back to 1 block at a time\n");
    }

    recover_from_log();
}

/* ------------------------------------------------------------------ */
/* ログブロックをホーム位置にコピー                                    */
/* ------------------------------------------------------------------ */

static void install_trans(int recovering) {
    /* **ログの読み戻しを 1 コマンドにする (P-5)。**
     *
     * 直前に write_log が書いた中身は中継バッファにそのまま在るので、
     * 通常の commit ではディスクを読む必要がない (n 回 → 0 回)。
     * 復旧時 (recovering) は中身が無いので、連続したログ領域を
     * **1 コマンドで**読み込んでから配る。 */
    int staged = 0;

    if (g_stage && lg.lh.n > 0) {
        if (!recovering && g_stage_valid) {
            staged = 1;
        } else if (xv6bio_rw_run(lg.dev, (uint32_t)(lg.start + 1),
                                 (uint32_t)lg.lh.n, g_stage, 0) == 0) {
            staged = 1;
        }
    }

    /* **書き戻しの宛先も、連番なら束ねる (P-5)。**
     *
     * lg.lh.block[] は writei が書いた順に並ぶので、逐次確保された
     * ファイルでは連番になりやすい。中継バッファ側も同じ順で並んで
     * いるので、**連番の区間はそのまま 1 コマンドで出せる。**
     *
     * キャッシュの整合は先に取る —— dbuf に中身を入れてから区間を
     * 書くので、キャッシュとディスクは一致する。 */
    for (int tail = 0; tail < lg.lh.n; ) {
        int run = 1;

        if (recovering)
            xv6log_print("xv6log: recover tail=%d dst=%d\n",
                         tail, lg.lh.block[tail]);

        /* まずキャッシュ側を更新する (束ねる/束ねないに関わらず要る) */
        {
            struct xv6buf *dbuf = xv6bread(lg.dev, (uint32_t)lg.lh.block[tail]);
            if (staged) {
                memcpy(dbuf->data, g_stage + (size_t)tail * XV6FS_BSIZE,
                       XV6FS_BSIZE);
            } else {
                struct xv6buf *lbuf = xv6bread(lg.dev,
                                               (uint32_t)(lg.start + tail + 1));
                memcpy(dbuf->data, lbuf->data, XV6FS_BSIZE);
                xv6brelse(lbuf);
            }
            if (!staged) xv6bwrite(dbuf);       /* 束ねられないので個別に */
            if (!recovering) xv6bunpin(dbuf);
            xv6brelse(dbuf);
        }

        if (!staged) { g_install_runs++; tail++; continue; }

        /* 宛先が連番で続くあいだ伸ばす。伸ばした分もキャッシュを更新する */
        while (tail + run < lg.lh.n &&
               lg.lh.block[tail + run] == lg.lh.block[tail] + run) {
            struct xv6buf *nb = xv6bread(lg.dev,
                                         (uint32_t)lg.lh.block[tail + run]);
            memcpy(nb->data, g_stage + (size_t)(tail + run) * XV6FS_BSIZE,
                   XV6FS_BSIZE);
            if (!recovering) xv6bunpin(nb);
            xv6brelse(nb);
            run++;
        }

        g_install_runs++;
        if (xv6bio_rw_run(lg.dev, (uint32_t)lg.lh.block[tail], (uint32_t)run,
                          g_stage + (size_t)tail * XV6FS_BSIZE, 1) != 0) {
            /* 束ねて書けなかった。**同じ区間を 1 ブロックずつ出し直す** */
            for (int k = 0; k < run; k++) {
                struct xv6buf *rb = xv6bread(lg.dev,
                                             (uint32_t)lg.lh.block[tail + k]);
                xv6bwrite(rb);
                xv6brelse(rb);
            }
        }
        tail += run;
    }
    g_stage_valid = 0;
}

/* ------------------------------------------------------------------ */
/* ログヘッダ読み書き                                                  */
/* ------------------------------------------------------------------ */

static void read_head(void) {
    struct xv6buf *buf = xv6bread(lg.dev, (uint32_t)lg.start);
    struct logheader *lh = (struct logheader *)buf->data;
    lg.lh.n = lh->n;
    for (int i = 0; i < lg.lh.n; i++)
        lg.lh.block[i] = lh->block[i];
    xv6brelse(buf);
}

static void write_head(void) {
    struct xv6buf *buf = xv6bread(lg.dev, (uint32_t)lg.start);
    struct logheader *hb = (struct logheader *)buf->data;
    hb->n = lg.lh.n;
    for (int i = 0; i < lg.lh.n; i++)
        hb->block[i] = lg.lh.block[i];
    xv6bwrite(buf);
    xv6brelse(buf);
}

/* ------------------------------------------------------------------ */
/* クラッシュリカバリ                                                  */
/* ------------------------------------------------------------------ */

void xv6log_recover(void) {
    recover_from_log();
}

static void recover_from_log(void) {
    read_head();
    install_trans(1);
    lg.lh.n = 0;
    write_head();
}

/* ------------------------------------------------------------------ */
/* トランザクション開始 / 終了                                         */
/* ------------------------------------------------------------------ */

/* begin_op が進めそうかのヒント (正確な判定は lg.lock 下で再確認する)。 */
/* ---- P-6: コミットを溜める ----------------------------------------------
 *
 * **以前は end_op が必ず commit していた。**1 回の write(2) が commit 1 回に
 * なるので、小さな書きを繰り返すものは SD を叩き潰す。実機で GCC の
 * configure を回すと、60 秒の窓で **書き 25,728 回・約 31 MB・窓の 99.7% が
 * 転送中**、しかも**ほぼ全部 2 セクタ (1 KB) 単位**だった
 * (日報2026-08-30 §10、[sd] の分布 0 20,628 5,028 72)。
 *
 * ログに余裕があるうちは溜める。**同じブロックへの繰り返しは xv6log_write の
 * 吸収で 1 エントリに畳まれる**ので、config.log に 1 行ずつ追記するようなものは
 * 最後のデータブロックと inode ブロックの 2 つに落ちる。
 *
 * ---- ★ 溜める上限だった「バッファキャッシュ」は P-3 で失効した (P-12) ----
 *
 * ログに載せたブロックは xv6log_write が xv6bpin で固定する。**48 という値は
 * 「XV6FS_NBUF = 128 のうち 80 本をデータ用に残す」という根拠で決めていたが、
 * P-3 (日報2026-08-30 §21) で XV6FS_NBUF を 4096 に上げた時点でこの根拠は
 * 失効している。**いま溜める量を実際に制限しているのは XV6FS_LOGBLOCKS
 * (ログ領域そのものの大きさ、126) だけなので、上限はそこに合わせる。
 *
 * ---- 出すのは 3 つの機会だけ ----
 *
 *   - 次の op の申告ぶんが入らないとき (begin_op)
 *   - sync(2) / fsync(2)              (xv6log_flush)
 *   - ログが満杯で begin_op が詰まったとき
 *
 * ★ 代償: write(2) から戻った時点では SD に載っていない。**電源を抜けば
 * 直近のぶんは消える。**Linux が fsync 無しで振る舞うのと同じ約束にした。
 * 焼き直しや再起動の前には busybox sync を通すこと。 */
#define XV6LOG_BATCH_MAX XV6FS_LOGBLOCKS

/* この申告ぶんが今すぐ入るか。**lg.lock は取らない** —— あくまでヒントで、
 * 本判定は begin_op がロックの下でやり直す */
static int xv6log_room_hint(void *arg) {
    int need = arg ? *(int *)arg : XV6FS_LOGBLOCKS;
    if (lg.committing) return 0;
    /* 自分で吐き出せる状態になった */
    if (lg.outstanding == 0 && lg.lh.n > 0) return 1;
    if (lg.lh.n + lg.reserved + need > XV6FS_LOGBLOCKS) return 0;
    /* 溜めるのは BATCH_MAX まで。空なら 1 op に全部使わせる */
    return lg.lh.n == 0 || lg.lh.n + lg.reserved + need <= XV6LOG_BATCH_MAX;
}

/* lg.lock を持たずに呼ぶ。溜まっているものがあり、誰も commit 中でなく、
 * 進行中の op も無いときだけ吐き出す */
static void xv6log_commit_if_idle(void) {
    int do_commit = 0;
    spin_lock(&lg.lock);
    if (!lg.committing && lg.outstanding == 0 && lg.lh.n > 0) {
        do_commit = 1;
        lg.committing = 1;
    }
    spin_unlock(&lg.lock);
    if (!do_commit) return;
    commit();
    spin_lock(&lg.lock);
    lg.committing = 0;
    spin_unlock(&lg.lock);
    wake_up_all(&lg_wait);
}

/* sync(2) / fsync(2) から呼ぶ。**ここが唯一の「必ず出す」約束**になった */
void xv6log_flush(void) {
    xv6log_commit_if_idle();
}

/* max_blocks = この op が汚しうる最大ブロック数 (XV6LOG_OP_* を使う)。
 * **見込みを間違えると xv6log_write の KASSERT で落ちる。** */
void xv6log_begin_op(int max_blocks) {
    int need = max_blocks;
    if (need < 1) need = 1;
    if (need > XV6FS_LOGBLOCKS) need = XV6FS_LOGBLOCKS;

    for (;;) {
        int do_commit = 0;
        spin_lock(&lg.lock);
        if (!lg.committing &&
            lg.lh.n + lg.reserved + need <= XV6FS_LOGBLOCKS &&
            (lg.lh.n == 0 || lg.lh.n + lg.reserved + need <= XV6LOG_BATCH_MAX)) {
            lg.outstanding++;
            lg.reserved += need;
            spin_unlock(&lg.lock);
            return;
        }
        /* **入らないなら自分で吐き出す。**end_op が commit しなくなったので、
         * ここで詰まったときに誰も出さないと止まる */
        if (!lg.committing && lg.outstanding == 0 && lg.lh.n > 0) {
            do_commit = 1;
            lg.committing = 1;
        }
        spin_unlock(&lg.lock);
        if (do_commit) {
            commit();
            spin_lock(&lg.lock);
            lg.committing = 0;
            spin_unlock(&lg.lock);
            wake_up_all(&lg_wait);
            continue;
        }
        /* ログが満杯 or commit 中: 待機列で眠る */
        wait_event(&lg_wait, xv6log_room_hint, &need);
    }
}

/* max_blocks は begin_op に渡したものと同じ値を渡すこと。
 * **ここでは commit しない** —— 出すかどうかは begin_op が決める */
void xv6log_end_op(int max_blocks) {
    int need = max_blocks;
    if (need < 1) need = 1;
    if (need > XV6FS_LOGBLOCKS) need = XV6FS_LOGBLOCKS;

    spin_lock(&lg.lock);
    lg.outstanding--;
    lg.reserved -= need;
    if (lg.reserved < 0) lg.reserved = 0;
    KASSERT(!lg.committing);
    spin_unlock(&lg.lock);
    /* outstanding / reserved が減ってログ空きが変化した */
    wake_up_all(&lg_wait);
}

/* ------------------------------------------------------------------ */
/* キャッシュ → ログへの書き込み                                       */
/* ------------------------------------------------------------------ */

static void write_log(void) {
    /* **連続したログ領域を 1 コマンドで書く (P-5)。**
     *
     * 中継バッファに集めてから 1 回で出す。従来はここで
     * xv6bread(ログブロック) を n 回呼んでおり、**126 ブロックのログが
     * 128 個しかないキャッシュを丸ごと追い出していた**という副作用も
     * あった。迂回することでキャッシュがデータ用に残る。 */
    if (g_stage && lg.lh.n > 0) {
        for (int tail = 0; tail < lg.lh.n; tail++) {
            struct xv6buf *from = xv6bread(lg.dev, (uint32_t)lg.lh.block[tail]);
            memcpy(g_stage + (size_t)tail * XV6FS_BSIZE, from->data, XV6FS_BSIZE);
            xv6brelse(from);
        }
        if (xv6bio_rw_run(lg.dev, (uint32_t)(lg.start + 1),
                          (uint32_t)lg.lh.n, g_stage, 1) == 0) {
            /* **中継バッファの中身がログと一致した。**install_trans は
             * ディスクから読み直さずにここから配れる */
            g_stage_valid = 1;
            return;
        }
        g_stage_valid = 0;
        /* 束ねて書けなかった。1 ブロックずつに退く (下へ落ちる) */
    }

    for (int tail = 0; tail < lg.lh.n; tail++) {
        struct xv6buf *to   = xv6bread(lg.dev, (uint32_t)(lg.start + tail + 1));
        struct xv6buf *from = xv6bread(lg.dev, (uint32_t)lg.lh.block[tail]);
        memcpy(to->data, from->data, XV6FS_BSIZE);
        xv6bwrite(to);
        xv6brelse(from);
        xv6brelse(to);
    }
}

/* **宛先のブロック番号で並べ替えてからコミットする。**
 *
 * lg.lh.block[] は xv6log_write が呼ばれた順、つまり「データ、間接ブロック、
 * inode、ビットマップ」が混ざった順に並ぶ。install_trans は連番の区間だけを
 * 1 コマンドにまとめるので、**混ざっていると区間が 1 で切れる。**
 *
 * 並べ替えれば write_log が g_stage に集める順も同じになり (ログ領域は
 * どのみち連続に書く)、install_trans の区間が最大まで伸びる。
 * **ログヘッダに載る順が変わるが、復旧は 1 ブロックずつ独立に書き戻す**
 * ので順序に依存しない。n は最大 126 なので挿入ソートで十分。 */
static void sort_log_blocks(void) {
    for (int i = 1; i < lg.lh.n; i++) {
        int v = lg.lh.block[i];
        int j = i - 1;
        while (j >= 0 && lg.lh.block[j] > v) {
            lg.lh.block[j + 1] = lg.lh.block[j];
            j--;
        }
        lg.lh.block[j + 1] = v;
    }
}

static void commit(void) {
    if (lg.lh.n > 0) {
        g_commits++;
        g_log_blocks += (uint64_t)lg.lh.n;
        sort_log_blocks();
        write_log();
        write_head();
        install_trans(0);
        lg.lh.n = 0;
        write_head();
    }
}

/* 60 秒ごとの計器。出したら 0 に戻す */
void xv6log_commit_report(void) {
    uint64_t c = g_commits, b = g_log_blocks, r = g_install_runs;

    g_commits = 0; g_log_blocks = 0; g_install_runs = 0;
    if (c == 0) return;

    xv6log_print("[log] 60s  commit %u  batched %u pages  %u pages/commit  "
                 "writeback %u intervals  interval length %u pages\n",
                 (unsigned)c, (unsigned)b, (unsigned)(b / c), (unsigned)r,
                 (unsigned)(r ? b / r : 0));
}

/* ------------------------------------------------------------------ */
/* ブロックをログに登録する                                            */
/* ------------------------------------------------------------------ */

void xv6log_write(struct xv6buf *b) {
    int i;

    spin_lock(&lg.lock);
    KASSERT(lg.lh.n < XV6FS_LOGBLOCKS);
    KASSERT(lg.outstanding >= 1);

    for (i = 0; i < lg.lh.n; i++) {
        if (lg.lh.block[i] == (int)b->blockno)
            break;   /* log absorption: 同一ブロックは1エントリ */
    }
    lg.lh.block[i] = (int)b->blockno;
    if (i == lg.lh.n) {
        xv6bpin(b);
        lg.lh.n++;
    }
    spin_unlock(&lg.lock);
}
