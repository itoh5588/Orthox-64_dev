#ifndef ORTHOX_AARCH64_FB_H
#define ORTHOX_AARCH64_FB_H

#include <stdint.h>

/* 失敗の理由。**「出ない」の一言では切り分けられない**ので、
 * どこで止まったかを起動ログに出せるようにしておく
 * (emmc2 の g_last_fail と同じ考え方。日報2026-08-15 §4) */
#define AARCH64_FB_FAIL_NONE          0
#define AARCH64_FB_FAIL_NO_MBOX       1   /* mailbox の番地が分からない = Pi ではない */
#define AARCH64_FB_FAIL_MBOX_TIMEOUT  2   /* VC が返事をしない */
#define AARCH64_FB_FAIL_VC_REJECTED   3   /* 返事は来たが応答コードが失敗 */
#define AARCH64_FB_FAIL_BAD_REPLY     4   /* 応答は成功だが番地やピッチが 0 */

typedef struct aarch64_fb_info {
    uint64_t base;      /* ARM の物理番地 (バス番地から直したもの) */
    uint32_t size;
    uint32_t pitch;     /* 1 行のバイト数。**width * 4 とは限らない** */
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t fail;
} aarch64_fb_info_t;

/* **MMU を入れる前に呼ぶ。** 理由は kernel/aarch64/fb.c の冒頭 */
int aarch64_fb_init(uint32_t want_w, uint32_t want_h);
const aarch64_fb_info_t* aarch64_fb_info(void);
void aarch64_fb_fill(uint32_t argb);
void aarch64_fb_test_pattern(void);
void aarch64_fb_mark_top(uint32_t argb);

/* 画面のテキストコンソール (kernel/aarch64/fbcon.c)。
 * **画面が無い機械では putc が何もしない**ので、呼ぶ側に条件は要らない */
void aarch64_fbcon_init(void);
void aarch64_fbcon_putc(char c);
int aarch64_fbcon_ready(void);
uint32_t aarch64_fbcon_cols(void);
uint32_t aarch64_fbcon_rows(void);

#endif
