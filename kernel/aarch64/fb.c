/*
 * Raspberry Pi のフレームバッファ。VideoCore に 1 枚もらって描く。
 *
 * ---- 出典 ----------------------------------------------------------------
 *
 * **仕様から起こした** (kernel/aarch64/mailbox.c の冒頭を参照)。
 * タグ番号と値の並びは Raspberry Pi 公式 wiki の
 * "Mailbox property interface" の記述に拠る。
 *
 * ---- 何をしているか --------------------------------------------------------
 *
 * ファームウェアに「この大きさ・この深さで 1 枚くれ」と頼むと、番地と
 * ピッチを返してくる。**HDMI の設定 (PHY、タイミング、HVS) は全部
 * ファームウェア側がやる** ので、こちらは番地に書くだけでよい。
 *
 * Linux が Pi 4 で HDMI を出す経路 (drivers/gpu/drm/vc4) はこれとは別物で、
 * DRM/KMS を自前で組み立てる。**そちらは数万行あり、今回は要らない。**
 *
 * ---- 呼ぶ位置 --------------------------------------------------------------
 *
 * **MMU を入れる前。** 理由が 2 つある:
 *
 *   - mailbox のバッファにキャッシュ管理が要らない (SCTLR.C = 0)
 *   - **返ってくる番地は pmm が配る RAM の外** なので、ページを確保する
 *     必要がない。vm.c がテーブルを組むときに専用の VA へ張る
 *
 * **HHDM 経由では届かない。** 当初そのつもりだったが、QEMU の raspi4b は
 * RAM 末尾 0x3c000000 の**上**、0x3c100000 に返してきた。HHDM は RAM しか
 * 張らないので届かない。専用の VA (AARCH64_FB_VA_BASE) に張る理由の 1 つ。
 */
#include <stdint.h>
#include "aarch64/boot.h"
#include "aarch64/fb.h"
#include "aarch64/vm.h"

int aarch64_mbox_property(volatile uint32_t* buf);
void aarch64_mbox_init(void);
uint64_t aarch64_mbox_base(void);

/* タグ番号 (公式 wiki の表) */
#define TAG_ALLOCATE_BUFFER   0x00040001U   /* req 4 (境界) / res 8 (番地, 大きさ) */
#define TAG_GET_PITCH         0x00040008U   /* req 0        / res 4 (1 行のバイト数) */
#define TAG_SET_PHYS_WH       0x00048003U   /* 画面の大きさ */
#define TAG_SET_VIRT_WH       0x00048004U   /* バッファの大きさ */
#define TAG_SET_DEPTH         0x00048005U
#define TAG_SET_PIXEL_ORDER   0x00048006U
#define TAG_SET_VIRT_OFFSET   0x00048009U

#define TAG_END               0x00000000U

/* **16 バイト境界に置くこと。** mailbox の語は下位 4 ビットをチャネルに
 * 使うので、番地の下位 4 ビットが 0 でなければならない。
 * .bss に置く = カーネルの直後 = 低位 RAM なので、0xc0000000 の別名も張れる */
static volatile uint32_t g_mbox_buf[64] __attribute__((aligned(16)));

static aarch64_fb_info_t g_fb;

const aarch64_fb_info_t* aarch64_fb_info(void) { return &g_fb; }

/* 要求する既定の画面。**実機が別の大きさを返してきたら、返ってきた値に従う。**
 * HDMI の相手 (モニタ) 次第でファームウェアが丸めることがある */
#define FB_DEFAULT_WIDTH   1024U
#define FB_DEFAULT_HEIGHT  768U
#define FB_DEFAULT_DEPTH   32U

int aarch64_fb_init(uint32_t want_w, uint32_t want_h) {
    volatile uint32_t* b = g_mbox_buf;
    uint32_t i = 0;
    int rc;

    g_fb.base = 0;
    g_fb.size = 0;
    g_fb.pitch = 0;
    g_fb.width = g_fb.height = g_fb.depth = 0;
    g_fb.fail = 0;

    aarch64_mbox_init();
    if (aarch64_mbox_base() == 0) {
        g_fb.fail = AARCH64_FB_FAIL_NO_MBOX;
        return -1;
    }

    if (want_w == 0) want_w = FB_DEFAULT_WIDTH;
    if (want_h == 0) want_h = FB_DEFAULT_HEIGHT;

    /* **1 往復で全部頼む。** タグを 1 つずつ送ると、途中でファームウェアが
     * 中間状態の画面を作ってしまう。まとめて渡すと順に処理される */
    b[i++] = 0;                       /* 全体のバイト数。最後に埋める */
    b[i++] = 0;                       /* 要求 */

    /* **タグの開始位置を控える。** 返りの値はタグごとに位置が変わるので、
     * 先頭からの決め打ちで拾うと隣のタグ番号を読む
     * (実測: 0x00048004 = 294916 を幅として拾っていた) */
    uint32_t phys_at = i;
    b[i++] = TAG_SET_PHYS_WH; b[i++] = 8; b[i++] = 8;
    b[i++] = want_w; b[i++] = want_h;

    b[i++] = TAG_SET_VIRT_WH; b[i++] = 8; b[i++] = 8;
    b[i++] = want_w; b[i++] = want_h;

    b[i++] = TAG_SET_VIRT_OFFSET; b[i++] = 8; b[i++] = 8;
    b[i++] = 0; b[i++] = 0;

    b[i++] = TAG_SET_DEPTH; b[i++] = 4; b[i++] = 4;
    b[i++] = FB_DEFAULT_DEPTH;

    /* pixel order: 0 = BGR、1 = RGB。**どちらで来ているかは画面を見れば
     * 分かる** — テストパターンの赤と青が入れ替わっていたらここを疑う */
    b[i++] = TAG_SET_PIXEL_ORDER; b[i++] = 4; b[i++] = 4;
    b[i++] = 1;

    /* 確保。要求側の 4 バイトは**境界**の指定 (返りでは番地になる) */
    uint32_t alloc_at = i;
    b[i++] = TAG_ALLOCATE_BUFFER; b[i++] = 8; b[i++] = 8;
    b[i++] = 16; b[i++] = 0;

    uint32_t pitch_at = i;
    b[i++] = TAG_GET_PITCH; b[i++] = 4; b[i++] = 4;
    b[i++] = 0;

    b[i++] = TAG_END;
    b[0] = i * 4U;

    rc = aarch64_mbox_property(b);
    if (rc != 0) {
        g_fb.fail = (rc == -4) ? AARCH64_FB_FAIL_VC_REJECTED
                               : AARCH64_FB_FAIL_MBOX_TIMEOUT;
        return -1;
    }

    /* **返ってくる番地はバス番地。** 0xc0000000 の別名を外して ARM の物理に
     * 直さないと、どこにも繋がらない所を叩く */
    g_fb.base  = (uint64_t)(b[alloc_at + 3] & 0x3fffffffU);
    g_fb.size  = b[alloc_at + 4];
    g_fb.pitch = b[pitch_at + 3];

    /* 実際に採用された大きさを読み戻す。**頼んだ値をそのまま覚えない** —
     * ファームウェアが丸めることがある */
    g_fb.width  = b[phys_at + 3];
    g_fb.height = b[phys_at + 4];
    g_fb.depth  = FB_DEFAULT_DEPTH;

    if (g_fb.base == 0 || g_fb.pitch == 0 || g_fb.width == 0 || g_fb.height == 0) {
        g_fb.fail = AARCH64_FB_FAIL_BAD_REPLY;
        return -1;
    }
    return 0;
}

/* ---- 描く ----------------------------------------------------------------
 *
 * **MMU の前後で番地の見え方が変わる。** 前は物理そのもの、後は上位 VA。
 * 触るたびに今の走り方で変換する (pmm の管理情報と同じ理屈) */
static volatile uint32_t* fb_ptr(void) {
    /* **MMU が入っていれば専用の写像を使う。** 上位 VA で走っているかでは
     * ない — TTBR1 は MMU を入れた時点で効いており、恒等マッピングには
     * フレームバッファが無い (vm.h の aarch64_vm_mmu_enabled) */
    if (aarch64_vm_mmu_enabled()) return (volatile uint32_t*)(uintptr_t)AARCH64_FB_VA_BASE;
    return (volatile uint32_t*)(uintptr_t)g_fb.base;
}

void aarch64_fb_fill(uint32_t argb) {
    volatile uint32_t* p;
    uint32_t x, y, stride;
    if (g_fb.base == 0) return;
    p = fb_ptr();
    stride = g_fb.pitch / 4U;
    for (y = 0; y < g_fb.height; y++) {
        for (x = 0; x < g_fb.width; x++) p[y * stride + x] = argb;
    }
}

/* 起動時のテストパターン。
 *
 * **単色で塗らない。** 塗りつぶしだけだと「番地が合っている」ことしか
 * 分からず、ピッチが違っていても気づけない。斜めの縞と四隅の印を出すと、
 * ピッチのずれは縞の傾きに、色順の違いは色に、そのまま出る */
void aarch64_fb_test_pattern(void) {
    volatile uint32_t* p;
    uint32_t x, y, stride;
    if (g_fb.base == 0) return;
    p = fb_ptr();
    stride = g_fb.pitch / 4U;

    for (y = 0; y < g_fb.height; y++) {
        for (x = 0; x < g_fb.width; x++) {
            /* 赤 = 横、緑 = 縦、青 = 斜め。**ピッチが違うと縞が傾く** */
            uint32_t r = (x * 255U) / g_fb.width;
            uint32_t g = (y * 255U) / g_fb.height;
            uint32_t bl = ((x + y) & 0x3fU) < 0x20U ? 0xffU : 0x00U;
            p[y * stride + x] = (r << 16) | (g << 8) | bl;
        }
    }

    /* 四隅に白い升目。**画面の端まで届いているかを見る** */
    for (y = 0; y < 16U && y < g_fb.height; y++) {
        for (x = 0; x < 16U && x < g_fb.width; x++) {
            p[y * stride + x] = 0xffffffU;
            p[y * stride + (g_fb.width - 1U - x)] = 0xffffffU;
            p[(g_fb.height - 1U - y) * stride + x] = 0xffffffU;
            p[(g_fb.height - 1U - y) * stride + (g_fb.width - 1U - x)] = 0xffffffU;
        }
    }
}

/* 画面の上端に帯を引く。
 *
 * **MMU を入れた後に上位 VA から書けることの確認用。**テストパターンは
 * MMU の前に物理番地で描いているので、それだけでは写像の証拠にならない */
void aarch64_fb_mark_top(uint32_t argb) {
    volatile uint32_t* p;
    uint32_t x, y, stride, rows;
    if (g_fb.base == 0) return;
    p = fb_ptr();
    stride = g_fb.pitch / 4U;
    rows = g_fb.height < 8U ? g_fb.height : 8U;
    for (y = 0; y < rows; y++) {
        for (x = 0; x < g_fb.width; x++) p[y * stride + x] = argb;
    }
}
