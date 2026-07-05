#ifndef ORTHOX_LWIP_PORT_H
#define ORTHOX_LWIP_PORT_H

#include <stdint.h>

void lwip_port_init(void);
void lwip_port_poll(void);
int lwip_port_is_ready(void);
int lwip_port_lookup_ipv4(const char* hostname, uint32_t* out_addr);

#endif
