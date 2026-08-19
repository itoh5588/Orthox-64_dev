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
/* BCM2711 のプル制御。**BCM2835 の GPPUD/GPPUDCLK とは別物** */
#define GPIO_PUP_PDN2   0xE8U   /* ピン 32..47。1 ピン 2 ビット */

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
#define PWM_RNG2        0x20U
#define PWM_DAT2        0x24U

#define PWM_CTL_PWEN1   (1U << 0)
#define PWM_CTL_MSEN1   (1U << 7)
#define PWM_CTL_CLRF1   (1U << 6)
#define PWM_CTL_PWEN2   (1U << 8)
#define PWM_CTL_MSEN2   (1U << 15)

/* 源発振 (上のコメントの前提) */
#define OSC_HZ          54000000U
/* PWM のクロック。**54 / 54 = ちょうど 1MHz** にして計算を見やすくする。
 * ビープの分解能は 1us 刻みで、可聴域には十分 */
#define PWM_DIVI        54U
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

/* **空回しではなく時刻で待つ。**回数は最適化と CPU の速さで変わる */
static void delay_ms(uint32_t ms) {
    uint64_t t0 = arch_time_now_ms();
    while ((arch_time_now_ms() - t0) < (uint64_t)ms) __asm__ volatile("yield");
}

/* GPIO 40/41 を ALT0 (= PWM1_0 / PWM1_1) にする。プルは無し (DTB の
 * bias-disable) */
static void gpio_to_pwm(void) {
    uint32_t v = r32(GPIO_BASE, GPFSEL4);
    v &= ~(0x7U << 0);          /* GPIO40 */
    v &= ~(0x7U << 3);          /* GPIO41 */
    v |=  (0x4U << 0);          /* ALT0 */
    v |=  (0x4U << 3);
    w32(GPIO_BASE, GPFSEL4, v);

    v = r32(GPIO_BASE, GPIO_PUP_PDN2);
    v &= ~(0x3U << 16);         /* GPIO40: プル無し */
    v &= ~(0x3U << 18);         /* GPIO41 */
    w32(GPIO_BASE, GPIO_PUP_PDN2, v);
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
    w32(PWM1_BASE, PWM_CTL, 0);          /* 先に止める */
    gpio_to_pwm();
    pwm_clock_setup();
    w32(PWM1_BASE, PWM_CTL, PWM_CTL_CLRF1);
    g_ready = 1;

    /* **クロックが本当に回っているかを読み戻す。**
     * CM_PWMCTL の BUSY (bit7) が立っていれば回っている。
     * 立っていなければ、いくら PWM を叩いても出力は動かない
     * (カウンタが進まないので、レベルが張り付いたままになる) */
    {
        uint32_t ctl = r32(CM_BASE, CM_PWMCTL);
        uint32_t div = r32(CM_BASE, CM_PWMDIV);
        aarch64_uart_puts("  cm pwmctl : 0x");
        aarch64_uart_puthex64(ctl);
        aarch64_uart_puts("  src=");
        aarch64_uart_putdec64(ctl & 0xFU);
        aarch64_uart_puts(" enab=");
        aarch64_uart_putdec64((ctl >> 4) & 1U);
        aarch64_uart_puts(" busy=");
        aarch64_uart_putdec64((ctl >> 7) & 1U);
        aarch64_uart_puts("\n  cm pwmdiv : 0x");
        aarch64_uart_puthex64(div);
        aarch64_uart_puts("  divi=");
        aarch64_uart_putdec64((div >> 12) & 0xFFFU);
        aarch64_uart_puts("\n");
    }

    aarch64_uart_puts("  pwm clk   : ");
    aarch64_uart_putdec64(PWM_CLK_HZ);
    aarch64_uart_puts(" Hz (osc ");
    aarch64_uart_putdec64(OSC_HZ);
    aarch64_uart_puts(" / ");
    aarch64_uart_putdec64(PWM_DIVI);
    aarch64_uart_puts(")\n");
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

    w32(PWM1_BASE, PWM_CTL, 0);
    w32(PWM1_BASE, PWM_RNG1, range);
    w32(PWM1_BASE, PWM_DAT1, range / 2U);
    w32(PWM1_BASE, PWM_RNG2, range);
    w32(PWM1_BASE, PWM_DAT2, range / 2U);
    /* mark-space = duty がそのまま出る。**これを入れないと
     * パルスがばらけて矩形波にならない** */
    w32(PWM1_BASE, PWM_CTL,
        PWM_CTL_PWEN1 | PWM_CTL_MSEN1 | PWM_CTL_PWEN2 | PWM_CTL_MSEN2);
}

void sound_beep_stop(void) {
    if (!g_ready) return;
    w32(PWM1_BASE, PWM_CTL, 0);
}

/* PCM はまだ。**FIFO に流し込む口を別に作る** (段階 2) */
int sound_pcm_play_u8(const uint8_t* data, uint32_t len, uint32_t sample_rate) {
    (void)data; (void)len; (void)sample_rate;
    return -1;
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
        sound_beep_start(tone[i]);
        /* **鳴らしている最中のレジスタを読む。**
         * STA の bit9 (STA1) は「チャネル 1 が送出中」。
         * ここが 0 なら、設定は入っているのに出ていない */
        sta = r32(PWM1_BASE, PWM_STA);
        ctl = r32(PWM1_BASE, PWM_CTL);
        dat = r32(PWM1_BASE, PWM_DAT1);
        aarch64_uart_puts("  sta=0x");
        aarch64_uart_puthex64(sta);
        aarch64_uart_puts(" ctl=0x");
        aarch64_uart_puthex64(ctl);
        aarch64_uart_puts(" dat=");
        aarch64_uart_putdec64(dat);
        aarch64_uart_puts("\n");
        delay_ms(1000);
        sound_beep_stop();
        delay_ms(500);
    }
    aarch64_uart_puts("  beep      : 3 音おわり\n");
}
