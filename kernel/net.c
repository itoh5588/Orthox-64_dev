#include <stdint.h>
#include "net.h"
#include "virtio_net.h"
#include "lwip_port.h"

static net_rx_handler_t g_rx_handler = 0;
static uint64_t g_rx_frames = 0;

static void net_rx_dispatch(const uint8_t* frame, uint16_t len) {
    g_rx_frames++;
    if (g_rx_handler) {
        g_rx_handler(frame, len);
    }
}

void net_init(void) {
    virtio_net_set_rx_callback(net_rx_dispatch);
    if (virtio_net_init() == 0) {
        lwip_port_init();
    }
}

void net_poll(void) {
    if (virtio_net_needs_poll_fallback()) {
        virtio_net_poll();
    }
    lwip_port_poll();
}

int net_needs_poll_fallback(void) {
    return virtio_net_needs_poll_fallback();
}

int net_is_ready(void) {
    return virtio_net_is_ready();
}

int net_send_frame(const void* frame, uint16_t len) {
    return virtio_net_send(frame, len);
}

const uint8_t* net_get_mac(void) {
    return virtio_net_mac();
}

void net_set_rx_handler(net_rx_handler_t handler) {
    g_rx_handler = handler;
}

uint64_t net_rx_frame_count(void) {
    return g_rx_frames;
}
