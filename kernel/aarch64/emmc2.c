/*
 * Raspberry Pi 4 (BCM2711) の SD カード = EMMC2。
 *
 * ---- 出典 ----------------------------------------------------------------
 *
 * レジスタ配置・コマンド表・初期化の順序は jncronin/rpi-boot の emmc.c
 * (Copyright (C) 2013 by John Cronin, MIT) を土台にしている。
 * **リポジトリ全体は GPL-2.0 扱いだが、emmc.c は冒頭で MIT を宣言している。**
 * 根拠にすべきは当該ファイルの宣言のほう (日報2026-08-11 追記 5)。
 * THIRD_PARTY_NOTICES.md に帰属を書いた。
 *
 * ---- Pi 4 で何が違うか ----------------------------------------------------
 *
 * Pi の SD コントローラは世代で別物で、実物の DTB を見ると:
 *
 *   mmc@7e300000    brcm,bcm2835-sdhci   status = disabled  ← 旧 arasan
 *   mmcnr@7e300000  同上                 status = okay      ← WiFi の SDIO
 *   /emmc2bus/mmc@7e340000  brcm,bcm2711-emmc2  okay        ← **SD カード**
 *
 * **土台の emmc.c が相手にしているのは旧 arasan のほう。** Pi 4 の SD は
 * EMMC2 で、こちらは素直な SDHCI なので:
 *
 *   - **メールボックスが要らない。** 土台は電源投入とクロック取得を
 *     VideoCore のメールボックスでやるが (SDHCI_IMPLEMENTATION_BCM_2708)、
 *     EMMC2 は CAPABILITIES_0 からベースクロックを読める標準の道が使える
 *   - 番地は DTB から取る。emmc2bus の **ranges を通して初めて物理になる**
 *     (0x7e340000 -> 0xfe340000)。変換しないと繋がらない番地を叩いて沈黙する
 *
 * ---- 転送は ADMA2 + 割り込み完了 (2026-08-27, M-4c) ------------------------
 *
 * **もとは PIO だった。** DATA レジスタ経由で 4 バイトずつ、512 バイトを
 * 128 回の MMIO で運び、しかも 1 セクタごとに CMD17 を出していた。
 * xv6fs は BSIZE=1024 なので**読み 1 回につきコマンド 2 回**。
 *
 * 2026-08-26 の計器 (M-4b') で、これがそのまま実機の遅さだと出た:
 * ビルド中の全 49 区間で**常に 1 本のコアが SD の応答を 70〜95% 待ち、
 * 残り 3 本がそのコアの握る g_emmc2_lock を 15〜29% 待っていた。**
 * 4 コアぶんの CPU 時間の 42.6% が待ちに消えている。
 *
 * そこで 3 つ同時に変えた:
 *
 *   1. **ADMA2 で運ぶ。** CPU は記述子を 1 本書くだけになる
 *   2. **複数ブロックを 1 コマンドで。** CMD18/CMD25 + AUTO CMD12
 *   3. **完了を割り込みで待ち、待つあいだは寝る。** 待つコアも、
 *      待たされるコアも、その間ほかのタスクを走らせる
 *
 * ---- なぜバウンスバッファか ------------------------------------------------
 *
 * **呼び手のバッファに直接 DMA しない。** 理由が 2 つある:
 *
 *   - **番地が届かない。** 実機の DTB の /emmc2bus は
 *     `dma-ranges = <0x0 0xc0000000  0x0 0x00000000  0x40000000>`。
 *     コントローラから見た番地は**物理 + 0xC0000000** で、しかも
 *     **窓は低位 1GB しか無い**。呼び手のバッファは 4GB の Pi なら
 *     どこにでも来る
 *   - **キャッシュが食い違う。** 呼び手のバッファは HHDM の Normal-WB。
 *     直接 DMA するなら dc civac / dc ivac を対で入れることになる
 *
 * そこで **DMA プールの非キャッシュ枠 (Normal-NC、物理 4MB 付近) を
 * 中継に使う。** キャッシュ維持も別名 (alias) も要らない。写す手間は
 * 増えるが、**8 バイト単位で運べば 512 バイトあたり 64 回**で済む
 * (PIO の 128 回の MMIO より軽い)。
 *
 * 使えないときは**黙って PIO に退く** — ADMA2 非対応 / プールから
 * 取れない / 物理 1GB を超えた、のいずれか。`AARCH64_EMMC2_PIO=1` で
 * 明示的に PIO へ固定もできる (切り分け用)。
 *
 * ---- 割り込みが来なくても止まらない ---------------------------------------
 *
 * **最初の 1 回はポーリングで完了を待つ。** そこで割り込みが上がって
 * 初めて「寝てよい」に切り替える (g_irq_works)。GIC の設定を取り違えて
 * INTID 158 が届かなくても、遅くなるだけで固まらない。
 */
#include <stddef.h>
#include <stdint.h>
#include "aarch64/boot.h"
#include "aarch64/vm.h"
#include "aarch64/time.h"
#include "spinlock.h"
#include "task.h"
#include "xv6fs.h"   /* XV6FS_FSMAGIC。MBR 走査でパーティションを見分けるため */

/* ---- SDHCI のレジスタ (base からのオフセット) ---------------------------- */
#define EMMC_ARG2           0x00
#define EMMC_BLKSIZECNT     0x04
#define EMMC_ARG1           0x08
#define EMMC_CMDTM          0x0C
#define EMMC_RESP0          0x10
#define EMMC_RESP1          0x14
#define EMMC_RESP2          0x18
#define EMMC_RESP3          0x1C
#define EMMC_DATA           0x20
#define EMMC_STATUS         0x24
#define EMMC_CONTROL0       0x28
#define EMMC_CONTROL1       0x2C
#define EMMC_INTERRUPT      0x30
#define EMMC_IRPT_MASK      0x34
#define EMMC_IRPT_EN        0x38
#define EMMC_CONTROL2       0x3C
#define EMMC_CAPABILITIES_0 0x40
/* **標準 SDHCI の ADMA System Address。** BCM の資料には名前が無いが、
 * EMMC2 は素直な SDHCI 3.0 なので 0x58 に居る (BCM 独自なのは名前だけ) */
#define EMMC_ADMA_SA_LOW    0x58
#define EMMC_ADMA_SA_HIGH   0x5C
#define EMMC_SLOTISR_VER    0xFC

/* CMDTM のビット */
#define SD_CMD_INDEX(a)     ((uint32_t)(a) << 24)
#define SD_CMD_TYPE_ABORT   (3U << 22)
#define SD_CMD_ISDATA       (1U << 21)
#define SD_CMD_CRCCHK_EN    (1U << 19)
#define SD_CMD_RSPNS_NONE   0U
#define SD_CMD_RSPNS_136    (1U << 16)
#define SD_CMD_RSPNS_48     (2U << 16)
#define SD_CMD_RSPNS_48B    (3U << 16)
#define SD_CMD_RSPNS_MASK   (3U << 16)
#define SD_CMD_MULTI_BLOCK  (1U << 5)
#define SD_CMD_DAT_DIR_CH   (1U << 4)   /* card -> host = 読み */
#define SD_CMD_AUTO_CMD12   (1U << 2)   /* 複数ブロックの後に CMD12 を自動で出す */
#define SD_CMD_BLKCNT_EN    (1U << 1)
#define SD_CMD_DMA_EN       (1U << 0)   /* データを DMA で運ぶ (M-4c) */

#define SD_RESP_R1          (SD_CMD_RSPNS_48  | SD_CMD_CRCCHK_EN)
#define SD_RESP_R1B         (SD_CMD_RSPNS_48B | SD_CMD_CRCCHK_EN)
#define SD_RESP_R2          (SD_CMD_RSPNS_136 | SD_CMD_CRCCHK_EN)
#define SD_RESP_R3          SD_CMD_RSPNS_48
#define SD_RESP_R6          (SD_CMD_RSPNS_48  | SD_CMD_CRCCHK_EN)
#define SD_RESP_R7          (SD_CMD_RSPNS_48  | SD_CMD_CRCCHK_EN)
#define SD_DATA_READ        (SD_CMD_ISDATA | SD_CMD_DAT_DIR_CH)
#define SD_DATA_WRITE       (SD_CMD_ISDATA)

/* INTERRUPT のビット */
#define SD_INT_CMD_DONE     (1U << 0)
#define SD_INT_DATA_DONE    (1U << 1)
#define SD_INT_WRITE_RDY    (1U << 4)
#define SD_INT_READ_RDY     (1U << 5)
#define SD_INT_ERR          (1U << 15)
#define SD_INT_ERR_MASK     0xFFFF0000U
/* 待ち手が見るのはこの 3 つだけ。**CMD_DONE は入れない** — 応答は数マイクロ秒で
 * 返るので、そこで寝ると切り替えのほうが高くつく */
#define SD_INT_DATA_WAIT    (SD_INT_DATA_DONE | SD_INT_ERR | SD_INT_ERR_MASK)

/* STATUS のビット */
#define SD_STATUS_CMD_INHIBIT   (1U << 0)
#define SD_STATUS_DAT_INHIBIT   (1U << 1)
#define SD_STATUS_CARD_INSERTED (1U << 16)

/* CONTROL0 のビット。
 *
 * **標準 SDHCI では 0x28 の上位バイトが Power Control。**
 * bit8 = SD Bus Power / bits9-11 = SD Bus Voltage Select (0b111 = 3.3V)。
 *
 * 土台にした rpi-boot の emmc.c は BCM2708 の ARASAN 向けで、
 * **あちらは電源を VideoCore のメールボックスで入れる**ため
 * Power Control を書かない。EMMC2 は標準 SDHCI なので、
 * **メールボックスを捨てた代わりにここを書く必要がある。**
 *
 * しかも CONTROL1 の全体リセット (bit24 = Reset All) は
 * **Power Control もクリアする**ので、ファームウェアが入れた設定は残らない。 */
#define SD_CTRL0_BUS_POWER      (1U << 8)
#define SD_CTRL0_VOLT_3V3       (7U << 9)

/* Host Control 1 の DMA 選択 (bits 4:3)。00 = SDMA / 10 = ADMA2 (32bit)。
 * **SDMA は 512KB の境界ごとに割り込みが上がる**ので使わない */
#define SD_CTRL0_DMA_MASK       (3U << 3)
#define SD_CTRL0_DMA_ADMA2      (2U << 3)

/* CAPABILITIES_0 bit19 = ADMA2 対応。**立っていなければ PIO に退く** */
#define SD_CAPS0_ADMA2          (1U << 19)

/* 使うコマンドだけ。**表を丸ごと持たない** — 使わないものを持つと、
 * 合っているかどうかを確かめる手立てが無いまま増える */
#define CMD_GO_IDLE         (SD_CMD_INDEX(0))
#define CMD_ALL_SEND_CID    (SD_CMD_INDEX(2)  | SD_RESP_R2)
#define CMD_SEND_REL_ADDR   (SD_CMD_INDEX(3)  | SD_RESP_R6)
#define CMD_SEND_CSD        (SD_CMD_INDEX(9)  | SD_RESP_R2)
#define CMD_SELECT_CARD     (SD_CMD_INDEX(7)  | SD_RESP_R1B)
#define CMD_SEND_IF_COND    (SD_CMD_INDEX(8)  | SD_RESP_R7)
#define CMD_SET_BLOCKLEN    (SD_CMD_INDEX(16) | SD_RESP_R1)
#define CMD_READ_SINGLE     (SD_CMD_INDEX(17) | SD_RESP_R1 | SD_DATA_READ)
#define CMD_WRITE_SINGLE    (SD_CMD_INDEX(24) | SD_RESP_R1 | SD_DATA_WRITE)
/* 複数ブロック (M-4c)。**停止は AUTO CMD12 に任せる** — 自分で CMD12 を
 * 出す形にすると、転送の後にもう 1 往復かかる */
#define CMD_READ_MULTI      (SD_CMD_INDEX(18) | SD_RESP_R1 | SD_DATA_READ)
#define CMD_WRITE_MULTI     (SD_CMD_INDEX(25) | SD_RESP_R1 | SD_DATA_WRITE)
/* 転送の中止。**退避のときだけ使う。**カードは「データを送る」状態のまま
 * 止まっているので、ホスト側の線を戻すだけでは次のコマンドが通らない */
#define CMD_STOP_TRANS      (SD_CMD_INDEX(12) | SD_RESP_R1B | SD_CMD_TYPE_ABORT)
#define CMD_APP_CMD         (SD_CMD_INDEX(55) | SD_RESP_R1)
#define ACMD_SD_SEND_OP_COND (SD_CMD_INDEX(41) | SD_RESP_R3)

#define SD_BLOCK_SIZE       512U
#define SD_CLOCK_ID         400000U      /* カード識別中は 400kHz */
#define SD_CLOCK_NORMAL     25000000U    /* 識別が済んだら 25MHz */

/* ---- 状態 ---------------------------------------------------------------- */

/* **コントローラは 1 台しかない (2026-08-25, M-1)。**
 *
 * `issue_cmd` は EMMC_ARG1 / EMMC_CMDTM / EMMC_BLKSIZECNT を書いてから
 * EMMC_INTERRUPT を待つ、という**線 1 本ぶんの手順**で、控えの
 * `g_last_resp` / `g_last_interrupt` / `g_last_fail` もグローバルに 1 組ずつ。
 * **2 つの CPU が同時に入ると、互いのコマンドとデータ転送が混ざる。**
 *
 * QEMU 側の virtio-blk で同じ穴を実測した (kernel/aarch64/virtio_blk_mmio.c
 * の g_vblk_lock を参照): exec したばかりの busybox のテキストページが
 * 別のブロックの中身になり、EL0 が「分岐命令でデータアボート」という
 * ありえない形で落ちた。4 コアで 12 回中 4 回、1 コアでは 0 回。
 *
 * **実機は 4 コアなので、そのままでは必ず同じ穴を踏む。**
 * 2026-08-26 に実機で通ることを確かめた。
 *
 * ---- ただし「スピンして待つ」のをやめた (2026-08-27, M-4c) -----------------
 *
 * **転送のあいだ spin_lock を握り続けるのが M-4 の正体だった。**
 * 待っているコアも、待たされているコアも、その間なにもできない。
 *
 * そこで**所有権と排他を分けた**:
 *
 *   - `g_emmc2_lock` は **g_emmc2_busy を触るあいだだけ**握る (数命令)
 *   - 空くのを待つ側は `kernel_yield()` で**他のタスクに譲る**
 *   - 転送の完了は割り込みで待ち、待つ側は `task_mark_io_wait_until` で寝る
 *
 * 作りは riscv64 の先例をそのまま踏襲した
 * (kernel/riscv64/virtio_blk_mmio.c の vblk_mmio_acquire / wait_used)。
 *
 * **タスクがまだ無い文脈 (起動の初期) では譲れない**ので、そのときは
 * 従来どおり回して待つ。そこは CPU 0 しか走っていないので競合しない。 */
static spinlock_t g_emmc2_lock;
static uint32_t   g_emmc2_busy;      /* 1 = 誰かがコントローラを使っている */
static struct task* g_waiter;        /* 完了を寝て待っているタスク */

static uint64_t g_base;          /* MMIO の VA。0 なら未初期化 */
static uint64_t g_base_pa;
static uint32_t g_rca;           /* カードの相対アドレス */
static int      g_sdhc;          /* 1 なら SDHC/SDXC = アドレスがブロック単位 */
static uint64_t g_blocks;        /* 容量 (512 バイト単位) */
static uint32_t g_last_resp[4];

/* ---- DMA (M-4c) ---------------------------------------------------------- */

/* バウンスの大きさ。**32KB = 64 セクタ。**
 *
 * ADMA2 の記述子は長さが 16 ビットで、0 が 65536 を意味する。**32KB なら
 * その約束に触れずに 1 本で書ける。** xv6fs が一度に求めるのは 2 セクタ
 * なので、これで足りないことはまず無い (足りなければ分けて回す) */
#define EMMC2_BOUNCE_SECTORS  64U
#define EMMC2_BOUNCE_BYTES    (EMMC2_BOUNCE_SECTORS * SD_BLOCK_SIZE)

/* コントローラから見た番地。**直書きしない** — /emmc2bus の dma-ranges を
 * DTB から読む (dtb.c)。実機の Pi 4 は「物理 + 0xC0000000、低位 1GB まで」、
 * QEMU の raspi4b (旧 sdhci) は変換なし */
static uint64_t g_dma_offset;
static uint64_t g_dma_limit;     /* 0 = 上限が分からない */

static inline uint32_t emmc2_bus(uint64_t pa) {
    return (uint32_t)(pa + g_dma_offset);
}

/* 寝て待つときの 1 回ぶんの上限。**割り込みが来なくても必ず起きる** */
#define EMMC2_WAIT_SLICE_MS   10U

/* ADMA2 の記述子 (32bit 版)。attr / len / addr で 8 バイト */
typedef struct {
    uint16_t attr;
    uint16_t len;
    uint32_t addr;
} adma2_desc_t;

#define ADMA2_ATTR_VALID    (1U << 0)
#define ADMA2_ATTR_END      (1U << 1)
#define ADMA2_ATTR_TRAN     (2U << 4)   /* act = 10b = データを運ぶ */

static uint64_t g_desc_pa;           /* 記述子表の物理。0 なら DMA を使わない */
static uint64_t g_bounce_pa;         /* バウンスの物理 */
static uint32_t g_dma_ok;            /* 1 なら ADMA2 で運ぶ */
static uint32_t g_dma_proven;        /* 1 = DMA で一度でも運べた */
static uint32_t g_intid;             /* 完了割り込みの INTID。0 なら使わない */
static volatile uint32_t g_irq_done;    /* 完了の印 */
static volatile uint32_t g_irq_status;  /* ハンドラが拾った INTERRUPT */
static uint32_t g_irq_works;         /* 1 = 割り込みが実際に上がった */
static uint64_t g_irq_count;

void aarch64_uart_puts(const char* s);
void aarch64_uart_puthex64(uint64_t v);
void aarch64_uart_putchar(char c);
uint64_t aarch64_timer_freq(void);
void aarch64_gic_enable_irq(unsigned intid);

int aarch64_emmc2_read(uint64_t lba, void* buf, uint32_t sectors);
static void find_xv6fs(void);
static int emmc2_bring_up(void);

static inline void w32(uint64_t off, uint32_t v) {
    *(volatile uint32_t*)(uintptr_t)(g_base + off) = v;
}
static inline uint32_t r32(uint64_t off) {
    return *(volatile uint32_t*)(uintptr_t)(g_base + off);
}

/* 汎用タイマのカウンタ。**タイマ割り込みに依らない**ので、
 * スケジューラが動く前でも使える */
static inline uint64_t counter(void) {
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

/* マイクロ秒待つ。**cntfrq が 0 だと 0 除算になる**ので、そのときは
 * 空回しに退く (EL3 で CNTFRQ_EL0 を入れ損ねた構成でも死なせない) */
static void udelay(uint64_t us) {
    uint64_t freq = aarch64_timer_freq();
    uint64_t start, end;
    if (freq == 0) {
        volatile uint64_t i;
        for (i = 0; i < us * 100U; i++) { }
        return;
    }
    start = counter();
    end = start + (us * freq) / 1000000U;
    while (counter() < end) { }
}

/* 条件が立つまで待つ。**必ず上限を付ける** (日報2026-08-11 追§9)。
 * 戻り値 1 = 立った / 0 = 時間切れ */
/* **待った時間を積む (M-4b')。** ここは SD の応答を待って回る場所で、
 * 「4 コアが揃って 100% なのに縮まない」の容疑者。条件が即真なら
 * counter() の 1 回ぶんしか増えない (もともと読んでいる) */
#define WAIT_UNTIL(cond, us) ({                                   \
    uint64_t _f = aarch64_timer_freq();                           \
    uint64_t _t0 = counter();                                     \
    uint64_t _lim = _f ? (_t0 + ((uint64_t)(us) * _f) / 1000000U) : 0; \
    uint64_t _spin = 0;                                           \
    int _ok = 0;                                                  \
    for (;;) {                                                    \
        if (cond) { _ok = 1; break; }                             \
        if (_f) { if (counter() >= _lim) break; }                 \
        else if (++_spin > (uint64_t)(us) * 100U) break;          \
    }                                                             \
    aarch64_wait_add(1, _t0);                                     \
    _ok;                                                          \
})

/* ---- クロック ------------------------------------------------------------
 *
 * SDHCI 3.0 の 10 ビット分周。**2 のべき乗でないと使えない**ので、
 * 目標を下回らない最小のべき乗に切り上げる (土台の sd_get_clock_divider と
 * 同じ考え方)。戻り値は CONTROL1 に載せる形 */
static uint32_t clock_divider(uint32_t base_clock, uint32_t target) {
    uint32_t want, divisor = 0;
    int first_bit;

    if (target >= base_clock) {
        want = 1;
    } else {
        want = base_clock / target;
        if (base_clock % target) want--;
    }

    for (first_bit = 31; first_bit >= 0; first_bit--) {
        uint32_t bit = 1U << first_bit;
        if (want & bit) {
            divisor = (uint32_t)first_bit;
            want &= ~bit;
            if (want) divisor++;   /* べき乗でないので 1 段上げる */
            break;
        }
    }
    if (divisor >= 32U) divisor = 31U;
    divisor = divisor ? (1U << (divisor - 1)) : 0U;
    if (divisor >= 0x400U) divisor = 0x3ffU;

    return ((divisor & 0xffU) << 8) | (((divisor >> 8) & 0x3U) << 6);
}

static int set_clock(uint32_t base_clock, uint32_t target) {
    uint32_t c1;

    /* **止めてから変える。** 動いているクロックの分周比を書き換えない */
    c1 = r32(EMMC_CONTROL1) & ~4U;      /* SD クロック停止 */
    w32(EMMC_CONTROL1, c1);
    udelay(2000);

    c1 &= ~0xffe0U;                     /* 分周比の桁を落とす */
    c1 |= clock_divider(base_clock, target);
    c1 |= 1U;                           /* 内部クロック有効 */
    c1 |= (7U << 16);                   /* データタイムアウト = TMCLK * 2^23 */
    w32(EMMC_CONTROL1, c1);

    if (!WAIT_UNTIL(r32(EMMC_CONTROL1) & 2U, 1000000)) return -1;  /* 安定待ち */

    udelay(2000);
    w32(EMMC_CONTROL1, r32(EMMC_CONTROL1) | 4U);   /* SD クロック供給開始 */
    udelay(2000);
    return 0;
}

/* ---- コマンド ------------------------------------------------------------
 *
 * 戻り値 0 = 成功。**時間切れとエラーを区別する**必要がある場合のために
 * 最後の INTERRUPT を残す (CMD8 は古いカードで時間切れになるのが正常) */
static uint32_t g_last_interrupt;

/* **どこで落ちたかを残す。** 「応答しない」の一言では、コマンドを出す前に
 * 詰まったのか、出したが返らないのか、エラーが返ったのかを区別できない。
 * 実機は往復が高くつく (SD の抜き差しが要る) ので、1 回で絞れるようにする */
#define EMMC_FAIL_NONE          0
#define EMMC_FAIL_CMD_INHIBIT   1   /* 前のコマンドが終わらない = 出す前 */
#define EMMC_FAIL_DAT_INHIBIT   2   /* 前の転送が終わらない = 出す前 */
#define EMMC_FAIL_CMD_TIMEOUT   3   /* 出したが CMD_DONE も ERR も来ない */
#define EMMC_FAIL_CMD_ERROR     4   /* エラービットが立って返った */
#define EMMC_FAIL_XFER_TIMEOUT  5   /* データの ready が来ない */
#define EMMC_FAIL_XFER_ERROR    6
#define EMMC_FAIL_DATA_TIMEOUT  7   /* 転送完了が来ない */
#define EMMC_FAIL_DATA_ERROR    8
static uint32_t g_last_fail;

static int issue_cmd(uint32_t cmd, uint32_t arg, uint32_t blocks,
                     void* buf, uint32_t timeout_us) {
    uint32_t irpts;

    g_last_fail = EMMC_FAIL_NONE;

    /* 前のコマンドが終わっているか。**DAT の待ちは abort コマンドでは見ない** */
    if (!WAIT_UNTIL((r32(EMMC_STATUS) & SD_STATUS_CMD_INHIBIT) == 0, timeout_us)) {
        g_last_fail = EMMC_FAIL_CMD_INHIBIT;
        return -1;
    }
    if ((cmd & SD_CMD_TYPE_ABORT) == 0 &&
        ((cmd & SD_CMD_RSPNS_MASK) == SD_CMD_RSPNS_48B || (cmd & SD_CMD_ISDATA))) {
        if (!WAIT_UNTIL((r32(EMMC_STATUS) & SD_STATUS_DAT_INHIBIT) == 0, timeout_us)) {
            g_last_fail = EMMC_FAIL_DAT_INHIBIT;
            return -1;
        }
    }

    if (cmd & SD_CMD_ISDATA) {
        /* BLKSIZECNT は上位 16 ビットが本数、下位が 1 本の大きさ */
        w32(EMMC_BLKSIZECNT, (blocks << 16) | SD_BLOCK_SIZE);
    }

    w32(EMMC_ARG1, arg);
    w32(EMMC_CMDTM, cmd);

    if (!WAIT_UNTIL(r32(EMMC_INTERRUPT) & (SD_INT_CMD_DONE | SD_INT_ERR), timeout_us)) {
        /* **時間切れでも INTERRUPT を控える。** 何も立っていないこと自体が
         * 「コマンドが線に出ていない」という手がかりになる */
        g_last_interrupt = r32(EMMC_INTERRUPT);
        g_last_fail = EMMC_FAIL_CMD_TIMEOUT;
        return -1;
    }
    irpts = r32(EMMC_INTERRUPT);
    g_last_interrupt = irpts;
    w32(EMMC_INTERRUPT, SD_INT_ERR_MASK | SD_INT_CMD_DONE);
    if ((irpts & (SD_INT_ERR_MASK | SD_INT_CMD_DONE)) != SD_INT_CMD_DONE) {
        g_last_fail = EMMC_FAIL_CMD_ERROR;
        return -1;
    }

    switch (cmd & SD_CMD_RSPNS_MASK) {
    case SD_CMD_RSPNS_48:
    case SD_CMD_RSPNS_48B:
        g_last_resp[0] = r32(EMMC_RESP0);
        break;
    case SD_CMD_RSPNS_136:
        g_last_resp[0] = r32(EMMC_RESP0);
        g_last_resp[1] = r32(EMMC_RESP1);
        g_last_resp[2] = r32(EMMC_RESP2);
        g_last_resp[3] = r32(EMMC_RESP3);
        break;
    default:
        break;
    }

    /* データがあるなら PIO で運ぶ。**1 ブロックごとに ready を待つ** */
    if ((cmd & SD_CMD_ISDATA) && buf) {
        int is_read = (cmd & SD_CMD_DAT_DIR_CH) != 0;
        uint32_t ready = is_read ? SD_INT_READ_RDY : SD_INT_WRITE_RDY;
        uint32_t* p = (uint32_t*)buf;
        uint32_t b;

        for (b = 0; b < blocks; b++) {
            uint32_t i;
            if (!WAIT_UNTIL(r32(EMMC_INTERRUPT) & (ready | SD_INT_ERR), timeout_us)) {
                g_last_interrupt = r32(EMMC_INTERRUPT);
                g_last_fail = EMMC_FAIL_XFER_TIMEOUT;
                return -1;
            }
            irpts = r32(EMMC_INTERRUPT);
            g_last_interrupt = irpts;
            w32(EMMC_INTERRUPT, SD_INT_ERR_MASK | ready);
            if ((irpts & (SD_INT_ERR_MASK | ready)) != ready) {
                g_last_fail = EMMC_FAIL_XFER_ERROR;
                return -1;
            }

            /* **DATA レジスタは 4 バイト単位。** buf の整列は呼び手の責任だが、
             * -mstrict-align で組んでいるので uint32_t* 経由で読み書きする */
            for (i = 0; i < SD_BLOCK_SIZE / 4U; i++) {
                if (is_read) *p++ = r32(EMMC_DATA);
                else         w32(EMMC_DATA, *p++);
            }
        }
    }

    /* 転送完了 (R1b とデータ付きコマンド) */
    if ((cmd & SD_CMD_ISDATA) || (cmd & SD_CMD_RSPNS_MASK) == SD_CMD_RSPNS_48B) {
        if ((r32(EMMC_STATUS) & SD_STATUS_DAT_INHIBIT) == 0) {
            w32(EMMC_INTERRUPT, SD_INT_ERR_MASK | SD_INT_DATA_DONE);
        } else {
            if (!WAIT_UNTIL(r32(EMMC_INTERRUPT) & (SD_INT_DATA_DONE | SD_INT_ERR),
                            timeout_us)) {
                g_last_interrupt = r32(EMMC_INTERRUPT);
                g_last_fail = EMMC_FAIL_DATA_TIMEOUT;
                return -1;
            }
            irpts = r32(EMMC_INTERRUPT);
            g_last_interrupt = irpts;
            w32(EMMC_INTERRUPT, SD_INT_ERR_MASK | SD_INT_DATA_DONE);
            if ((irpts & (SD_INT_ERR_MASK | SD_INT_DATA_DONE)) != SD_INT_DATA_DONE) {
                g_last_fail = EMMC_FAIL_DATA_ERROR;
                return -1;
            }
        }
    }
    return 0;
}

/* ---- 所有権 (M-4c) --------------------------------------------------------
 *
 * **空くまでスピンしない。**譲る。`g_emmc2_lock` を握るのは
 * `g_emmc2_busy` を読み書きする数命令のあいだだけ */
static void emmc2_acquire(void) {
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_emmc2_lock);
        if (!g_emmc2_busy) {
            g_emmc2_busy = 1;
            spin_unlock_irqrestore(&g_emmc2_lock, flags);
            return;
        }
        spin_unlock_irqrestore(&g_emmc2_lock, flags);
        /* **タスクがまだ無い文脈では譲れない。** ここは CPU 0 しか
         * 走っていないので、そもそも塞がっていることが無い */
        if (!get_current_task()) return;
        kernel_yield();
    }
}

static void emmc2_release(void) {
    uint64_t flags = spin_lock_irqsave(&g_emmc2_lock);
    g_emmc2_busy = 0;
    spin_unlock_irqrestore(&g_emmc2_lock, flags);
}

/* ---- 完了割り込み (M-4c) --------------------------------------------------
 *
 * **INTID 158 は SPI で、GIC は SPI を全部 CPU 0 に向けている** (gic.c:150)。
 * 待っているタスクがどのコアに居ても、起こすのは task_wake なので届く。
 *
 * ハンドラは**状態を落として起こすだけ**。レベル駆動なので、
 * INTERRUPT を落とさないと上がりっぱなしになる。 */
void aarch64_emmc2_irq(void) {
    struct task* waiter;
    uint64_t flags;
    uint32_t bits;

    if (!g_base) return;

    bits = r32(EMMC_INTERRUPT) & SD_INT_DATA_WAIT;
    if (bits) w32(EMMC_INTERRUPT, bits);

    /* **信号を閉じてから抜ける。** 次の転送で開け直す。こうしておけば、
     * 転送していないあいだに線が上がることが無い */
    w32(EMMC_IRPT_EN, 0);

    g_irq_status |= bits;
    g_irq_count++;
    g_irq_works = 1;          /* **実際に上がった。**以後は寝て待ってよい */
    __sync_synchronize();
    g_irq_done = 1;

    flags = spin_lock_irqsave(&g_emmc2_lock);
    waiter = g_waiter;
    g_waiter = 0;
    spin_unlock_irqrestore(&g_emmc2_lock, flags);
    /* task_wake は g_task_lock を取るので、デバイスのロックの外で呼ぶ */
    if (waiter) task_wake(waiter);
}

uint32_t aarch64_emmc2_intid(void) { return g_intid; }
uint64_t aarch64_emmc2_irq_count(void) { return g_irq_count; }
int aarch64_emmc2_dma_enabled(void) { return (int)g_dma_ok; }

/* データ転送の完了を待つ。戻り値 1 = 完了 / 0 = 時間切れ。
 *
 * **最初の 1 回はポーリングで待つ** (g_irq_works が 0 のあいだ)。そこで
 * 割り込みが上がって初めて寝る側に移る。GIC の設定を取り違えていても
 * 「遅い」で済み、固まらない。 */
static int emmc2_wait_data_done(uint32_t timeout_us) {
    struct task* self = get_current_task();
    uint64_t t0 = aarch64_wait_now();
    uint64_t deadline_ms = arch_time_now_ms() + (uint64_t)(timeout_us / 1000U) + 1ULL;
    int ok = 0;

    for (;;) {
        uint32_t st;

        if (g_irq_done) { ok = 1; break; }

        /* **毎回レジスタも見る。** IRPT_EN を立てるより先に終わっていた
         * 場合、割り込みは二度と来ない */
        st = r32(EMMC_INTERRUPT) & SD_INT_DATA_WAIT;
        if (st) {
            w32(EMMC_INTERRUPT, st);
            g_irq_status |= st;
            g_irq_done = 1;
            ok = 1;
            break;
        }

        if (arch_time_now_ms() > deadline_ms) break;

        /* 寝られない (タスクが無い / まだ割り込みを見ていない) なら回す */
        if (!self || !g_irq_works) continue;

        {
            uint64_t flags = spin_lock_irqsave(&g_emmc2_lock);
            if (g_irq_done) {
                spin_unlock_irqrestore(&g_emmc2_lock, flags);
                ok = 1;
                break;
            }
            /* **印を立ててから寝る。**この 2 つのあいだに割り込みが
             * 入っても、ハンドラは g_emmc2_lock を待つので取りこぼさない */
            g_waiter = self;
            task_mark_io_wait_until(self, arch_time_now_ms() + EMMC2_WAIT_SLICE_MS);
            spin_unlock_irqrestore(&g_emmc2_lock, flags);
        }
        kernel_yield();
    }

    {
        uint64_t flags = spin_lock_irqsave(&g_emmc2_lock);
        g_waiter = 0;
        spin_unlock_irqrestore(&g_emmc2_lock, flags);
    }
    aarch64_wait_add(1, t0);   /* SD の待ちとして数える (M-4b' の計器) */
    return ok;
}

/* ---- バウンスとの往復 -----------------------------------------------------
 *
 * **必ず幅の広いアクセスで運ぶ。** バウンスは Normal-NC で、1 アクセスが
 * そのままバスの 1 往復になる。バイト単位で 512 バイト写すと PIO と
 * 変わらなくなる (8 バイト単位なら 64 回で済む)。
 *
 * -mstrict-align で組んでいるので、**整列していない相手にはひとつ落とす。** */
static void bounce_copy(void* dst, const void* src, uint32_t bytes) {
    uintptr_t mix = (uintptr_t)dst | (uintptr_t)src | (uintptr_t)bytes;
    uint32_t i;

    if ((mix & 7U) == 0) {
        uint64_t* d = (uint64_t*)dst;
        const uint64_t* sp = (const uint64_t*)src;
        for (i = 0; i < bytes / 8U; i++) d[i] = sp[i];
    } else if ((mix & 3U) == 0) {
        uint32_t* d = (uint32_t*)dst;
        const uint32_t* sp = (const uint32_t*)src;
        for (i = 0; i < bytes / 4U; i++) d[i] = sp[i];
    } else {
        uint8_t* d = (uint8_t*)dst;
        const uint8_t* sp = (const uint8_t*)src;
        for (i = 0; i < bytes; i++) d[i] = sp[i];
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void* bounce_va(void) {
    return (void*)(uintptr_t)aarch64_phys_to_virt(g_bounce_pa);
}

/* CMD / DAT の線だけ戻す。**全体リセットではない** — 電源もクロックも
 * カードの選択も保ったまま、詰まった転送だけ捨てる (SDHCI の作法)。
 * DMA が失敗して PIO へ退くときに使う */
static void emmc2_reset_lines(void) {
    w32(EMMC_CONTROL1, r32(EMMC_CONTROL1) | (1U << 25) | (1U << 26));
    (void)WAIT_UNTIL((r32(EMMC_CONTROL1) & ((1U << 25) | (1U << 26))) == 0, 100000);
    w32(EMMC_INTERRUPT, 0xffffffffU);
}

/* ---- ADMA2 でデータ付きコマンドを出す (M-4c) ------------------------------
 *
 * 戻り値 0 = 成功。`issue_cmd` の PIO 版と同じ約束で `g_last_fail` を残す。 */
static int issue_cmd_dma(uint32_t cmd, uint32_t arg, uint32_t blocks,
                         uint32_t timeout_us) {
    volatile adma2_desc_t* d;
    uint32_t irpts;
    uint32_t bytes = blocks * SD_BLOCK_SIZE;

    g_last_fail = EMMC_FAIL_NONE;

    if (!WAIT_UNTIL((r32(EMMC_STATUS) & SD_STATUS_CMD_INHIBIT) == 0, timeout_us)) {
        g_last_fail = EMMC_FAIL_CMD_INHIBIT;
        return -1;
    }
    if (!WAIT_UNTIL((r32(EMMC_STATUS) & SD_STATUS_DAT_INHIBIT) == 0, timeout_us)) {
        g_last_fail = EMMC_FAIL_DAT_INHIBIT;
        return -1;
    }

    /* 記述子は 1 本。**バウンスは連続した 32KB** なので分ける必要が無い */
    d = (volatile adma2_desc_t*)(uintptr_t)aarch64_phys_to_virt(g_desc_pa);
    d->attr = (uint16_t)(ADMA2_ATTR_TRAN | ADMA2_ATTR_END | ADMA2_ATTR_VALID);
    d->len  = (uint16_t)bytes;
    d->addr = emmc2_bus(g_bounce_pa);
    __asm__ volatile("dsb sy" ::: "memory");

    w32(EMMC_ADMA_SA_LOW, emmc2_bus(g_desc_pa));
    w32(EMMC_ADMA_SA_HIGH, 0);
    w32(EMMC_BLKSIZECNT, (blocks << 16) | SD_BLOCK_SIZE);

    g_irq_status = 0;
    g_irq_done = 0;
    __sync_synchronize();

    /* **CMDTM を書く前に信号を開ける。** 後だと、速く終わった転送の
     * 完了を取り逃す */
    w32(EMMC_IRPT_EN, SD_INT_DATA_WAIT);

    w32(EMMC_ARG1, arg);
    w32(EMMC_CMDTM, cmd);

    /* 応答は数マイクロ秒で返る。ここは回して待つ。
     * **g_irq_done も見る** — 転送が速く終わってハンドラが先に走ると、
     * エラービットはもう落とされている */
    if (!WAIT_UNTIL(g_irq_done ||
                    (r32(EMMC_INTERRUPT) & (SD_INT_CMD_DONE | SD_INT_ERR)),
                    timeout_us)) {
        g_last_interrupt = r32(EMMC_INTERRUPT);
        g_last_fail = EMMC_FAIL_CMD_TIMEOUT;
        w32(EMMC_IRPT_EN, 0);
        return -1;
    }
    irpts = r32(EMMC_INTERRUPT);
    g_last_interrupt = irpts;
    w32(EMMC_INTERRUPT, SD_INT_CMD_DONE);   /* **CMD_DONE だけ落とす** */
    if (irpts & (SD_INT_ERR | SD_INT_ERR_MASK)) {
        w32(EMMC_INTERRUPT, SD_INT_ERR_MASK | SD_INT_ERR);
        w32(EMMC_IRPT_EN, 0);
        g_last_fail = EMMC_FAIL_CMD_ERROR;
        return -1;
    }
    if (!(irpts & SD_INT_CMD_DONE)) {
        w32(EMMC_IRPT_EN, 0);
        g_last_fail = EMMC_FAIL_CMD_TIMEOUT;
        return -1;
    }
    g_last_resp[0] = r32(EMMC_RESP0);

    if (!emmc2_wait_data_done(timeout_us)) {
        g_last_interrupt = r32(EMMC_INTERRUPT);
        g_last_fail = EMMC_FAIL_DATA_TIMEOUT;
        w32(EMMC_IRPT_EN, 0);
        return -1;
    }
    w32(EMMC_IRPT_EN, 0);

    g_last_interrupt = g_irq_status;
    if (g_irq_status & (SD_INT_ERR | SD_INT_ERR_MASK)) {
        g_last_fail = EMMC_FAIL_DATA_ERROR;
        return -1;
    }

    /* **DMA が書き終わったものを読む前に境界を置く。**バウンスは
     * Normal-NC なので無効化は要らないが、順序は要る */
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

/* ACMD は CMD55 を前置きする。**RCA を渡すこと** — 0 のままだと
 * 識別後のカードが受け付けない */
static int issue_acmd(uint32_t cmd, uint32_t arg, uint32_t timeout_us) {
    if (issue_cmd(CMD_APP_CMD, g_rca << 16, 0, 0, timeout_us) != 0) return -1;
    return issue_cmd(cmd, arg, 0, 0, timeout_us);
}

/* ---- 容量 (CSD) ----------------------------------------------------------
 *
 * **R2 の応答は 8 ビットずれている。** コントローラが CRC の 1 バイトを
 * 落とすので、RESP0..RESP3 に載るのは CSD[127:8]。つまり
 * 「ずらし後のビット i」= CSD のビット (i + 8) で、
 *
 *   resp[0] = CSD[39:8]   resp[1] = CSD[71:40]
 *   resp[2] = CSD[103:72] resp[3] = CSD[135:104]  (上位 8 ビットは無効)
 *
 * ここを 8 ずらし忘れると容量が桁違いになる。**block_count が 0 のままだと
 * storage 層が全部の読みを弾く** (lba + count > block_count) ので、
 * 「初期化は通ったのにマウントできない」という形で出る (実測)。 */
static uint64_t csd_to_blocks(void) {
    uint32_t structure = (g_last_resp[3] >> 22) & 0x3U;   /* CSD[127:126] */

    if (structure == 1U) {
        /* v2 (SDHC / SDXC): C_SIZE = CSD[69:48] -> ずらし後 61:40 */
        uint32_t c_size = (g_last_resp[1] >> 8) & 0x3fffffU;
        return (uint64_t)(c_size + 1U) * 1024ULL;   /* (C_SIZE+1) * 512KB / 512B */
    }

    /* v1 (標準容量) */
    {
        uint32_t read_bl_len = (g_last_resp[2] >> 8) & 0xfU;          /* CSD[83:80] */
        uint32_t c_size_mult = (g_last_resp[1] >> 7) & 0x7U;          /* CSD[49:47] */
        /* C_SIZE = CSD[73:62] -> ずらし後 65:54 = resp2 の下位 2 ビット +
         * resp1 の上位 10 ビット */
        uint32_t c_size = ((g_last_resp[2] & 0x3U) << 10) |
                          ((g_last_resp[1] >> 22) & 0x3ffU);
        uint64_t bytes = (uint64_t)(c_size + 1U) *
                         (1ULL << (c_size_mult + 2U)) *
                         (1ULL << read_bl_len);
        return bytes / SD_BLOCK_SIZE;
    }
}

/* ---- 初期化 -------------------------------------------------------------- */

static void put(const char* s) { aarch64_uart_puts(s); }
static void puthex(uint64_t v) { aarch64_uart_puthex64(v); }

/* **桁を揃えると読めなくなる値がある。** パーティション番号やセクタ数を
 * puthex64 で出すと `part 0x0000000000000003` になり、目が滑る。
 * レジスタのダンプ (ビットの位置を数えるもの) は 16 桁のままでよいが、
 * ただの数はこちらで出す */
static void putdec(uint64_t v) {
    char buf[21];
    int i = 0;
    if (v == 0) { put("0"); return; }
    while (v) { buf[i++] = (char)('0' + (uint32_t)(v % 10U)); v /= 10U; }
    while (i--) aarch64_uart_putchar(buf[i]);
}

/* 先頭の 0 を落とした 16 進。番地やサイズは 16 進のほうが見当をつけやすい */
static void puthex_short(uint64_t v) {
    char buf[17];
    int i = 0;
    put("0x");
    if (v == 0) { put("0"); return; }
    while (v) {
        uint32_t d = (uint32_t)(v & 0xFU);
        buf[i++] = (char)(d < 10U ? '0' + d : 'a' + (d - 10U));
        v >>= 4;
    }
    while (i--) aarch64_uart_putchar(buf[i]);
}

/* 落ちたときのレジスタを全部出す。**実機は往復が高くつく** (SD の抜き差しが
 * 要る) ので、1 回で原因が絞れるだけ出す。
 *
 * 見方:
 *   fail=3 (CMD_TIMEOUT) で int=0     -> コマンドが線に出ていない。電源かクロック
 *   fail=3 で int に何か立っている    -> 出たが CMD_DONE を取り逃した
 *   fail=4 (CMD_ERROR)                -> int の上位 16 ビットが理由 (bit16=CTO
 *                                        タイムアウト / bit17=CCRC / bit19=CMD index)
 *   fail=1 (CMD_INHIBIT)              -> 前のコマンドが終わっていない。出す前に詰まった
 *   ctrl0 の bit8 が 0                -> **バス電源が入っていない** */
static void dump_regs(uint32_t base_clock) {
    put("              fail  = ");   puthex(g_last_fail);
    put("  int = ");                 puthex(g_last_interrupt);
    put("\n              status= ");  puthex(r32(EMMC_STATUS));
    put("  ctrl0 = ");               puthex(r32(EMMC_CONTROL0));
    put("\n              ctrl1 = ");  puthex(r32(EMMC_CONTROL1));
    put("  ctrl2 = ");               puthex(r32(EMMC_CONTROL2));
    put("\n              caps0 = ");  puthex(r32(EMMC_CAPABILITIES_0));
    put("  base clk = ");            puthex(base_clock);
    put("\n              ver   = ");  puthex(r32(EMMC_SLOTISR_VER));
    put("\n");
}

/* ---- DMA の準備 (M-4c) ----------------------------------------------------
 *
 * **駄目なら黙って PIO に退く。** ここで止めない — 遅くても起動するほうが
 * 良い。何で退いたかは 1 行出す。 */
static void emmc2_dma_setup(void) {
#ifdef AARCH64_EMMC2_PIO
    g_dma_ok = 0;
    put("  emmc2 dma : PIO に固定 (AARCH64_EMMC2_PIO=1)\n");
#else
    const aarch64_boot_info_t* b = aarch64_boot_info();
    uint64_t desc_pa, buf_pa;

    g_dma_ok = 0;
    g_dma_offset = b->emmc2_dma_offset;
    g_dma_limit  = b->emmc2_dma_limit;

    /* **HHDM が無いとバウンスに触れない。** DMA プールは物理で返るので、
     * 上位 VA へ移った後でないと読み書きできない */
    if (!aarch64_vm_mmu_enabled()) {
        put("  emmc2 dma : MMU の前なので PIO のまま\n");
        return;
    }
    if (!(r32(EMMC_CAPABILITIES_0) & SD_CAPS0_ADMA2)) {
#ifdef AARCH64_EMMC2_FORCE_DMA
        /* **QEMU の raspi4b は旧 arasan を模しているので caps に ADMA2 が
         * 立たない。** それでも sdhci の共通部は ADMA2 を実装しているので、
         * 記述子の形・複数ブロック・割り込みの道はここで確かめられる。
         * **実機でこれを使う理由は無い** (EMMC2 は自分で名乗る) */
        put("  emmc2 dma : caps に ADMA2 が無いが強行 (AARCH64_EMMC2_FORCE_DMA) caps0=");
        puthex(r32(EMMC_CAPABILITIES_0));
        put("\n");
#else
        put("  emmc2 dma : ADMA2 非対応 caps0=");
        puthex(r32(EMMC_CAPABILITIES_0));
        put("  PIO のまま\n");
        return;
#endif
    }

    desc_pa = aarch64_vm_dma_alloc(1);
    buf_pa  = aarch64_vm_dma_alloc(EMMC2_BOUNCE_BYTES / 4096U);
    if (!desc_pa || !buf_pa) {
        put("  emmc2 dma : プールから取れない。PIO のまま\n");
        return;
    }
    /* **窓に収まっているか** (/emmc2bus の dma-ranges)。
     * 外に出たら黙って壊れるより先に言う (sound.c と同じ考え方) */
    if (g_dma_limit && (desc_pa + 4096ULL > g_dma_limit ||
                        buf_pa + EMMC2_BOUNCE_BYTES > g_dma_limit)) {
        put("  emmc2 dma : DMA の窓の外 pa=");
        puthex_short(buf_pa);
        put(" limit=");
        puthex_short(g_dma_limit);
        put("  PIO のまま\n");
        return;
    }

    g_desc_pa   = desc_pa;
    g_bounce_pa = buf_pa;

    /* Host Control 1 の DMA 選択を ADMA2 へ。**電源のビットを消さない** */
    w32(EMMC_CONTROL0,
        (r32(EMMC_CONTROL0) & ~SD_CTRL0_DMA_MASK) | SD_CTRL0_DMA_ADMA2);

    g_dma_ok = 1;
    aarch64_console_begin();
    put("  emmc2 dma : ADMA2  desc=");
    puthex_short(emmc2_bus(g_desc_pa));
    put(" buf=");
    puthex_short(emmc2_bus(g_bounce_pa));
    put(" (バス番地)\n");
    aarch64_console_end();

    /* 完了割り込み。**ここまで来てから開ける。**
     * 最初の 1 回はポーリングで待ち、上がったのを見てから寝る側に移る */
    if (b->emmc2_intid) {
        g_intid = b->emmc2_intid;
        aarch64_gic_enable_irq(g_intid);
        put("  emmc2 irq : INTID ");
        putdec(g_intid);
        put("\n");
    } else {
        put("  emmc2 irq : DTB に番号が無い。ポーリングで待つ\n");
    }
#endif
}

/* ---- カードを立ち上げる --------------------------------------------------
 *
 * リセットからカードを選ぶところまで。**MBR も DMA も見ない。**
 *
 * 割ってあるのは、**転送が詰まったときにここだけをやり直せるようにする**
 * ため (M-4c の退避)。線のリセットと CMD12 では DAT_INHIBIT が落ちない
 * 場合があるのを QEMU で実測した (fail=2 で固まる)。全体リセットから
 * 通し直せば、起動直後と同じ状態に戻る。 */
static int emmc2_bring_up(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    uint32_t ver, c1, base_clock, i;

    g_base = 0;
    g_base_pa = 0;
    g_rca = 0;
    g_sdhc = 0;
    g_blocks = 0;

    /* **番地 0 は「この機械には無い」の印** (boot.c)。QEMU virt がこちら */
    if (b->emmc2_base == 0) return -1;
    g_base_pa = b->emmc2_base;
    g_base = aarch64_phys_to_virt(b->emmc2_base);

    /* コントローラの版。**3.0 未満は 10 ビット分周が無い**ので相手にしない */
    ver = r32(EMMC_SLOTISR_VER);
    if (((ver >> 16) & 0xffU) < 2U) {
        put("  emmc2     : SDHCI の版が古い ver=");
        puthex(ver);
        put("\n");
        return -1;
    }

    /* リセット。CONTROL1 の bit24 = 全体リセット、bit0/2 でクロックを止める */
    c1 = r32(EMMC_CONTROL1);
    c1 |= (1U << 24);
    c1 &= ~(1U << 2);
    c1 &= ~(1U << 0);
    w32(EMMC_CONTROL1, c1);
    if (!WAIT_UNTIL((r32(EMMC_CONTROL1) & (0x7U << 24)) == 0, 1000000)) {
        put("  emmc2     : リセットが終わらない\n");
        return -1;
    }

    /* カードが刺さっているか。**「無い」と「壊れた」を混ぜない** */
    if (!WAIT_UNTIL(r32(EMMC_STATUS) & SD_STATUS_CARD_INSERTED, 500000)) {
        put("  emmc2     : カードが無い\n");
        return -1;
    }

    w32(EMMC_CONTROL2, 0);

    /* **バス電源を入れる。** ここを書かないとカードに電気が行かず、
     * コマンドが線に出ないまま CMD0 が時間切れになる (2026-08-15 の実機)。
     *
     * 仕様どおり**電圧を選んでから電源を入れる** (同時に書くと、電圧が
     * 決まる前に投入する実装がある)。3.3V 以外は使わない —
     * Pi 4 の SD スロットは 3.3V 固定で、1.8V へ落とすのは UHS-I の話。 */
    w32(EMMC_CONTROL0, SD_CTRL0_VOLT_3V3);
    udelay(2000);
    w32(EMMC_CONTROL0, SD_CTRL0_VOLT_3V3 | SD_CTRL0_BUS_POWER);
    udelay(2000);

    /* **ベースクロックは CAPABILITIES_0 から読む。** メールボックスは要らない。
     * 0 が返ったら EMMC2 の実測値 (100MHz) に退く */
    base_clock = ((r32(EMMC_CAPABILITIES_0) >> 8) & 0xffU) * 1000000U;
    if (base_clock == 0) base_clock = 100000000U;

    if (set_clock(base_clock, SD_CLOCK_ID) != 0) {
        put("  emmc2     : クロックが安定しない\n");
        return -1;
    }

    /* 割り込みは ARM へ出さない。**INTERRUPT レジスタには全部立てる** —
     * ポーリングで見るのはこちら */
    w32(EMMC_IRPT_EN, 0);
    w32(EMMC_INTERRUPT, 0xffffffffU);
    w32(EMMC_IRPT_MASK, 0xffffffffU & ~(1U << 8));   /* カード割り込みだけ外す */
    udelay(2000);

    /* CMD0: アイドルへ */
    if (issue_cmd(CMD_GO_IDLE, 0, 0, 0, 500000) != 0) {
        put("  emmc2     : CMD0 に応答しない\n");
        dump_regs(base_clock);
        return -1;
    }

    /* CMD8: 電圧の確認。**2.0 より前のカードは時間切れになるのが正常**。
     * 応答すれば SDHC の可能性があるので ACMD41 に HCS を立てる */
    {
        int v2 = 0;
        if (issue_cmd(CMD_SEND_IF_COND, 0x1aa, 0, 0, 500000) == 0) {
            if ((g_last_resp[0] & 0xfffU) != 0x1aaU) {
                put("  emmc2     : CMD8 の反響が合わない\n");
                return -1;
            }
            v2 = 1;
        } else {
            /* 時間切れの後始末。**残したままだと次のコマンドが誤判定する** */
            w32(EMMC_INTERRUPT, SD_INT_ERR_MASK | SD_INT_CMD_DONE);
        }

        /* ACMD41 を、初期化が終わるまで繰り返す。
         * bit30 = HCS (大容量に対応)、bit31 = 完了の印 */
        for (i = 0; i < 1000U; i++) {
            uint32_t arg = 0x00ff8000U | (v2 ? (1U << 30) : 0U);
            if (issue_acmd(ACMD_SD_SEND_OP_COND, arg, 500000) != 0) {
                put("  emmc2     : ACMD41 が通らない (i=");
                puthex(i);
                put(")\n");
                dump_regs(base_clock);
                return -1;
            }
            if (g_last_resp[0] & (1U << 31)) {
                g_sdhc = (g_last_resp[0] & (1U << 30)) ? 1 : 0;
                break;
            }
            udelay(10000);
        }
        if (i >= 1000U) {
            put("  emmc2     : カードの初期化が終わらない\n");
            return -1;
        }
    }

    /* CMD2 -> CMD3: CID を読み、相対アドレスをもらう */
    if (issue_cmd(CMD_ALL_SEND_CID, 0, 0, 0, 500000) != 0) {
        put("  emmc2     : CMD2 が通らない\n");
        dump_regs(base_clock);
        return -1;
    }
    if (issue_cmd(CMD_SEND_REL_ADDR, 0, 0, 0, 500000) != 0) {
        put("  emmc2     : CMD3 が通らない\n");
        return -1;
    }
    g_rca = (g_last_resp[0] >> 16) & 0xffffU;

    /* CMD9: CSD から容量を出す。**カードを選ぶ前に送ること** —
     * CMD9 は stand-by 状態のカードに対するコマンドで、CMD7 で選んだ後
     * (transfer 状態) では受け付けない */
    if (issue_cmd(CMD_SEND_CSD, g_rca << 16, 0, 0, 500000) != 0) {
        put("  emmc2     : CMD9 が通らない\n");
        return -1;
    }
    g_blocks = csd_to_blocks();
    if (g_blocks == 0) {
        put("  emmc2     : 容量が 0 と出た\n");
        return -1;
    }

    /* 識別が済んだので速いクロックへ */
    if (set_clock(base_clock, SD_CLOCK_NORMAL) != 0) {
        put("  emmc2     : 通常クロックへ移れない\n");
        return -1;
    }

    /* CMD7: このカードを選ぶ */
    if (issue_cmd(CMD_SELECT_CARD, g_rca << 16, 0, 0, 500000) != 0) {
        put("  emmc2     : CMD7 が通らない\n");
        return -1;
    }

    /* **SDHC でもブロック長は 512 に揃えておく。** 標準容量のカードでは
     * これを送らないと 512 バイト単位で読めない */
    if (issue_cmd(CMD_SET_BLOCKLEN, SD_BLOCK_SIZE, 0, 0, 500000) != 0) {
        put("  emmc2     : CMD16 が通らない\n");
        return -1;
    }

    aarch64_console_begin();
    put("  emmc2     : 初期化 ok  base=");
    puthex_short(g_base_pa);
    put("  ");
    putdec(g_blocks);
    put(" ブロック (");
    putdec(g_blocks / 2097152ULL);    /* 512B ブロック -> GiB */
    put(" GiB)");
    put(g_sdhc ? "  (SDHC/SDXC)\n" : "  (標準容量)\n");
    aarch64_console_end();

    return 0;
}

int aarch64_emmc2_init(void) {
    if (emmc2_bring_up() != 0) return -1;

    /* **DMA は MBR を読む前に用意する。** 起動そのものが最初の試験になる
     * (M-4c)。駄目なら PIO のまま進む */
    emmc2_dma_setup();

    /* **カードが読めるようになってから MBR を見る。** ここまで来て初めて
     * aarch64_emmc2_read が使える */
    find_xv6fs();
    return 0;
}

/* ---- パーティション ------------------------------------------------------
 *
 * **Pi 4 の SD スロットは 1 つしかない。** QEMU のように「起動用と rootfs 用を
 * 別デバイスにする」ことができず、カードの先頭には Orthox 自身を起動するための
 * boot パーティション (FAT32) が要る。そこを xv6fs で潰すと自分が起動できない。
 *
 * そこで **MBR を読んで xv6fs の居場所を自分で見つける。**
 *
 * 判定は**パーティションタイプではなく magic** で行う。xv6fs に割り当てられた
 * タイプ番号は無いので、型で決め打ちすると「Linux (0x83) の 1 番目」のような
 * 当てにならない規則になる。**中身を見れば確実で、カードを作り直しても
 * 番号を調べ直さなくてよい。**
 *
 * storage 層のブロック = 512 バイトセクタ。xv6fs は BSIZE=1024 なので
 * **superblock (xv6 のブロック 1) は 512 バイトセクタの 2 番目**に載る。 */
#define MBR_SIG_OFF       0x1FEU
#define MBR_PART_OFF      0x1BEU
#define MBR_PART_SIZE     16U
#define MBR_PART_COUNT    4U
#define MBR_PART_TYPE     4U      /* エントリ内オフセット */
#define MBR_PART_LBA      8U
#define MBR_PART_SECTORS  12U

/* **xv6fs.h の XV6FS_FSMAGIC と一致していること。**
 * ここは MBR を走査して xv6fs のパーティションを見つけるためだけに使う。
 * 2026-08-28 に mtime 拡張でマジックを上げたとき、ここの直書きを
 * 直し忘れると「パーティションが見つからない」形で出る */
#define XV6FS_MAGIC       XV6FS_FSMAGIC
#define XV6FS_SB_SECTOR   2U      /* BSIZE=1024 のブロック 1 = 512B の 2 番目 */

static uint64_t g_part_lba;       /* xv6fs の先頭 LBA。0 = カードの先頭から */
static uint64_t g_part_blocks;    /* そこから使えるブロック数。0 = カード全体 */

/* **バッファは uint32_t で持つ。** -mstrict-align で組んでいるので、
 * 512 バイトを uint8_t の配列に置いて 32 ビットで読むと整列違反になりうる */
static uint32_t g_sector[SD_BLOCK_SIZE / 4U];

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* その LBA から始まる領域が xv6fs か。**生の LBA で読む** (この時点では
 * まだオフセットが決まっていない) */
static int looks_like_xv6fs(uint64_t lba) {
    if (aarch64_emmc2_read(lba + XV6FS_SB_SECTOR, g_sector, 1) != 0) return 0;
    return le32((const uint8_t*)g_sector) == XV6FS_MAGIC;
}

static void find_xv6fs(void) {
    const uint8_t* mbr = (const uint8_t*)g_sector;
    uint32_t i;

    g_part_lba = 0;
    g_part_blocks = 0;

    /* **カードの先頭がいきなり xv6fs の場合を先に見る。** QEMU に渡している
     * ような「まるごと 1 個の fs」のイメージを、そのまま焼いたカード */
    if (looks_like_xv6fs(0)) {
        put("  xv6fs     : カードの先頭から (パーティションなし)\n");
        return;
    }

    if (aarch64_emmc2_read(0, g_sector, 1) != 0) {
        put("  xv6fs     : MBR が読めない\n");
        return;
    }
    if (mbr[MBR_SIG_OFF] != 0x55U || mbr[MBR_SIG_OFF + 1U] != 0xAAU) {
        put("  xv6fs     : MBR の印が無い\n");
        return;
    }

    for (i = 0; i < MBR_PART_COUNT; i++) {
        const uint8_t* e = mbr + MBR_PART_OFF + i * MBR_PART_SIZE;
        uint32_t type = e[MBR_PART_TYPE];
        uint32_t lba = le32(e + MBR_PART_LBA);
        uint32_t sectors = le32(e + MBR_PART_SECTORS);
        uint32_t saved[SD_BLOCK_SIZE / 4U];
        uint32_t k;

        if (type == 0U || sectors == 0U) continue;   /* 空きエントリ */

        /* **見つからなかったときのために、あるものを全部出す。**
         * 「無い」とだけ言われても、パーティションが幾つあってどこから
         * 始まっているのかが分からないと次の手が打てない */
        /* **1 行を 8 回に分けて組み立てている (M-2)。**囲まないと、
         * 別のコアの [ep0] の行と混ざって「part 39 type=」のような
         * 実在しない行になる (2026-08-26 に実測) */
        aarch64_console_begin();
        put("  part ");
        putdec(i + 1U);
        put("    : type=");
        puthex_short(type);
        put(" lba=");
        puthex_short(lba);
        put(" sectors=");
        puthex_short(sectors);
        put("\n");
        aarch64_console_end();

        /* **MBR を退避してから中身を見に行く。** looks_like_xv6fs が
         * g_sector を踏むので、残りのエントリを読めなくなる */
        for (k = 0; k < SD_BLOCK_SIZE / 4U; k++) saved[k] = g_sector[k];

        if (looks_like_xv6fs(lba)) {
            g_part_lba = lba;
            g_part_blocks = sectors;
            aarch64_console_begin();
            put("  xv6fs     : パーティション ");
            putdec(i + 1U);
            put("  lba=");
            puthex_short(g_part_lba);
            put(" blocks=");
            putdec(g_part_blocks);
            put("\n");
            aarch64_console_end();
            return;
        }

        for (k = 0; k < SD_BLOCK_SIZE / 4U; k++) g_sector[k] = saved[k];
    }

    put("  xv6fs     : MBR に xv6fs が無い\n");
}

int aarch64_emmc2_present(void) { return g_base != 0; }
uint64_t aarch64_emmc2_base_pa(void) { return g_base_pa; }

/* **storage に見せるのはパーティションの大きさ。** カード全体を見せると、
 * 上の層が末尾を越えて読みに行ける形になる */
uint64_t aarch64_emmc2_blocks(void) {
    return g_part_blocks ? g_part_blocks : g_blocks;
}

uint64_t aarch64_emmc2_card_blocks(void) { return g_blocks; }
uint64_t aarch64_emmc2_part_lba(void) { return g_part_lba; }

/* **アドレスの単位がカードで違う。** SDHC/SDXC はブロック番号、
 * 標準容量はバイト単位。ここを取り違えると 512 倍ずれた場所を読む */
static uint32_t lba_to_arg(uint64_t lba) {
    return g_sdhc ? (uint32_t)lba : (uint32_t)(lba * SD_BLOCK_SIZE);
}

/* PIO で 1 セクタずつ運ぶ従来の道。**DMA が使えないときの退避経路** */
static int emmc2_rw_pio(uint64_t lba, uint8_t* buf, uint32_t sectors, int is_write) {
    uint32_t i;
    for (i = 0; i < sectors; i++) {
        uint32_t cmd = is_write ? CMD_WRITE_SINGLE : CMD_READ_SINGLE;
        if (issue_cmd(cmd, lba_to_arg(lba + i), 1,
                      buf + (size_t)i * SD_BLOCK_SIZE, 1000000) != 0) return -1;
    }
    return 0;
}

/* ADMA2 で運ぶ (M-4c)。**複数セクタは 1 コマンドにまとめる。**
 *
 * 従来は 1 セクタ 1 コマンドで、xv6fs の 1 ブロック (1024 バイト) を
 * 読むのに CMD17 を 2 回出していた。 */
static int emmc2_rw_dma(uint64_t lba, uint8_t* buf, uint32_t sectors, int is_write) {
    uint8_t* bounce = (uint8_t*)bounce_va();

    while (sectors) {
        uint32_t n = (sectors > EMMC2_BOUNCE_SECTORS) ? EMMC2_BOUNCE_SECTORS : sectors;
        uint32_t bytes = n * SD_BLOCK_SIZE;
        uint32_t cmd;

        if (is_write) bounce_copy(bounce, buf, bytes);

        if (n == 1) {
            cmd = (is_write ? CMD_WRITE_SINGLE : CMD_READ_SINGLE) | SD_CMD_DMA_EN;
        } else {
            cmd = (is_write ? CMD_WRITE_MULTI : CMD_READ_MULTI) |
                  SD_CMD_MULTI_BLOCK | SD_CMD_BLKCNT_EN | SD_CMD_AUTO_CMD12 |
                  SD_CMD_DMA_EN;
        }

        if (issue_cmd_dma(cmd, lba_to_arg(lba), n, 1000000) != 0) return -1;

        if (!is_write) bounce_copy(buf, bounce, bytes);

        buf += bytes;
        lba += n;
        sectors -= n;
    }
    return 0;
}

/* **転送の丸ごとを 1 人で持つ。** 1 ブロックずつ手放すと、複数セクタの
 * 要求が別の CPU の要求と 1 ブロックおきに混ざる余地が残る。
 * ただし**待つ側はスピンしない** (emmc2_acquire を参照) */
static int emmc2_rw(uint64_t lba, void* buf, uint32_t sectors, int is_write) {
    int ret;
    if (!g_base || !buf) return -1;
    if (sectors == 0) return 0;

    emmc2_acquire();
    if (g_dma_ok) {
        ret = emmc2_rw_dma(lba, (uint8_t*)buf, sectors, is_write);
        /* **最初の 1 回が通らなかったら PIO へ退く。**
         *
         * ADMA2 の設定を取り違えていたときに、起動そのものが止まるのを
         * 避ける。**一度でも通った後の失敗は本物の I/O エラー**として
         * そのまま返す (黙って遅い道に落ちると原因が見えなくなる) */
        if (ret != 0 && !g_dma_proven) {
            put("  emmc2 dma : 最初の転送が通らない fail=");
            puthex(g_last_fail);
            put(" int=");
            puthex(g_last_interrupt);
            put("  PIO へ退く\n");
            g_dma_ok = 0;
            /* **中途半端に止まった転送は線のリセットでは戻らない。**
             * CMD12 を足しても DAT_INHIBIT が落ちないのを実測した
             * (QEMU、fail=2)。**全体リセットから立ち上げ直す** */
            emmc2_reset_lines();
            (void)issue_cmd(CMD_STOP_TRANS, 0, 0, 0, 500000);
            if (emmc2_bring_up() != 0) {
                put("  emmc2 dma : 立ち上げ直しにも失敗した\n");
                emmc2_release();
                return -1;
            }
            ret = emmc2_rw_pio(lba, (uint8_t*)buf, sectors, is_write);
        } else if (ret == 0) {
            g_dma_proven = 1;
        }
    } else {
        ret = emmc2_rw_pio(lba, (uint8_t*)buf, sectors, is_write);
    }
    emmc2_release();
    return ret;
}

int aarch64_emmc2_read(uint64_t lba, void* buf, uint32_t sectors) {
    return emmc2_rw(lba, buf, sectors, 0);
}

int aarch64_emmc2_write(uint64_t lba, const void* buf, uint32_t sectors) {
    return emmc2_rw(lba, (void*)(uintptr_t)buf, sectors, 1);
}

/* ---- storage 層の受け口 --------------------------------------------------
 *
 * **戻り値は「成功なら 0」。** ブロック数ではない (virtio 側と同じ約束。
 * count を返すと xv6bio が毎回エラーとして記録する) */
/* **オフセットを足すのはここだけ。** aarch64_emmc2_read/write は生の LBA を
 * 扱う (MBR 自体を読むのに要る)。上の層はパーティションの先頭を 0 として
 * 数えるので、その差をこの 2 つで吸収する */
int aarch64_emmc2_storage_read(void* ctx, uint64_t lba, void* buf, size_t count) {
    (void)ctx;
    return aarch64_emmc2_read(g_part_lba + lba, buf, (uint32_t)count);
}

int aarch64_emmc2_storage_write(void* ctx, uint64_t lba, const void* buf, size_t count) {
    (void)ctx;
    return aarch64_emmc2_write(g_part_lba + lba, buf, (uint32_t)count);
}
