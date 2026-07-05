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

#endif
