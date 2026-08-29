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
    lg.committing  = 0;
    lg.lh.n        = 0;
    g_stage_valid  = 0;

    if (!g_stage) {
        int pages = (XV6FS_LOGBLOCKS * XV6FS_BSIZE + PAGE_SIZE - 1) / PAGE_SIZE;
        void *phys = pmm_alloc(pages);
        g_stage = phys ? (uint8_t *)PHYS_TO_VIRT(phys) : 0;
        if (!g_stage)
            xv6log_print("xv6log: 中継バッファを取れない。1 ブロックずつに退く\n");
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

        if (!staged) { tail++; continue; }

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
static int xv6log_progress_hint(void *arg) {
    (void)arg;
    return !lg.committing &&
           lg.lh.n + (lg.outstanding + 1) * 10 <= XV6FS_LOGBLOCKS;
}

void xv6log_begin_op(void) {
    for (;;) {
        spin_lock(&lg.lock);
        if (!lg.committing &&
            lg.lh.n + (lg.outstanding + 1) * 10 <= XV6FS_LOGBLOCKS) {
            lg.outstanding++;
            spin_unlock(&lg.lock);
            return;
        }
        spin_unlock(&lg.lock);
        /* ログが満杯 or commit 中: 待機列で眠る (end_op / commit 完了で wake) */
        wait_event(&lg_wait, xv6log_progress_hint, 0);
    }
}

void xv6log_end_op(void) {
    int do_commit = 0;

    spin_lock(&lg.lock);
    lg.outstanding--;
    KASSERT(!lg.committing);
    if (lg.outstanding == 0) {
        do_commit = 1;
        lg.committing = 1;
    }
    spin_unlock(&lg.lock);

    if (do_commit) {
        commit();
        spin_lock(&lg.lock);
        lg.committing = 0;
        spin_unlock(&lg.lock);
    }
    /* outstanding 減少 or commit 完了でログ空きが変化した */
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

static void commit(void) {
    if (lg.lh.n > 0) {
        write_log();
        write_head();
        install_trans(0);
        lg.lh.n = 0;
        write_head();
    }
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
