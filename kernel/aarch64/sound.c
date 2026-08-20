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
 * いまは CPU から直接流し込む (FULL1 を見ながら書く)。**再生中は
 * この関数から戻らない。**長い音が要るようになったら DMA へ移す */
int sound_pcm_play_u8(const uint8_t* data, uint32_t len, uint32_t sample_rate) {
    uint32_t range, i, spin;

    if (!g_ready) return -1;
    if (!data || len == 0U) return 0;
    /* x86 側の sound.c と同じ範囲に揃える */
    if (sample_rate < 4000U)  sample_rate = 4000U;
    if (sample_rate > 44100U) sample_rate = 44100U;

    /* **1 サンプルの長さ。**これが標本化周期そのものになる */
    range = PWM_CLK_HZ / sample_rate;
    if (range < 2U) range = 2U;

    w32(PWM1_BASE, PWM_CTL, 0);                 delay_us(10);
    w32(PWM1_BASE, PWM_CTL, PWM_CTL_CLRF1);     delay_us(10);
    w32(PWM1_BASE, PWM_STA, PWM_STA_BERR);      delay_us(10);
    w32(PWM1_BASE, PWM_RNG1, range);            delay_us(10);
    w32(PWM1_BASE, PWM_RNG2, range);            delay_us(10);
    w32(PWM1_BASE, PWM_CTL,
        PWM_CTL_PWEN1 | PWM_CTL_USEF1 |
        PWM_CTL_PWEN2 | PWM_CTL_USEF2);         delay_us(10);

    for (i = 0; i < len; i++) {
        /* u8 の 0..255 を 0..range に伸ばす */
        uint32_t v = ((uint32_t)data[i] * range) / 255U;
        int ch;
        for (ch = 0; ch < 2; ch++) {    /* L と R に同じ値 (モノラル) */
            /* **FIFO が空くのを待つ。**待てなければ諦めて戻る —
             * ここで無限に回ると音のために OS が止まる */
            for (spin = 0; spin < 10000000U; spin++) {
                if (!(r32(PWM1_BASE, PWM_STA) & PWM_STA_FULL1)) break;
            }
            if (spin >= 10000000U) {
                w32(PWM1_BASE, PWM_CTL, 0);
                return -1;
            }
            w32(PWM1_BASE, PWM_FIF1, v);
        }
    }

    /* **最後まで流し切ってから止める。**空になった直後はまだ最終
     * サンプルを出している最中なので、1 サンプル分だけ余計に待つ */
    for (spin = 0; spin < 10000000U; spin++) {
        if (r32(PWM1_BASE, PWM_STA) & PWM_STA_EMPT1) break;
    }
    delay_us((range * 2U) / (PWM_CLK_HZ / 1000000U) + 1U);
    w32(PWM1_BASE, PWM_CTL, 0);
    return (int)len;
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
