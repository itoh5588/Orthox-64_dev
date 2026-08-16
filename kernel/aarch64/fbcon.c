/*
 * フレームバッファのテキストコンソール。**カーネルのログを HDMI にも出す。**
 *
 * シリアルは残す。**画面はシリアルの代わりではなく、増設**である:
 *   - 実機で画面だけ見たいとき (変換器を繋がずに済む)
 *   - 画面が出ていること自体が、フレームバッファの健全性の確認になる
 *
 * ---- 描き方 ----------------------------------------------------------------
 *
 * 字形は kernel/aarch64/font8x8.c (scripts/gen_font8x8.py が生成)。
 * 8x8 の升目を **AARCH64_FBCON_SCALE 倍**に拡大して描く。1024x768 のとき
 * 2 倍で 64 桁 x 48 行。等倍だと 128 桁 x 96 行になるが、テレビに繋いだとき
 * 字が小さすぎて読めない。
 *
 * ---- 遅さについて ----------------------------------------------------------
 *
 * **フレームバッファは Normal NC (キャッシュ無効) で張ってある** ので、
 * 読み書きはどちらも DRAM に直行する。とくにスクロールは画面 1 枚ぶんを
 * 読んで書くので重い。**64 ビット単位でまとめて動かして回数を減らしている。**
 *
 * それでも起動ログを全部流すと目に見えて時間がかかる。**シリアルの方が
 * 速いので、速さが要る場面ではそちらを見ること。**
 */
#include <stdint.h>
#include "aarch64/fb.h"
#include "aarch64/vm.h"

/* 96 個目は「表に無い文字」の升目 (scripts/gen_font8x8.py) */
#define FONT_MISSING 95
extern const uint8_t aarch64_font8x8[96][8];

#define FONT_W 8U
#define FONT_H 8U

/* 拡大率。**1024x768 で 2 倍 = 64 桁 x 48 行** */
#define AARCH64_FBCON_SCALE 2U

#define FBCON_FG 0x00c8c8c8U     /* 明るすぎない灰。白は目が疲れる */
#define FBCON_BG 0x00000000U

static int      g_ready;
static uint32_t g_cols, g_rows;
static uint32_t g_col, g_row;
static uint32_t g_stride;        /* 1 行 (画素) */
static uint32_t g_cell_w, g_cell_h;

/* fb.c と同じ理屈。**MMU の前後で番地の見え方が変わる。**
 * 判断は「MMU が入っているか」で行う (vm.h の aarch64_vm_mmu_enabled) */
static volatile uint32_t* fbcon_ptr(void) {
    const aarch64_fb_info_t* fb = aarch64_fb_info();
    if (aarch64_vm_mmu_enabled()) return (volatile uint32_t*)(uintptr_t)AARCH64_FB_VA_BASE;
    return (volatile uint32_t*)(uintptr_t)fb->base;
}

static void fbcon_clear_rows(uint32_t y0, uint32_t rows_px) {
    volatile uint32_t* p = fbcon_ptr();
    const aarch64_fb_info_t* fb = aarch64_fb_info();
    uint32_t x, y;
    for (y = y0; y < y0 + rows_px && y < fb->height; y++) {
        for (x = 0; x < fb->width; x++) p[y * g_stride + x] = FBCON_BG;
    }
}

void aarch64_fbcon_init(void) {
    const aarch64_fb_info_t* fb = aarch64_fb_info();
    g_ready = 0;
    if (!fb || fb->base == 0 || fb->pitch == 0) return;

    g_stride = fb->pitch / 4U;
    g_cell_w = FONT_W * AARCH64_FBCON_SCALE;
    g_cell_h = FONT_H * AARCH64_FBCON_SCALE;
    g_cols = fb->width / g_cell_w;
    g_rows = fb->height / g_cell_h;
    if (g_cols == 0 || g_rows == 0) return;

    g_col = g_row = 0;
    fbcon_clear_rows(0, fb->height);
    g_ready = 1;
}

int aarch64_fbcon_ready(void) { return g_ready; }
uint32_t aarch64_fbcon_cols(void) { return g_cols; }
uint32_t aarch64_fbcon_rows(void) { return g_rows; }

static void fbcon_draw_glyph(uint32_t cx, uint32_t cy, char ch) {
    volatile uint32_t* p = fbcon_ptr();
    const uint8_t* g;
    uint32_t gx, gy, sx, sy;
    uint32_t px0 = cx * g_cell_w;
    uint32_t py0 = cy * g_cell_h;
    unsigned char c = (unsigned char)ch;

    /* **表に無い文字は空白にしない。** 「字が無い」と「空白」を混ぜると
     * 化けに気づけない。専用の升目を出す。
     *
     * **日本語 (UTF-8) はここを通る。** 1 文字が 3 バイトなので升目が
     * 3 つ並ぶ。起動ログの説明文はほぼこれになる */
    if (c < 0x20 || c > 0x7e) {
        g = aarch64_font8x8[FONT_MISSING];
    } else {
        g = aarch64_font8x8[c - 0x20];
    }

    for (gy = 0; gy < FONT_H; gy++) {
        uint8_t bits = g[gy];
        for (gx = 0; gx < FONT_W; gx++) {
            uint32_t v = (bits & (0x80U >> gx)) ? FBCON_FG : FBCON_BG;
            for (sy = 0; sy < AARCH64_FBCON_SCALE; sy++) {
                uint32_t row = (py0 + gy * AARCH64_FBCON_SCALE + sy) * g_stride
                             + px0 + gx * AARCH64_FBCON_SCALE;
                for (sx = 0; sx < AARCH64_FBCON_SCALE; sx++) p[row + sx] = v;
            }
        }
    }
}

/* 1 行ぶん上へずらす。
 *
 * **64 ビット単位で動かす。** NC のフレームバッファは 1 回の読み書きが
 * そのまま DRAM に出るので、32 ビットずつだと回数が倍になる。
 * ピッチは 4 の倍数だが 8 の倍数とは限らないので、端数は 32 ビットで拾う */
static void fbcon_scroll(void) {
    const aarch64_fb_info_t* fb = aarch64_fb_info();
    volatile uint32_t* p = fbcon_ptr();
    uint32_t move_px = (g_rows - 1U) * g_cell_h;
    uint32_t y, x;
    uint32_t pairs = fb->width / 2U;

    for (y = 0; y < move_px; y++) {
        volatile uint64_t* dst = (volatile uint64_t*)(p + y * g_stride);
        volatile uint64_t* src = (volatile uint64_t*)(p + (y + g_cell_h) * g_stride);
        for (x = 0; x < pairs; x++) dst[x] = src[x];
        if (fb->width & 1U) {
            p[y * g_stride + fb->width - 1U] = p[(y + g_cell_h) * g_stride + fb->width - 1U];
        }
    }
    fbcon_clear_rows(move_px, g_cell_h);
}

static void fbcon_newline(void) {
    g_col = 0;
    if (++g_row >= g_rows) {
        fbcon_scroll();
        g_row = g_rows - 1U;
    }
}

/* **1 文字。ここが唯一の入口。** aarch64_uart_putchar から呼ばれる。
 * 画面が無い機械では何もしないので、呼び出し側は条件を書かなくてよい */
void aarch64_fbcon_putc(char c) {
    if (!g_ready) return;

    if (c == '\n') { fbcon_newline(); return; }
    if (c == '\r') { g_col = 0; return; }
    if (c == '\t') {
        do { aarch64_fbcon_putc(' '); } while (g_col & 7U);
        return;
    }
    if (c == '\b') { if (g_col > 0) g_col--; return; }

    if (g_col >= g_cols) fbcon_newline();
    fbcon_draw_glyph(g_col, g_row, c);
    g_col++;
}
