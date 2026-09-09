#ifndef NET_H
#define NET_H

#include <stdint.h>

typedef void (*net_rx_handler_t)(const uint8_t* frame, uint16_t len);

void net_init(void);
void net_poll(void);
int net_needs_poll_fallback(void);
int net_is_ready(void);
int net_send_frame(const void* frame, uint16_t len);
const uint8_t* net_get_mac(void);
void net_set_rx_handler(net_rx_handler_t handler);
uint64_t net_rx_frame_count(void);

/* DHCP の結果を /etc/resolv.conf に書く。1 = 書いた、0 = 書かなかった */
int net_write_resolv_conf(void);

/* ---- 壁時計を SNTP で合わせる -------------------------------------------
 *
 * **戻り値は真偽値ではない。**`if (net_sync_wallclock())` と書くと
 * **失敗 (-1/-2) まで真になる。**必ず値で分けること */
#define NET_CLOCK_SYNCED        1   /* 合わせた */
#define NET_CLOCK_SKIPPED       0   /* 何もしなかった (網が無い / 設定で無効) */
#define NET_CLOCK_ERR_DNS     (-1)  /* 時刻サーバの名前を引けなかった */
#define NET_CLOCK_ERR_NOREPLY (-2)  /* 引けたが、どのサーバも応答しなかった */
int net_sync_wallclock(void);

#endif
