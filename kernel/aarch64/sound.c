/*
 * Raspberry Pi 4 (BCM2711) の音。**PWM で 3.5mm ジャックへ出す。**
 *
 * ---- 出典 ------------------------------------------------------------------
 *
 * **番地とピンは実機の DTB から取った** (tests/dtb/bcm2711-rpi-4-b-live.dtb)。
 * 他所のコードは持ち込んでいない (このリポジトリは MIT で、Linux の
 * bcm2835 系は GPL-2.0)。DTB から読めた事実は以下。
 *
 *   /soc/gpio@7e200000/audio_pins  brcm,pins = <40 41>  brcm,function = <4>
 *   /soc/pwm@7e20c800              reg = <0x7e20c800 0x28>
 *   /soc/pwm@7e20c800              clocks = <cprman 30>  50 MHz を要求
 *   /soc/cprman@7e101000           reg = <0x7e101000 0x2000>
 *
 * brcm,function の 4 は **ALT0** (0=in 1=out 2=alt5 3=alt4 4=alt0 5=alt1
 * 6=alt2 7=alt3)。**Pi 3 までとは違う** — あちらは GPIO 40/45 で PWM0
 * だったが、**Pi 4 は GPIO 40/41 で PWM1** (0x7e20c800)。
 *
 * ---- 鳴らし方 --------------------------------------------------------------
 *
 * ジャックの手前に RC のローパスが入っている。**ビープは PWM の出力
 * そのものを可聴域の矩形波にする** (mark-space モードで duty 50%)。
 * PCM はこれとは別で、搬送波を高くして duty にサンプルを載せる。
 *
 * ---- クロックの前提 --------------------------------------------------------
 *
 * **源発振を 54MHz と仮定している。**Pi 4 の水晶は 54MHz で、
 * このカーネルも汎用タイマを AARCH64_CNTFRQ_HZ=54000000 で動かしている。
 * **仮定が外れていれば音程がその比でずれる**ので、耳で分かる
 * (Pi 3 までの 19.2MHz なら 0.36 倍の低い音になる)。狙った周波数と
 * 実際の分周値をログに出すので、ずれたら突き合わせられる。
 */
#include <stdint.h>
#include "sound.h"
#include "aarch64/boot.h"
#include "aarch64/vm.h"
#include "aarch64/time.h"
#include "aarch64/bcm_periph.h"

/* 番地は include/aarch64/bcm_periph.h。**vm.c の写像と共有している** */
#define GPIO_BASE       AARCH64_BCM_GPIO_BASE
#define CM_BASE         AARCH64_BCM_CM_BASE
#define PWM1_BASE       AARCH64_BCM_PWM1_BASE

/* GPFSEL は 10 ピンで 1 語、1 ピン 3 ビット。**40,41 は GPFSEL4** */
#define GPFSEL4         0x10U
/* BCM2711 のプル制御。**BCM2835 の GPPUD/GPPUDCLK とは別物**。
 * REG0=0xE4 (0..15) / REG1=0xE8 (16..31) / **REG2=0xEC (32..47)** /
 * REG3=0xF0 (48..57)。**40,41 は REG2** — 0xE8 は GPIO 24/25 に当たる */
#define GPIO_PUP_PDN2   0xECU   /* ピン 32..47。1 ピン 2 ビット */

#define CM_PWMCTL       0xA0U
#define CM_PWMDIV       0xA4U
#define CM_PASSWD       0x5A000000U
#define CM_CTL_ENAB     (1U << 4)
#define CM_CTL_BUSY     (1U << 7)
#define CM_CTL_SRC_OSC  1U      /* 源発振 */

#define PWM_CTL         0x00U
#define PWM_STA         0x04U
#define PWM_RNG1        0x10U
#define PWM_DAT1        0x14U
#define PWM_FIF1        0x18U   /* ch1/ch2 で共有。交互に振り分けられる */
#define PWM_RNG2        0x20U
#define PWM_DAT2        0x24U

#define PWM_CTL_PWEN1   (1U << 0)
#define PWM_CTL_MSEN1   (1U << 7)
#define PWM_CTL_CLRF1   (1U << 6)
#define PWM_CTL_USEF1   (1U << 5)
#define PWM_CTL_PWEN2   (1U << 8)
#define PWM_CTL_USEF2   (1U << 13)
#define PWM_CTL_MSEN2   (1U << 15)

/* STA のビット。**bit8 は STA1 ではなく BERR** — ここを読み違えると
 * 「送出中」と「バスエラー」を取り違える (2026-08-20 に実際に取り違えた) */
#define PWM_STA_FULL1   (1U << 0)
#define PWM_STA_EMPT1   (1U << 1)
#define PWM_STA_BERR    (1U << 8)
#define PWM_STA_STA1    (1U << 9)
#define PWM_STA_STA2    (1U << 10)

/* 源発振 (上のコメントの前提) */
#define OSC_HZ          54000000U
/* PWM のクロック。**54 / 2 = 27MHz**。
 *
 * ビープだけなら 1MHz で足りるが、**PCM は 1 サンプルの長さが
 * range = クロック / 標本化周波数**で決まる。1MHz だと 22050Hz で
 * range=45 しか取れず、8 ビットの音を載せられない。27MHz なら
 * range=1224 あり、u8 の 256 段が余裕で入る。
 * Linux も この板では PWM を 50MHz で回している (DTB の
 * assigned-clock-rates = 0x2faf080) */
#define PWM_DIVI        2U
#define PWM_CLK_HZ      (OSC_HZ / PWM_DIVI)

static int g_ready;

/* **MMU の前後で番地の見え方が変わる。**触るたびに変換する
 * (mailbox.c / fb.c と同じ理屈) */
static inline volatile uint32_t* reg_ptr(uint64_t base, uint32_t off) {
    uint64_t pa = base + off;
    if (aarch64_vm_mmu_enabled()) return (volatile uint32_t*)(uintptr_t)aarch64_phys_to_virt(pa);
    return (volatile uint32_t*)(uintptr_t)pa;
}
static inline uint32_t r32(uint64_t b, uint32_t o) { return *reg_ptr(b, o); }
static inline void w32(uint64_t b, uint32_t o, uint32_t v) { *reg_ptr(b, o) = v; }

/* **us 単位の待ち。**PWM は遅いクロック領域にいるので、レジスタを
 * 続けて叩くとバスエラー (STA の BERR) になる。ms の API では粗すぎるので
 * 汎用タイマを直接読む */
static void delay_us(uint32_t us) {
    uint64_t f, t0, n;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (f == 0) return;
    n = (f * (uint64_t)us) / 1000000ULL;
    if (n == 0) n = 1;
    __asm__ volatile("isb");
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t0));
    for (;;) {
        uint64_t t;
        __asm__ volatile("isb");
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
        if ((t - t0) >= n) break;
    }
}

/* **空回しではなく時刻で待つ。**回数は最適化と CPU の速さで変わる */
static void delay_ms(uint32_t ms) {
    uint64_t t0 = arch_time_now_ms();
    while ((arch_time_now_ms() - t0) < (uint64_t)ms) __asm__ volatile("yield");
}

/* GPIO 40/41 を ALT0 (= PWM1_0 / PWM1_1) にする。プルは無し (DTB の
 * bias-disable) */
static void gpio_to_pwm(void) {
    uint32_t v = r32(GPIO_BASE, GPFSEL4);
    uint32_t before = v;

    v &= ~(0x7U << 0);          /* GPIO40 */
    v &= ~(0x7U << 3);          /* GPIO41 */
    v |=  (0x4U << 0);          /* ALT0 */
    v |=  (0x4U << 3);
    w32(GPIO_BASE, GPFSEL4, v);

    v = r32(GPIO_BASE, GPIO_PUP_PDN2);
    v &= ~(0x3U << 16);         /* GPIO40: プル無し */
    v &= ~(0x3U << 18);         /* GPIO41 */
    w32(GPIO_BASE, GPIO_PUP_PDN2, v);

    /* **書いたものが本当に入ったかを読み戻す。**ここを測っていなかったので
     * 「PWM は動いているのに音が出ない」の切り分けが GPIO 側で止まっていた。
     * fsel は 0=in 1=out 4=ALT0。**40 も 41 も 4 でなければ経路が繋がらない** */
    {
        uint32_t after = r32(GPIO_BASE, GPFSEL4);
        uint32_t pud   = r32(GPIO_BASE, GPIO_PUP_PDN2);
        aarch64_uart_puts("  gpio fsel4: ");
        aarch64_uart_puthex64(before);
        aarch64_uart_puts(" -> ");
        aarch64_uart_puthex64(after);
        aarch64_uart_puts("  gpio40=");
        aarch64_uart_putdec64(after & 0x7U);
        aarch64_uart_puts(" gpio41=");
        aarch64_uart_putdec64((after >> 3) & 0x7U);
        aarch64_uart_puts(" (4=ALT0)\n  gpio pud2 : ");
        aarch64_uart_puthex64(pud);
        aarch64_uart_puts("  p40=");
        aarch64_uart_putdec64((pud >> 16) & 0x3U);
        aarch64_uart_puts(" p41=");
        aarch64_uart_putdec64((pud >> 18) & 0x3U);
        aarch64_uart_puts(" (0=プル無し)\n");
    }
}

/* PWM のクロックを源発振から作る。**止めてから設定して、また回す** */
static void pwm_clock_setup(void) {
    uint32_t spin;

    /* 止める。**BUSY が落ちるまで待つ** — 回っている最中に
     * DIV を書くと出力が化ける */
    w32(CM_BASE, CM_PWMCTL, CM_PASSWD | CM_CTL_SRC_OSC);
    for (spin = 0; spin < 100000U; spin++) {
        if (!(r32(CM_BASE, CM_PWMCTL) & CM_CTL_BUSY)) break;
    }

    /* 整数分周のみ (MASH=0)。**小数分周はジッタを載せる** */
    w32(CM_BASE, CM_PWMDIV, CM_PASSWD | (PWM_DIVI << 12));
    w32(CM_BASE, CM_PWMCTL, CM_PASSWD | CM_CTL_SRC_OSC);
    w32(CM_BASE, CM_PWMCTL, CM_PASSWD | CM_CTL_SRC_OSC | CM_CTL_ENAB);

    for (spin = 0; spin < 100000U; spin++) {
        if (r32(CM_BASE, CM_PWMCTL) & CM_CTL_BUSY) break;
    }
}


/* ==========================================================================
 * D-3: PWM の FIFO を DMA で埋める
 *
 * **これまでは CPU が 1 サンプルずつ FIFO に書いていた。**FULL1 を見ながら
 * 回すので、**再生中この関数から戻らない。**512 サンプル @16kHz なら 32ms
 * 止まる。DOOM は 35 tic/秒 (1 tic = 28.6ms) なので、**音を出すとゲームが
 * 1 tic まるごと止まる。**鳴らせば止まり、止めなければ鳴らない。
 *
 * ---- 出典 ------------------------------------------------------------------
 *
 * **他所のコードは持ち込んでいない** (このリポジトリは MIT)。持ち込んだのは
 * **事実 1 つ**だけ:
 *
 *   BCM2711 の PWM1 の DMA DREQ (PERMAP) は **1**
 *
 * BCM2711 のデータシートに DREQ の表が無く、Raspberry Pi のフォーラムでも
 * 「RPi 4 の PWM を DMA で鳴らすときどの DREQ か分かっていない」で
 * 止まっている。bare-metal の Circle (GPL-2.0) が RASPPI>=4 で
 * DREQSourcePWM1 = 1 を使っており、**同じ表の HDMI = 10 が
 * Raspberry Pi 版 Linux の bcm2711.dtsi の hdmi0 dmas = <&dma 10> と
 * 一致する**ので、表として裏が取れた。**番号は事実であって表現ではない。**
 *
 * BCM2835 では 1 は DSI だった。**BCM2711 で割り当て直されている。**
 *
 * ---- 作り ------------------------------------------------------------------
 *
 * **円環にした制御ブロック (CB) を DMA に延々と回させる。**
 *
 *   CB[0] -> CB[1] -> ... -> CB[N-1] -+
 *     ^                               |
 *     +-------------------------------+
 *
 * 各 CB は 1 ブロック (BLOCK_SAMPLES サンプル) を FIFO へ流す。CPU は
 * **いま鳴っている場所より先**のブロックを埋める。埋まっていないブロックは
 * 無音なので、**間に合わなくても雑音ではなく無音になる。**止まりもしない。
 *
 * いま鳴っている場所は **CONBLK_AD を読めば分かる** — DMA が実行中の CB の
 * 番地が入っている。割り込みは要らない。
 *
 * **埋めた次のブロックを無音で潰す。**潰さないと、続きが来なかったときに
 * 一周してから同じ音がもう一度鳴る。
 * ========================================================================== */

#define DMA_BASE        AARCH64_BCM_DMA_BASE
#define DMA_CH          5U      /* brcm,dma-channel-mask = 0x7f5 の空き */
#define DMA_CH_OFF      (DMA_CH * 0x100U)

#define DMA_CS          0x00U
#define DMA_CONBLK_AD   0x04U
#define DMA_DEBUG       0x20U

#define DMA_CS_ACTIVE   (1U << 0)
#define DMA_CS_END      (1U << 1)
#define DMA_CS_INT      (1U << 2)
#define DMA_CS_ERROR    (1U << 8)
#define DMA_CS_ABORT    (1U << 30)
#define DMA_CS_RESET    (1U << 31)
/* 優先度は中庸に。上げすぎると他のバス利用者を飢えさせる */
#define DMA_CS_PRIO     (8U << 16)
#define DMA_CS_PANICPRIO (8U << 20)
#define DMA_CS_WAIT_WR  (1U << 28)

#define DMA_TI_WAIT_RESP (1U << 3)
#define DMA_TI_DEST_DREQ (1U << 6)
#define DMA_TI_SRC_INC   (1U << 8)
#define DMA_TI_PERMAP(x) (((uint32_t)(x) & 0x1FU) << 16)

#define DREQ_PWM1        1U     /* 上の「出典」を見ること */

#define PWM_DMAC         0x08U
#define PWM_DMAC_ENAB    (1U << 31)
/* FIFO は 16 語。**しきい値はデータシートの既定値**。DREQ は「FIFO の
 * 残りがこれを下回ったら要求する」、PANIC は「下回ったら優先度を上げる」 */
#define PWM_DMAC_PANIC(x) (((uint32_t)(x) & 0xFFU) << 8)
#define PWM_DMAC_DREQ(x)  ((uint32_t)(x) & 0xFFU)

/* **DMA が見る番地は CPU の物理番地とは別。**バスから見た非キャッシュの
 * 別名に直す。DMA プールは物理 0x00400000 付近 = 低位 1GB なので legacy
 * DMA (32bit) の射程に入っている */
#define BUS_ADDR(pa)    ((uint32_t)(((uint64_t)(pa) & ~0xC0000000ULL) | 0xC0000000ULL))
/* ペリフェラルは 0x7Exxxxxx で見える (CPU からは 0xFExxxxxx) */
#define BUS_PERI(pa)    ((uint32_t)(((uint64_t)(pa) - AARCH64_BCM_PERI_BASE) + 0x7E000000ULL))

/* **1 ブロックは DOOM の 1 回の提出とそろえる** (ORTHOS_MIX_SAMPLES=512)。
 * 16kHz で 32ms。8 ブロックで 256ms ぶん先まで貯められる */
#define SND_BLOCKS       8U
#define SND_BLOCK_SAMPLES 512U
/* **モノラルでも 1 サンプルにつき 2 語。**ch1/ch2 が FIFO を共有していて
 * 交互に振り分けられるため (PWM_FIF1 の注記) */
#define SND_BLOCK_WORDS  (SND_BLOCK_SAMPLES * 2U)

/* CB は 32 バイト。**並びは BCM2835 のデータシートのとおり** */
typedef struct {
    uint32_t ti;
    uint32_t source_ad;
    uint32_t dest_ad;
    uint32_t txfr_len;
    uint32_t stride;
    uint32_t nextconbk;
    uint32_t pad[2];
} dma_cb_t;

static uint64_t g_cb_pa;        /* CB 配列の物理番地 */
static uint64_t g_buf_pa;       /* 音のブロックの物理番地 */
static uint32_t g_dma_running;
static uint32_t g_write_idx;
/* **鳴り終わったブロックを無音に戻す位置。**円環は回り続けるので、
 * 戻さないと一周して同じ音がもう一度鳴る (実機で「鳴るけど止まらない」) */
static uint32_t g_clean_idx;
static uint32_t g_dma_range;    /* いまの PWM 分周。無音の値がこれで決まる */
static uint32_t g_dma_rate;

static inline volatile uint32_t* dma_reg(uint32_t off) {
    return reg_ptr(DMA_BASE, DMA_CH_OFF + off);
}
static inline dma_cb_t* cb_at(uint32_t i) {
    uint64_t pa = g_cb_pa + (uint64_t)i * sizeof(dma_cb_t);
    if (aarch64_vm_mmu_enabled()) return (dma_cb_t*)(uintptr_t)aarch64_phys_to_virt(pa);
    return (dma_cb_t*)(uintptr_t)pa;
}
static inline uint32_t* blk_at(uint32_t i) {
    uint64_t pa = g_buf_pa + (uint64_t)i * SND_BLOCK_WORDS * 4ULL;
    if (aarch64_vm_mmu_enabled()) return (uint32_t*)(uintptr_t)aarch64_phys_to_virt(pa);
    return (uint32_t*)(uintptr_t)pa;
}

/* 無音 = duty 50%。**0 にすると出力が振り切れたままになり、ジャックの RC を
 * 通ったあとで「ブツッ」と鳴る** */
static void blk_silence(uint32_t i) {
    uint32_t* w = blk_at(i);
    uint32_t v = g_dma_range / 2U;
    uint32_t k;
    for (k = 0; k < SND_BLOCK_WORDS; k++) w[k] = v;
}

/* いま DMA が実行している CB の番号。分からなければ 0 */
static uint32_t dma_play_idx(void) {
    uint32_t cur = *dma_reg(DMA_CONBLK_AD);
    uint32_t base = BUS_ADDR(g_cb_pa);
    if (cur < base) return 0;
    return ((cur - base) / (uint32_t)sizeof(dma_cb_t)) % SND_BLOCKS;
}

/* PWM と DMA を止める。**発生源から順に。**先に DMA を止めないと、
 * PWM を落とした瞬間に DREQ が来なくなって DMA が宙ぶらりんになる */
static void dma_stop(void) {
    *dma_reg(DMA_CS) = DMA_CS_ABORT;
    delay_us(10);
    *dma_reg(DMA_CS) = DMA_CS_RESET;
    delay_us(100);
    w32(PWM1_BASE, PWM_DMAC, 0);
    g_dma_running = 0;
}

/* 円環を組んで回し始める。**sample_rate が変わったら組み直す** */
static int dma_start(uint32_t sample_rate) {
    uint32_t range, i;

    if (!g_cb_pa || !g_buf_pa) return -1;
    if (sample_rate < 4000U)  sample_rate = 4000U;
    if (sample_rate > 44100U) sample_rate = 44100U;

    if (g_dma_running && g_dma_rate == sample_rate) return 0;
    if (g_dma_running) dma_stop();

    range = PWM_CLK_HZ / sample_rate;
    if (range < 2U) range = 2U;
    g_dma_range = range;
    g_dma_rate = sample_rate;

    /* 全部を無音で埋めてから回す。**ゴミを鳴らさない** */
    for (i = 0; i < SND_BLOCKS; i++) blk_silence(i);

    for (i = 0; i < SND_BLOCKS; i++) {
        dma_cb_t* cb = cb_at(i);
        cb->ti = DMA_TI_PERMAP(DREQ_PWM1) | DMA_TI_DEST_DREQ |
                 DMA_TI_SRC_INC | DMA_TI_WAIT_RESP;
        cb->source_ad = BUS_ADDR(g_buf_pa + (uint64_t)i * SND_BLOCK_WORDS * 4ULL);
        cb->dest_ad   = BUS_PERI(PWM1_BASE + PWM_FIF1);
        cb->txfr_len  = SND_BLOCK_WORDS * 4U;
        cb->stride    = 0;
        cb->nextconbk = BUS_ADDR(g_cb_pa + (uint64_t)((i + 1U) % SND_BLOCKS) * sizeof(dma_cb_t));
        cb->pad[0] = 0;
        cb->pad[1] = 0;
    }
    /* **CB とバッファを書き終えてから DMA に見せる。**プールは Normal-NC
     * なのでキャッシュの掃除は要らないが、並べ替えは止める必要がある */
    __asm__ volatile("dsb sy" ::: "memory");

    /* PWM を FIFO 経由に。**CLRF1 で残骸を捨ててから** */
    w32(PWM1_BASE, PWM_CTL, 0);                 delay_us(10);
    w32(PWM1_BASE, PWM_CTL, PWM_CTL_CLRF1);     delay_us(10);
    w32(PWM1_BASE, PWM_STA, PWM_STA_BERR);      delay_us(10);
    w32(PWM1_BASE, PWM_RNG1, range);            delay_us(10);
    w32(PWM1_BASE, PWM_RNG2, range);            delay_us(10);
    /* **DMAC は PWEN より先。**後にすると、有効化した瞬間の FIFO が空で
     * BERR が立つ */
    w32(PWM1_BASE, PWM_DMAC,
        PWM_DMAC_ENAB | PWM_DMAC_PANIC(7) | PWM_DMAC_DREQ(7));
    delay_us(10);
    w32(PWM1_BASE, PWM_CTL,
        PWM_CTL_PWEN1 | PWM_CTL_USEF1 |
        PWM_CTL_PWEN2 | PWM_CTL_USEF2);         delay_us(10);

    /* チャネルを起こす */
    *dma_reg(DMA_CS) = DMA_CS_RESET;
    delay_us(100);
    *dma_reg(DMA_CS) = DMA_CS_INT | DMA_CS_END;
    *dma_reg(DMA_DEBUG) = 7U;      /* 溜まっていたエラーを落とす (RW1C) */
    *dma_reg(DMA_CONBLK_AD) = BUS_ADDR(g_cb_pa);
    __asm__ volatile("dsb sy" ::: "memory");
    *dma_reg(DMA_CS) = DMA_CS_ACTIVE | DMA_CS_PRIO | DMA_CS_PANICPRIO | DMA_CS_WAIT_WR;

    g_write_idx = 0;
    g_clean_idx = 0;
    g_dma_running = 1;
    return 0;
}

/* **鳴り終わったブロックを無音に戻す。**タイマ割り込みから 10ms ごとに
 * 呼ばれる。1 ブロックは 32ms (512 サンプル @16kHz) なので取りこぼさない。
 *
 * **DMA の割り込みは使っていない。**掃除に間に合えばよく、そのために
 * GIC にもう 1 本繋ぐ理由が無い。
 *
 * 消すのは**いま鳴っている場所より後ろだけ。**積んだばかりで
 * まだ鳴っていないブロックには触らない */
void sound_tick(void) {
    uint32_t play, guard;
    if (!g_ready || !g_dma_running) return;
    play = dma_play_idx();
    for (guard = 0; guard < SND_BLOCKS && g_clean_idx != play; guard++) {
        blk_silence(g_clean_idx);
        g_clean_idx = (g_clean_idx + 1U) % SND_BLOCKS;
    }
}

/* **積むだけで戻る。**受け取ったサンプル数を返す。0 は「いまは満杯」で
 * 失敗ではない。**ここが D-3 の本体** */
int sound_pcm_submit_u8(const uint8_t* data, uint32_t len, uint32_t sample_rate) {
    uint32_t play, queued, i;
    uint32_t* w;

    if (!g_ready) return -1;
    if (!data || len == 0U) return 0;
    if (dma_start(sample_rate) < 0) return -1;
    if (len > SND_BLOCK_SAMPLES) len = SND_BLOCK_SAMPLES;

    play = dma_play_idx();
    queued = (g_write_idx + SND_BLOCKS - play) % SND_BLOCKS;
    /* **1 つは鳴っている最中、もう 1 つは無音の緩衝に使う** */
    if (queued >= SND_BLOCKS - 2U) return 0;

    w = blk_at(g_write_idx);
    for (i = 0; i < len; i++) {
        uint32_t v = ((uint32_t)data[i] * g_dma_range) / 255U;
        w[i * 2U + 0U] = v;     /* L */
        w[i * 2U + 1U] = v;     /* R (モノラルなので同じ) */
    }
    /* 足りないぶんは無音で埋める。**前回の残りを鳴らさない** */
    for (i = len; i < SND_BLOCK_SAMPLES; i++) {
        uint32_t v = g_dma_range / 2U;
        w[i * 2U + 0U] = v;
        w[i * 2U + 1U] = v;
    }

    /* **次のブロックを無音で潰す。**続きが来なかったときに、一周してから
     * 同じ音がもう一度鳴るのを防ぐ */
    blk_silence((g_write_idx + 1U) % SND_BLOCKS);
    __asm__ volatile("dsb sy" ::: "memory");

    g_write_idx = (g_write_idx + 1U) % SND_BLOCKS;
    return (int)len;
}

/* **この機械に BCM の周辺があるか。**
 *
 * boot.c は QEMU virt でも同じ道を通る。**virt に PWM は無い**ので、
 * 番地を触った瞬間に data abort で落ちる (実測: FAR=0xfe20c800、
 * ESR=0x96000045)。mailbox と同じ目印で見分ける — Pi の DTB は
 * mailbox も emmc2 も持っている */
static int machine_has_pwm(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    if (!b) return 0;
    return (b->mbox_base != 0 || b->emmc2_base != 0);
}

void sound_init(void) {
    if (!machine_has_pwm()) {
        aarch64_uart_puts("  sound     : 無し (PWM が無い機械)\n");
        return;
    }
    /* **クロックが先。**PWM はクロックが止まった状態で書くとバスエラーに
     * なり、STA の BERR (bit8) が立ったまま残る。2026-08-19 の版は
     * PWM_CTL を先に叩いており、実測で sta=0x102 (BERR=1 / STA1=0) だった */
    pwm_clock_setup();
    gpio_to_pwm();

    w32(PWM1_BASE, PWM_CTL, 0);          /* 止める */
    delay_us(10);
    w32(PWM1_BASE, PWM_CTL, PWM_CTL_CLRF1);
    delay_us(10);
    /* BERR は書いて消す (write-1-to-clear)。**残骸を持ち越さない** */
    w32(PWM1_BASE, PWM_STA, PWM_STA_BERR);
    delay_us(10);
    g_ready = 1;

    /* ---- D-3: DMA の場所を確保する ------------------------------------
     *
     * **非キャッシュのプールから取る。**DMA と CPU が同じ場所を見るので、
     * キャッシュに載る領域だと食い違う。USB が使っているのと同じプール
     * (kernel/aarch64/vm.c、物理 0x00400000 付近 / Normal-NC)。
     *
     * **低位 1GB に居ることが要る。**legacy DMA は 32bit しか出せない */
    {
        uint64_t cb_bytes  = (uint64_t)SND_BLOCKS * sizeof(dma_cb_t);
        uint64_t buf_bytes = (uint64_t)SND_BLOCKS * SND_BLOCK_WORDS * 4ULL;
        uint64_t cb_pages  = (cb_bytes  + 4095ULL) / 4096ULL;
        uint64_t buf_pages = (buf_bytes + 4095ULL) / 4096ULL;

        g_cb_pa  = aarch64_vm_dma_alloc(cb_pages);
        g_buf_pa = aarch64_vm_dma_alloc(buf_pages);

        aarch64_uart_puts("  sound dma : ");
        if (!g_cb_pa || !g_buf_pa) {
            g_cb_pa = 0;
            g_buf_pa = 0;
            aarch64_uart_puts("*** プールから取れない (CPU 直書きに退く)\n");
        } else if ((g_cb_pa >> 32) || (g_buf_pa >> 32)) {
            /* **32bit を超えたら使えない。**黙って壊れるより先に言う */
            g_cb_pa = 0;
            g_buf_pa = 0;
            aarch64_uart_puts("*** 1GB より上に取れた。legacy DMA から見えない\n");
        } else {
            aarch64_uart_puts("cb=0x");
            aarch64_uart_puthex64(g_cb_pa);
            aarch64_uart_puts(" buf=0x");
            aarch64_uart_puthex64(g_buf_pa);
            aarch64_uart_puts("  ");
            aarch64_uart_putdec64(SND_BLOCKS);
            aarch64_uart_puts(" ブロック x ");
            aarch64_uart_putdec64(SND_BLOCK_SAMPLES);
            aarch64_uart_puts(" サンプル  ch");
            aarch64_uart_putdec64(DMA_CH);
            aarch64_uart_puts(" dreq");
            aarch64_uart_putdec64(DREQ_PWM1);
            aarch64_uart_puts("\n");
        }
    }

    /* **クロックが本当に回っているかを読み戻す。**
     * CM_PWMCTL の BUSY (bit7) が立っていれば回っている。
     * 立っていなければ、いくら PWM を叩いても出力は動かない
     * (カウンタが進まないので、レベルが張り付いたままになる) */
    {
        uint32_t ctl = r32(CM_BASE, CM_PWMCTL);
        uint32_t div = r32(CM_BASE, CM_PWMDIV);
        aarch64_uart_puts("  cm pwmctl : ");
        aarch64_uart_puthex64(ctl);
        aarch64_uart_puts("  src=");
        aarch64_uart_putdec64(ctl & 0xFU);
        aarch64_uart_puts(" enab=");
        aarch64_uart_putdec64((ctl >> 4) & 1U);
        aarch64_uart_puts(" busy=");
        aarch64_uart_putdec64((ctl >> 7) & 1U);
        {
            uint32_t sta = r32(PWM1_BASE, PWM_STA);
            aarch64_uart_puts("\n  pwm sta0  : ");
            aarch64_uart_puthex64(sta);
            aarch64_uart_puts("  berr=");
            aarch64_uart_putdec64((sta & PWM_STA_BERR) ? 1U : 0U);
            aarch64_uart_puts("  ← クリア後。1 なら初期化中の書き込みで出ている");
        }
        aarch64_uart_puts("\n  cm pwmdiv : ");
        aarch64_uart_puthex64(div);
        aarch64_uart_puts("  divi=");
        aarch64_uart_putdec64((div >> 12) & 0xFFFU);
        aarch64_uart_puts("\n");
    }

    {
        uint64_t f;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
        aarch64_uart_puts("  cntfrq    : ");
        aarch64_uart_putdec64(f);
        aarch64_uart_puts(" Hz (armstub が入れた値。触っていない)\n");
    }

    aarch64_uart_puts("  pwm clk   : ");
    aarch64_uart_putdec64(PWM_CLK_HZ);
    aarch64_uart_puts(" Hz (osc ");
    aarch64_uart_putdec64(OSC_HZ);
    aarch64_uart_puts(" / ");
    aarch64_uart_putdec64(PWM_DIVI);
    aarch64_uart_puts(")\n");
}

/* **どの書き込みが BERR を立てるかを 1 回の起動で特定する。**
 * 立っていなければ何も出さないので、平常時は静か */
static int g_probe;
static void probe_step(const char* what) {
    uint32_t sta;
    delay_us(10);
    if (!g_probe) return;
    sta = r32(PWM1_BASE, PWM_STA);
    aarch64_uart_puts("    step ");
    aarch64_uart_puts(what);
    aarch64_uart_puts(" sta=");
    aarch64_uart_puthex64(sta);
    aarch64_uart_puts(" berr=");
    aarch64_uart_putdec64((sta & PWM_STA_BERR) ? 1U : 0U);
    aarch64_uart_puts(" sta1=");
    aarch64_uart_putdec64((sta & PWM_STA_STA1) ? 1U : 0U);
    aarch64_uart_puts(" sta2=");
    aarch64_uart_putdec64((sta & PWM_STA_STA2) ? 1U : 0U);
    aarch64_uart_puts("\n");
}

/* **duty 50% の矩形波を可聴域で出す。**
 * range = PWM クロック / 周波数、data = range / 2 */
void sound_beep_start(uint32_t freq_hz) {
    uint32_t range;

    if (!g_ready || freq_hz == 0) return;
    /* 下は PWM クロック / 2^32 まで出せるが、可聴域の外は意味が無い。
     * 上は range が 2 を切ると duty が刻めない */
    if (freq_hz < 20U) freq_hz = 20U;
    if (freq_hz > (PWM_CLK_HZ / 2U)) freq_hz = PWM_CLK_HZ / 2U;

    range = PWM_CLK_HZ / freq_hz;
    if (range < 2U) range = 2U;

    /* **1 つ書くごとに間を空ける。**PWM は PWM クロック (いまは 1MHz) で
     * 動いており、CPU の速さで続けて叩くとバスエラーになる */
    w32(PWM1_BASE, PWM_CTL, 0);                 probe_step("ctl=0");
    w32(PWM1_BASE, PWM_STA, PWM_STA_BERR);      probe_step("berr clr");
    w32(PWM1_BASE, PWM_RNG1, range);            probe_step("rng1");
    w32(PWM1_BASE, PWM_DAT1, range / 2U);       probe_step("dat1");
    w32(PWM1_BASE, PWM_RNG2, range);            probe_step("rng2");
    w32(PWM1_BASE, PWM_DAT2, range / 2U);       probe_step("dat2");
    /* mark-space = duty がそのまま出る。**これを入れないと
     * パルスがばらけて矩形波にならない** */
    w32(PWM1_BASE, PWM_CTL,
        PWM_CTL_PWEN1 | PWM_CTL_MSEN1 | PWM_CTL_PWEN2 | PWM_CTL_MSEN2);
    probe_step("ctl=on");
}

void sound_beep_stop(void) {
    if (!g_ready) return;
    w32(PWM1_BASE, PWM_CTL, 0);
}


/* **段階 2: PCM を FIFO に流し込む。**
 *
 * ビープは RNG/DAT を据え置いて矩形波を出しっぱなしにするが、PCM は
 * **1 サンプルごとに DAT を差し替える**。それを CPU から間に合わせるのは
 * 無理なので、PWM の FIFO を使う (USEF=1)。FIFO は 16 語で ch1/ch2 に
 * 交互に振り分けられるため、**モノラルでも 1 サンプルにつき 2 語**書く。
 *
 * **MSEN は立てない。**M/S はパルスを 1 か所に固めるので、可聴域に
 * 折り返しが乗る。既定の PWM アルゴリズムはパルスをばらけさせるので、
 * ジャックの RC を通したあとの波形がなめらかになる。
 *
 * **D-3 で DMA に移した。**この関数は「積んで、鳴り終わるまで待つ」形で、
 * 起動時の自己診断が経過時間からクロックの当否を見るために残してある。
 * **DOOM のようにゲームループから鳴らす側は sound_pcm_submit_u8 を使う** —
 * あちらは積むだけで戻る */
int sound_pcm_play_u8(const uint8_t* data, uint32_t len, uint32_t sample_rate) {
    uint32_t done = 0;
    uint64_t guard;

    if (!g_ready) return -1;
    if (!data || len == 0U) return 0;
    if (sample_rate < 4000U)  sample_rate = 4000U;
    if (sample_rate > 44100U) sample_rate = 44100U;

    /* **積み終わるまで待つ。**満杯なら鳴り終わるのを待つだけなので、
     * 待ちは高々 1 ブロック (32ms @16kHz) */
    guard = arch_time_now_ms() + 1000ULL +
            ((uint64_t)len * 1000ULL) / (uint64_t)sample_rate;
    while (done < len) {
        int n = sound_pcm_submit_u8(data + done, len - done, sample_rate);
        if (n < 0) return -1;
        if (n == 0) {
            if (arch_time_now_ms() > guard) break;   /* 進まない。諦める */
            __asm__ volatile("yield");
            continue;
        }
        done += (uint32_t)n;
    }

    /* **鳴り終わるまで待つ。**呼ぶ側 (起動時の自己診断) は経過時間で
     * クロックの当否を見るので、積んだだけで戻ると測れない。
     * **DOOM が使うのは sound_pcm_submit_u8 のほう** */
    sound_pcm_drain();
    return (int)done;
}

/* 積んだぶんが鳴り終わるまで待つ。**時限つき** */
void sound_pcm_drain(void) {
    uint64_t guard;
    if (!g_ready || !g_dma_running) return;
    guard = arch_time_now_ms() + 5000ULL;
    for (;;) {
        uint32_t play = dma_play_idx();
        /* 書き位置に追いつき、その 1 つ先 (無音で潰したところ) まで
         * 進んだら鳴り終わり */
        if (play == g_write_idx) break;
        if (arch_time_now_ms() > guard) break;
        __asm__ volatile("yield");
    }
    /* **鳴り終わったら全部黙らせる。**タイマの掃除に任せると最大 10ms
     * 遅れるうえ、呼んだ側は「戻ったのにまだ鳴っている」を見ることになる */
    {
        uint32_t i;
        for (i = 0; i < SND_BLOCKS; i++) blk_silence(i);
        g_clean_idx = g_write_idx;
        __asm__ volatile("dsb sy" ::: "memory");
    }
}

/* **段階 2 の確認。耳と時計の両方で見る。**
 *
 * 耳: ビープ (矩形波) と三角波は音色がはっきり違う。「ピー」のあとに
 *     「ブー」寄りの音が来れば FIFO の道が通っている。
 *
 * 時計: **ここが本題。**サンプル数と標本化周波数は分かっているので、
 *     再生に何 ms かかるべきかは計算で出る。実測がそこから外れていたら
 *     **PWM のクロックが思っている値ではない**。
 *     OSC_HZ を 54MHz と仮定しているが、Pi 3 までの 19.2MHz だとすると
 *     実クロックは 2.8125 倍遅いので、再生も 2.8125 倍長くかかる。
 *     **耳で音程を当てるより、こちらのほうが確実に切り分けられる** */
#define PCM_RATE    8000U
#define PCM_LEN     4000U       /* 0.5 秒 */
static uint8_t g_pcm_buf[PCM_LEN];

static void pcm_selftest(void) {
    uint32_t i, phase = 0U;
    /* 440Hz の三角波。**矩形波と音色を変えて、FIFO の道を通ったことが
     * 耳でも分かるようにする。**位相は 16.16 の固定小数 */
    const uint32_t inc = (440U * 65536U) / PCM_RATE;
    uint64_t t0, ms;
    int n;

    for (i = 0; i < PCM_LEN; i++) {
        uint32_t p = (phase >> 8) & 0xFFU;      /* 0..255 の のこぎり */
        g_pcm_buf[i] = (uint8_t)((p < 128U) ? (p * 2U) : (255U - (p - 128U) * 2U));
        phase += inc;
    }

    aarch64_uart_puts("  pcm       : ");
    aarch64_uart_putdec64(PCM_LEN);
    aarch64_uart_puts(" サンプル @");
    aarch64_uart_putdec64(PCM_RATE);
    aarch64_uart_puts("Hz 三角波 440Hz  range=");
    aarch64_uart_putdec64(PWM_CLK_HZ / PCM_RATE);
    aarch64_uart_puts("\n");

    t0 = arch_time_now_ms();
    n  = sound_pcm_play_u8(g_pcm_buf, PCM_LEN, PCM_RATE);
    ms = arch_time_now_ms() - t0;

    aarch64_uart_puts("            返り値=");
    aarch64_uart_putdec64((uint64_t)(n < 0 ? 0 : n));
    if (n < 0) aarch64_uart_puts(" (失敗)");
    aarch64_uart_puts("  期待 ");
    aarch64_uart_putdec64((uint64_t)PCM_LEN * 1000ULL / PCM_RATE);
    aarch64_uart_puts(" ms  実測 ");
    aarch64_uart_putdec64(ms);
    aarch64_uart_puts(" ms\n            ");
    /* **ここで OSC_HZ の当否が出る** */
    if (ms >= 400ULL && ms <= 620ULL) {
        aarch64_uart_puts("→ 一致。OSC_HZ=54MHz の仮定は正しい");
    } else if (ms >= 1200ULL && ms <= 1650ULL) {
        aarch64_uart_puts("→ 約 2.8 倍長い。**OSC は 54MHz ではなく 19.2MHz**");
    } else {
        aarch64_uart_puts("→ どちらでもない。FIFO の待ちかクロックを見直す");
    }
    aarch64_uart_puts("\n");
}

/* **鳴らして確かめる。**耳で聞くしか確認の手が無いので、
 * 音程が分かるように 3 音を並べる (ラ / 1kHz / 高いラ) */
void aarch64_sound_selftest(void) {
    /* **低い方から高い方へ。**間を長く空けて、何音鳴ったかを
     * 数えられるようにする (前回は 3 音が 1 回に聞こえた) */
    static const uint32_t tone[3] = { 262U, 523U, 1047U };  /* ド / 1 オクターブ上 / さらに上 */
    int i;
    if (!g_ready) return;

    /* **まず待ちが効いているかを測る。**耳に頼らずに分かる。
     * 400ms 頼んで 400ms 前後経っていなければ、音の長さの話は
     * そこから先へ進めない */
    {
        uint64_t t0 = arch_time_now_ms();
        delay_ms(400);
        aarch64_uart_puts("  delay     : 400ms 頼んで ");
        aarch64_uart_putdec64(arch_time_now_ms() - t0);
        aarch64_uart_puts(" ms 経った\n");
    }

    for (i = 0; i < 3; i++) {
        uint32_t sta, ctl, dat;
        aarch64_uart_puts("  beep      : ");
        aarch64_uart_putdec64(tone[i]);
        aarch64_uart_puts(" Hz range=");
        aarch64_uart_putdec64(PWM_CLK_HZ / tone[i]);
        aarch64_uart_puts("\n");
        /* **1 音目だけ、書き込み 1 つごとに STA を出す。**
         * どの書き込みで BERR が立つかがこれで分かる */
        g_probe = (i == 0);
        sound_beep_start(tone[i]);
        g_probe = 0;
        /* **鳴らしている最中のレジスタを読む。**
         * STA1 (bit9) が「チャネル 1 が送出中」。**bit8 は BERR** で、
         * ここを取り違えると「送出中」と読めてしまう */
        sta = r32(PWM1_BASE, PWM_STA);
        ctl = r32(PWM1_BASE, PWM_CTL);
        dat = r32(PWM1_BASE, PWM_DAT1);
        aarch64_uart_puts("            sta=");
        aarch64_uart_puthex64(sta);
        aarch64_uart_puts(" berr=");
        aarch64_uart_putdec64((sta & PWM_STA_BERR) ? 1U : 0U);
        aarch64_uart_puts(" sta1=");
        aarch64_uart_putdec64((sta & PWM_STA_STA1) ? 1U : 0U);
        aarch64_uart_puts(" sta2=");
        aarch64_uart_putdec64((sta & PWM_STA_STA2) ? 1U : 0U);
        aarch64_uart_puts(" ctl=");
        aarch64_uart_puthex64(ctl);
        aarch64_uart_puts(" dat=");
        aarch64_uart_putdec64(dat);
        aarch64_uart_puts("\n");
        delay_ms(1000);
        sound_beep_stop();
        delay_ms(500);
    }
    aarch64_uart_puts("  beep      : 3 音おわり\n");

    pcm_selftest();
}
