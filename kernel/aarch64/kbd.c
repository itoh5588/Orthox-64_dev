/*
 * USB HID キーボードを key_event に変換する層。
 *
 * ---- なぜ変換が要るか ------------------------------------------------------
 *
 * USB が返すのは **HID の usage コード** (a = 4, b = 5, 上矢印 = 82)。
 * 一方 DOOM (user/doomgeneric/doomgeneric/doomgeneric_orthos.c) が待って
 * いるのは **PC の scancode** (set 1) と ASCII。x86 のキーボードドライバに
 * 合わせた形なので、**同じ struct key_event で渡せるよう usage を
 * scancode に直す。**
 *
 * ---- 押した / 離した -------------------------------------------------------
 *
 * **boot protocol のレポートは「いま押されているキーの一覧」**であって、
 * 押した / 離したの通知ではない:
 *
 *   [0] 修飾キー  [1] 予約  [2..7] 同時に押されているキーの usage
 *
 * 前回との差分を取って押下と解放を作る。**ここをやらないと DOOM が
 * 歩き続けられない** — あちらは「押した」で歩き始め「離した」で止まる。
 */
#include <stdint.h>
#include "syscall.h"
#include "usb.h"

/* ---- usage -> (scancode, ascii) ------------------------------------------
 *
 * **DOOM が見る所だけ埋める。** convertToDoomKey は特殊キーを scancode で
 * 見て、それ以外は ascii に落ちる。全部の usage を埋める必要はない。
 *
 * scancode は PC set 1。**矢印などの拡張キーは 0xE0 が前置される種類**で、
 * DOOM 側は 0xC8 (= 0x48 | 0x80) の形で待っているのでそれに合わせる */
typedef struct { uint8_t usage; uint8_t scancode; uint16_t ascii; } kbd_map_t;

static const kbd_map_t g_map[] = {
    /* 英字。usage 4..29 が a..z */
    {  4, 0x1e, 'a' }, {  5, 0x30, 'b' }, {  6, 0x2e, 'c' }, {  7, 0x20, 'd' },
    {  8, 0x12, 'e' }, {  9, 0x21, 'f' }, { 10, 0x22, 'g' }, { 11, 0x23, 'h' },
    { 12, 0x17, 'i' }, { 13, 0x24, 'j' }, { 14, 0x25, 'k' }, { 15, 0x26, 'l' },
    { 16, 0x32, 'm' }, { 17, 0x31, 'n' }, { 18, 0x18, 'o' }, { 19, 0x19, 'p' },
    { 20, 0x10, 'q' }, { 21, 0x13, 'r' }, { 22, 0x1f, 's' }, { 23, 0x14, 't' },
    { 24, 0x16, 'u' }, { 25, 0x2f, 'v' }, { 26, 0x11, 'w' }, { 27, 0x2d, 'x' },
    { 28, 0x15, 'y' }, { 29, 0x2c, 'z' },
    /* 数字。usage 30..38 が 1..9、39 が 0 */
    { 30, 0x02, '1' }, { 31, 0x03, '2' }, { 32, 0x04, '3' }, { 33, 0x05, '4' },
    { 34, 0x06, '5' }, { 35, 0x07, '6' }, { 36, 0x08, '7' }, { 37, 0x09, '8' },
    { 38, 0x0a, '9' }, { 39, 0x0b, '0' },
    /* **DOOM が実際に使うのはここから下** */
    { 40, 0x1c, '\r' },      /* Enter  = 決定 */
    { 41, 0x01, 27   },      /* Esc    = メニュー */
    { 42, 0x0e, '\b' },      /* BS */
    { 43, 0x0f, '\t' },      /* Tab    = 地図 */
    { 44, 0x39, ' '  },      /* Space  = 扉を開ける (KEY_USE) */
    { 45, 0x0c, '-'  },
    { 46, 0x0d, '='  },
    { 55, 0x34, '.'  },
    { 56, 0x35, '/'  },
    /* 矢印。**上位ビットを立てた形で渡す** (DOOM 側が 0xC8 等で待つ) */
    { 79, 0xcd, 0 },         /* → */
    { 80, 0xcb, 0 },         /* ← */
    { 81, 0xd0, 0 },         /* ↓ */
    { 82, 0xc8, 0 },         /* ↑ */
    { 74, 0xc7, 0 },         /* Home */
    { 77, 0xcf, 0 },         /* End */
    { 75, 0xc9, 0 },         /* PgUp */
    { 78, 0xd1, 0 },         /* PgDn */
    { 73, 0xd2, 0 },         /* Ins */
    { 76, 0xd3, 0 },         /* Del */
};

#define KBD_MAP_COUNT (sizeof(g_map) / sizeof(g_map[0]))

/* 修飾キーはレポートの [0] にビットで来る。**usage には現れない** */
#define MOD_LCTRL   (1U << 0)
#define MOD_LSHIFT  (1U << 1)
#define MOD_LALT    (1U << 2)
#define MOD_RCTRL   (1U << 4)
#define MOD_RSHIFT  (1U << 5)
#define MOD_RALT    (1U << 6)

static int map_usage(uint8_t usage, uint8_t* scancode, uint16_t* ascii) {
    for (uint32_t i = 0; i < KBD_MAP_COUNT; i++) {
        if (g_map[i].usage == usage) {
            *scancode = g_map[i].scancode;
            *ascii = g_map[i].ascii;
            return 1;
        }
    }
    return 0;
}

/* ---- イベントの待ち行列 ---------------------------------------------------
 *
 * **取りこぼしても止まらないほうがよい。** 溢れたら古いものを捨てる。
 * DOOM は毎フレーム取りに来るので、深さは 32 で足りる */
#define KBD_QUEUE_SIZE 32
static struct key_event g_queue[KBD_QUEUE_SIZE];
static uint32_t g_head, g_tail;

static void kbd_push(uint8_t pressed, uint8_t scancode, uint16_t ascii) {
    uint32_t next = (g_head + 1U) % KBD_QUEUE_SIZE;
    if (next == g_tail) g_tail = (g_tail + 1U) % KBD_QUEUE_SIZE;   /* 古いものを捨てる */
    g_queue[g_head].pressed = pressed;
    g_queue[g_head].scancode = scancode;
    g_queue[g_head].ascii = ascii;
    g_head = next;
}

/* 前回のレポート。差分を取るために覚えておく */
static uint8_t g_prev[8];

static void emit_mod_diff(uint8_t now, uint8_t before, uint8_t bit,
                          uint8_t scancode) {
    if ((now & bit) && !(before & bit))  kbd_push(1, scancode, 0);
    if (!(now & bit) && (before & bit))  kbd_push(0, scancode, 0);
}

/* レポート 1 つを差分にして待ち行列へ積む */
static void kbd_apply_report(const uint8_t rep[8]) {
    /* 修飾キー。**DOOM は ctrl (撃つ) と shift (走る) を見る** */
    emit_mod_diff(rep[0], g_prev[0], MOD_LCTRL,  0x1d);
    emit_mod_diff(rep[0], g_prev[0], MOD_RCTRL,  0x9d);
    emit_mod_diff(rep[0], g_prev[0], MOD_LSHIFT, 0x2a);
    emit_mod_diff(rep[0], g_prev[0], MOD_RSHIFT, 0x36);
    emit_mod_diff(rep[0], g_prev[0], MOD_LALT,   0x38);
    emit_mod_diff(rep[0], g_prev[0], MOD_RALT,   0xb8);

    /* 押された: 今回に在って前回に無いもの */
    for (int i = 2; i < 8; i++) {
        uint8_t u = rep[i];
        int found = 0;
        if (u == 0) continue;
        for (int j = 2; j < 8; j++) if (g_prev[j] == u) { found = 1; break; }
        if (!found) {
            uint8_t sc; uint16_t as;
            if (map_usage(u, &sc, &as)) kbd_push(1, sc, as);
        }
    }
    /* 離された: 前回に在って今回に無いもの */
    for (int i = 2; i < 8; i++) {
        uint8_t u = g_prev[i];
        int found = 0;
        if (u == 0) continue;
        for (int j = 2; j < 8; j++) if (rep[j] == u) { found = 1; break; }
        if (!found) {
            uint8_t sc; uint16_t as;
            if (map_usage(u, &sc, &as)) kbd_push(0, sc, as);
        }
    }
    for (int i = 0; i < 8; i++) g_prev[i] = rep[i];
}

/* ---- 外から呼ぶ口 ---------------------------------------------------------
 *
 * **ここで USB を叩く。** 割り込みは使っていないので、取りに来られた
 * ときにポーリングする。DOOM は毎フレーム呼ぶので間隔としては足りる。
 *
 * 戻り値: 1 = 取れた / 0 = 何も無い */
int aarch64_kbd_get_event(struct key_event* ev) {
    uint8_t rep[8];
    if (!ev) return 0;

    /* **溜まっているものを先に吐く。** 1 回のレポートで押下と解放が
     * 同時に出ることがあるので、キューを空にしてから次を読む */
    if (g_head != g_tail) {
        *ev = g_queue[g_tail];
        g_tail = (g_tail + 1U) % KBD_QUEUE_SIZE;
        return 1;
    }

    if (!usb_hid_keyboard_ready()) return 0;
    if (usb_hid_keyboard_poll(rep) != 0) return 0;
    kbd_apply_report(rep);

    if (g_head != g_tail) {
        *ev = g_queue[g_tail];
        g_tail = (g_tail + 1U) % KBD_QUEUE_SIZE;
        return 1;
    }
    return 0;
}
