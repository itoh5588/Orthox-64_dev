#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>

typedef void (*virtio_net_rx_cb_t)(const uint8_t* frame, uint16_t len);

int virtio_net_init(void);
void virtio_net_poll(void);
int virtio_net_needs_poll_fallback(void);
int virtio_net_is_ready(void);
int virtio_net_send(const void* frame, uint16_t len);
const uint8_t* virtio_net_mac(void);
void virtio_net_set_rx_callback(virtio_net_rx_cb_t cb);

#endif
