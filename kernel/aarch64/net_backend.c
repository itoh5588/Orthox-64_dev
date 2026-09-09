/*
 * ネットワークの実体選択 (N-9 手2)。
 *
 * aarch64 は 2 つの実装を持つ:
 *   GENET        (kernel/aarch64/genet.c)       実機 (Pi 4) 専用
 *   virtio-net-mmio (kernel/aarch64/virtio_net_mmio.c) QEMU virt 専用
 *
 * どちらも include/virtio_net.h と同じ形の関数を、それぞれ
 * aarch64_genet_* / aarch64_vnetmmio_* という名前で内部に持っている
 * (衝突を避けるため公開名にしていない)。ここが kernel/net.c から見える
 * 唯一の `virtio_net_*` の実体で、DTB に GENET のノードがあれば GENET を、
 * 無ければ virtio-net-mmio を選ぶ。
 *
 * kernel/aarch64/virtio_blk_mmio.c と kernel/aarch64/emmc2.c が
 * 「両方コンパイルして DTB で選ぶ」のと同じ形 (M4-3)。あちらは
 * storage.c の登録 API に複数繋げられるが、ネットワークは
 * net.c が単一の実装しか呼ばないので、ここで 1 つに絞る。
 */
#include <stdint.h>
#include "virtio_net.h"

int  aarch64_genet_probe(void);
int  aarch64_genet_init(void);
void aarch64_genet_poll(void);
int  aarch64_genet_needs_poll_fallback(void);
int  aarch64_genet_is_ready(void);
int  aarch64_genet_send(const void* frame, uint16_t len);
const uint8_t* aarch64_genet_mac(void);
void aarch64_genet_set_rx_callback(virtio_net_rx_cb_t cb);

int  aarch64_vnetmmio_init(void);
void aarch64_vnetmmio_poll(void);
int  aarch64_vnetmmio_needs_poll_fallback(void);
int  aarch64_vnetmmio_is_ready(void);
int  aarch64_vnetmmio_send(const void* frame, uint16_t len);
const uint8_t* aarch64_vnetmmio_mac(void);
void aarch64_vnetmmio_set_rx_callback(virtio_net_rx_cb_t cb);

/* 0=未選択 1=genet 2=vnetmmio 3=無し (どちらも見つからなかった) */
static int g_backend = 0;

int virtio_net_init(void) {
    if (aarch64_genet_probe()) {
        if (aarch64_genet_init() == 0) { g_backend = 1; return 0; }
    }
    if (aarch64_vnetmmio_init() == 0) { g_backend = 2; return 0; }
    g_backend = 3;
    return -1;
}

void virtio_net_poll(void) {
    if (g_backend == 1) aarch64_genet_poll();
    else if (g_backend == 2) aarch64_vnetmmio_poll();
}

int virtio_net_needs_poll_fallback(void) {
    if (g_backend == 1) return aarch64_genet_needs_poll_fallback();
    if (g_backend == 2) return aarch64_vnetmmio_needs_poll_fallback();
    return 0;
}

int virtio_net_is_ready(void) {
    if (g_backend == 1) return aarch64_genet_is_ready();
    if (g_backend == 2) return aarch64_vnetmmio_is_ready();
    return 0;
}

int virtio_net_send(const void* frame, uint16_t len) {
    if (g_backend == 1) return aarch64_genet_send(frame, len);
    if (g_backend == 2) return aarch64_vnetmmio_send(frame, len);
    return -1;
}

const uint8_t* virtio_net_mac(void) {
    static const uint8_t zero[6];
    if (g_backend == 1) return aarch64_genet_mac();
    if (g_backend == 2) return aarch64_vnetmmio_mac();
    return zero;
}

void virtio_net_set_rx_callback(virtio_net_rx_cb_t cb) {
    /* **両方に登録しておく。**init() で使う側が決まる前にこれが呼ばれる
     * (kernel/net.c の net_init: set_rx_callback -> init の順) ので、
     * g_backend が決まるのを待たずに両方へ渡す */
    aarch64_genet_set_rx_callback(cb);
    aarch64_vnetmmio_set_rx_callback(cb);
}
