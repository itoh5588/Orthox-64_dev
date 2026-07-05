#include <stdint.h>
#include "lwip_port.h"
#include "lapic.h"
#include "net.h"
#include "lwip/init.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/ip4_addr.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/raw.h"
#include "lwip/udp.h"
#include "lwip/def.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ethernet.h"
#include "netif/ethernet.h"
#include "task.h"
#include "spinlock.h"

void puts(const char* s);
void puthex(uint64_t v);

static struct netif g_netif;
static int g_lwip_ready = 0;
static int g_dhcp_ready = 0;
static int g_lwip_rx_busy = 0;
static int g_lwip_poll_busy = 0;
static int g_gateway_seen = 0;
static uint64_t g_last_arp_probe_ms = 0;
static uint64_t g_last_ping_ms = 0;
static uint64_t g_rx_frames = 0;
static uint16_t g_ping_seq = 0;
static uint16_t g_ping_sent = 0;
static uint16_t g_ping_recv = 0;
static uint16_t g_ping_id = 0x4F58;
static struct raw_pcb* g_icmp_pcb = 0;
static struct udp_pcb* g_udp_echo_pcb = 0;
static struct dhcp g_dhcp;
static ip_addr_t g_ping_target;
static volatile int g_dns_pending = 0;
static volatile int g_dns_done = 0;
static volatile err_t g_dns_result = ERR_OK;
static ip_addr_t g_dns_addr;
static struct task* g_dns_waiter = 0;

static err_t orthox_lwip_output(struct netif* netif, struct pbuf* p);
static err_t orthox_lwip_init_netif(struct netif* netif);
static void orthox_lwip_rx(const uint8_t* frame, uint16_t len);
static void orthox_lwip_log_rx_type(uint16_t etype);
static void orthox_lwip_netif_status(struct netif* netif);
static u8_t orthox_lwip_icmp_recv(void* arg, struct raw_pcb* pcb, struct pbuf* p, const ip_addr_t* addr);
static void orthox_lwip_ping_gateway(void);
static void orthox_lwip_udp_echo_recv(void* arg, struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* addr, u16_t port);
static void orthox_lwip_dns_found(const char* name, const ip_addr_t* ipaddr, void* arg);

static void orthox_lwip_wake_dns_waiter(void) {
    if (!g_dns_waiter) return;
    if (g_dns_waiter->state == TASK_SLEEPING) {
        task_wake(g_dns_waiter);
    }
    g_dns_waiter = 0;
}

static void orthox_lwip_set_dns_waiter(struct task* task) {
    g_dns_waiter = task;
}

static void orthox_lwip_clear_dns_waiter(struct task* task) {
    if (g_dns_waiter == task) {
        g_dns_waiter = 0;
    }
}

static void putdec_u8(uint8_t v) {
    char buf[4];
    int pos = 0;
    if (v >= 100) {
        buf[pos++] = (char)('0' + (v / 100));
        v %= 100;
        buf[pos++] = (char)('0' + (v / 10));
    } else if (v >= 10) {
        buf[pos++] = (char)('0' + (v / 10));
    }
    buf[pos++] = (char)('0' + (v % 10));
    buf[pos] = '\0';
    puts(buf);
}

static uint16_t be16_to_cpu(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static void orthox_lwip_log_rx_type(uint16_t etype) {
    puts("[lwip] rx frame type=0x");
    puthex(etype);
    puts(" count=0x");
    puthex(g_rx_frames);
    puts("\r\n");
}

static void putdec_u16(uint16_t v) {
    char buf[6];
    int pos = 0;
    if (v >= 10000) {
        buf[pos++] = (char)('0' + (v / 10000));
        v %= 10000;
        buf[pos++] = (char)('0' + (v / 1000));
        v %= 1000;
        buf[pos++] = (char)('0' + (v / 100));
        v %= 100;
        buf[pos++] = (char)('0' + (v / 10));
    } else if (v >= 1000) {
        buf[pos++] = (char)('0' + (v / 1000));
        v %= 1000;
        buf[pos++] = (char)('0' + (v / 100));
        v %= 100;
        buf[pos++] = (char)('0' + (v / 10));
    } else if (v >= 100) {
        buf[pos++] = (char)('0' + (v / 100));
        v %= 100;
        buf[pos++] = (char)('0' + (v / 10));
    } else if (v >= 10) {
        buf[pos++] = (char)('0' + (v / 10));
    }
    buf[pos++] = (char)('0' + (v % 10));
    buf[pos] = '\0';
    puts(buf);
}

static void orthox_lwip_put_ip4(const ip4_addr_t* addr) {
    const uint8_t* p = (const uint8_t*)&addr->addr;
    putdec_u8(p[0]); puts(".");
    putdec_u8(p[1]); puts(".");
    putdec_u8(p[2]); puts(".");
    putdec_u8(p[3]);
}

static void orthox_lwip_log_ping_reply(uint16_t seq) {
    puts("[lwip] ping reply seq=");
    putdec_u16(seq);
    puts(" sent=");
    putdec_u16(g_ping_sent);
    puts(" recv=");
    putdec_u16(g_ping_recv);
    puts("\r\n");
}

static void orthox_lwip_log_gateway_ready(void) {
    puts("[lwip] gateway arp resolved\r\n");
}

static void orthox_lwip_log_udp_echo(uint16_t len, uint16_t port) {
    puts("[lwip] udp echo len=");
    putdec_u16(len);
    puts(" port=");
    putdec_u16(port);
    puts("\r\n");
}

static void orthox_lwip_log_dns_server(const ip_addr_t* addr) {
    if (!addr || !IP_IS_V4(addr)) return;
    puts("[lwip] dns=");
    orthox_lwip_put_ip4(ip_2_ip4(addr));
    puts("\r\n");
}

static void orthox_lwip_netif_status(struct netif* netif) {
    if (!netif) return;
    if (ip4_addr_isany_val(*netif_ip4_addr(netif))) return;

    ip_addr_copy_from_ip4(g_ping_target, *netif_ip4_gw(netif));
    g_gateway_seen = 0;
    g_dhcp_ready = 1;

    puts("[lwip] dhcp bound ip=");
    orthox_lwip_put_ip4(netif_ip4_addr(netif));
    puts(" gw=");
    orthox_lwip_put_ip4(netif_ip4_gw(netif));
    puts(" mask=");
    orthox_lwip_put_ip4(netif_ip4_netmask(netif));
    puts("\r\n");
    orthox_lwip_log_dns_server(dns_getserver(0));
}

static u8_t orthox_lwip_icmp_recv(void* arg, struct raw_pcb* pcb, struct pbuf* p, const ip_addr_t* addr) {
    (void)arg;
    (void)pcb;
    (void)addr;
    if (!p || p->len < sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr)) return 0;

    const struct ip_hdr* iph = (const struct ip_hdr*)p->payload;
    uint16_t hlen = IPH_HL_BYTES(iph);
    if (p->len < hlen + sizeof(struct icmp_echo_hdr)) return 0;

    const struct icmp_echo_hdr* echo = (const struct icmp_echo_hdr*)((const uint8_t*)p->payload + hlen);
    if (echo->type != ICMP_ER || echo->code != 0) return 0;
    if (lwip_ntohs(echo->id) != g_ping_id) return 0;

    g_ping_recv++;
    orthox_lwip_log_ping_reply(lwip_ntohs(echo->seqno));
    return 0;
}

static void orthox_lwip_ping_gateway(void) {
    struct pbuf* p = pbuf_alloc(PBUF_IP, (uint16_t)(sizeof(struct icmp_echo_hdr) + 8), PBUF_RAM);
    if (!p) {
        puts("[lwip] ping alloc failed\r\n");
        return;
    }

    struct icmp_echo_hdr* echo = (struct icmp_echo_hdr*)p->payload;
    uint8_t* payload = (uint8_t*)(echo + 1);
    for (uint16_t i = 0; i < 8; i++) {
        payload[i] = (uint8_t)('A' + i);
    }

    echo->type = ICMP_ECHO;
    echo->code = 0;
    echo->id = lwip_htons(g_ping_id);
    echo->seqno = lwip_htons(++g_ping_seq);
    echo->chksum = 0;
    echo->chksum = inet_chksum(echo, p->len);

    if (raw_sendto(g_icmp_pcb, p, &g_ping_target) == ERR_OK) {
        g_ping_sent++;
        puts("[lwip] ping gw seq=");
        putdec_u16(g_ping_seq);
        puts("\r\n");
    } else {
        puts("[lwip] ping send failed\r\n");
    }
    pbuf_free(p);
}

static void orthox_lwip_udp_echo_recv(void* arg, struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* addr, u16_t port) {
    (void)arg;
    if (!pcb || !p || !addr) {
        if (p) pbuf_free(p);
        return;
    }

    orthox_lwip_log_udp_echo(p->tot_len, port);
    (void)udp_sendto(pcb, p, addr, port);
    pbuf_free(p);
}

static void orthox_lwip_dns_found(const char* name, const ip_addr_t* ipaddr, void* arg) {
    (void)name;
    (void)arg;
    if (ipaddr) {
        g_dns_addr = *ipaddr;
        g_dns_result = ERR_OK;
    } else {
        g_dns_result = ERR_VAL;
    }
    g_dns_done = 1;
    g_dns_pending = 0;
    orthox_lwip_wake_dns_waiter();
}

static err_t orthox_lwip_output(struct netif* netif, struct pbuf* p) {
    (void)netif;
    if (!p || p->tot_len == 0 || p->tot_len > 1514) return ERR_IF;

    uint8_t frame[1514];
    if (pbuf_copy_partial(p, frame, p->tot_len, 0) != p->tot_len) {
        return ERR_IF;
    }
    return (net_send_frame(frame, (uint16_t)p->tot_len) == 0) ? ERR_OK : ERR_IF;
}

static err_t orthox_lwip_init_netif(struct netif* netif) {
    const uint8_t* mac = net_get_mac();
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->output = etharp_output;
    netif->linkoutput = orthox_lwip_output;
    netif->mtu = 1500;
    netif->hwaddr_len = ETH_HWADDR_LEN;
    for (uint16_t i = 0; i < ETH_HWADDR_LEN; i++) {
        netif->hwaddr[i] = mac[i];
    }
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

static void orthox_lwip_rx(const uint8_t* frame, uint16_t len) {
    if (!g_lwip_ready || !frame || len == 0 || g_lwip_rx_busy) return;
    g_lwip_rx_busy = 1;
    g_rx_frames++;

    if (len >= sizeof(struct eth_hdr)) {
        const struct eth_hdr* hdr = (const struct eth_hdr*)frame;
        uint16_t etype = be16_to_cpu(hdr->type);
        if (etype == ETHTYPE_ARP || etype == ETHTYPE_IP) {
            orthox_lwip_log_rx_type(etype);
        }
        if (etype == ETHTYPE_ARP && g_dhcp_ready && !g_gateway_seen) {
            g_gateway_seen = 1;
            orthox_lwip_log_gateway_ready();
        }
    }

    struct pbuf* p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (p) {
        if (pbuf_take(p, frame, len) == ERR_OK) {
            if (g_netif.input(p, &g_netif) != ERR_OK) {
                pbuf_free(p);
            }
        } else {
            pbuf_free(p);
        }
    }

    g_lwip_rx_busy = 0;
}

void orthox_lwip_diag(const char* msg) {
    if (!msg) return;
    puts("[lwip] ");
    puts(msg);
    puts("\r\n");
}

void orthox_lwip_assert(const char* msg, const char* file, int line) {
    puts("[lwip] ASSERT ");
    if (msg) puts(msg);
    puts(" file=");
    if (file) puts(file);
    puts(" line=0x");
    puthex((uint64_t)line);
    puts("\r\n");
}

sys_prot_t sys_arch_protect(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return (sys_prot_t)flags;
}

void sys_arch_unprotect(sys_prot_t pval) {
    if ((pval & (1UL << 9)) != 0) {
        __asm__ volatile("sti" : : : "memory");
    }
}

uint32_t sys_now(void) {
    return (uint32_t)lapic_get_ticks_ms();
}

void lwip_port_init(void) {
    if (g_lwip_ready || !net_is_ready()) return;

    ip4_addr_t ipaddr;
    ip4_addr_t netmask;
    ip4_addr_t gw;

    lwip_init();

    ip4_addr_set_zero(&ipaddr);
    ip4_addr_set_zero(&netmask);
    ip4_addr_set_zero(&gw);

    net_set_rx_handler(orthox_lwip_rx);
    if (!netif_add(&g_netif, &ipaddr, &netmask, &gw, NULL, orthox_lwip_init_netif, ethernet_input)) {
        puts("[lwip] netif_add failed\r\n");
        return;
    }

    netif_set_default(&g_netif);
    netif_set_status_callback(&g_netif, orthox_lwip_netif_status);
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);
    g_icmp_pcb = raw_new(IP_PROTO_ICMP);
    if (!g_icmp_pcb) {
        puts("[lwip] raw icmp pcb failed\r\n");
        return;
    }
    raw_bind(g_icmp_pcb, IP4_ADDR_ANY);
    raw_recv(g_icmp_pcb, orthox_lwip_icmp_recv, NULL);
    g_udp_echo_pcb = udp_new();
    if (!g_udp_echo_pcb) {
        puts("[lwip] udp echo pcb failed\r\n");
        return;
    }
    if (udp_bind(g_udp_echo_pcb, IP4_ADDR_ANY, 12345) != ERR_OK) {
        puts("[lwip] udp echo bind failed\r\n");
        return;
    }
    udp_recv(g_udp_echo_pcb, orthox_lwip_udp_echo_recv, NULL);
    dhcp_set_struct(&g_netif, &g_dhcp);
    if (dhcp_start(&g_netif) != ERR_OK) {
        puts("[lwip] dhcp start failed\r\n");
        return;
    }
    g_lwip_ready = 1;
    puts("[lwip] dhcp start\r\n");
    puts("[lwip] udp echo listen 12345\r\n");
}

void lwip_port_poll(void) {
    if (!g_lwip_ready || g_lwip_poll_busy) return;
    g_lwip_poll_busy = 1;
    sys_check_timeouts();
    uint64_t now = lapic_get_ticks_ms();
    if (g_dhcp_ready && !g_gateway_seen && now - g_last_arp_probe_ms >= 1000) {
        g_last_arp_probe_ms = now;
        puts("[lwip] arp probe gw\r\n");
        (void)etharp_request(&g_netif, netif_ip4_gw(&g_netif));
    }
    if (g_gateway_seen && now - g_last_ping_ms >= 3000) {
        g_last_ping_ms = now;
        orthox_lwip_ping_gateway();
    }
    g_lwip_poll_busy = 0;
}

int lwip_port_is_ready(void) {
    return g_lwip_ready;
}

int lwip_port_lookup_ipv4(const char* hostname, uint32_t* out_addr) {
    struct task* current = get_current_task();
    if (!g_lwip_ready || !g_dhcp_ready || !hostname || !out_addr) return -1;
    if (g_dns_pending) return -1;

    g_dns_pending = 1;
    g_dns_done = 0;
    g_dns_result = ERR_OK;
    ip_addr_set_zero(&g_dns_addr);

    err_t err = dns_gethostbyname(hostname, &g_dns_addr, orthox_lwip_dns_found, NULL);
    if (err == ERR_OK) {
        g_dns_pending = 0;
        g_dns_done = 1;
    } else if (err != ERR_INPROGRESS) {
        g_dns_pending = 0;
        return -1;
    }

    while (!g_dns_done) {
        if (current) {
            task_mark_sleeping(current);
            orthox_lwip_set_dns_waiter(current);
            if (g_dns_done) {
                orthox_lwip_clear_dns_waiter(current);
                if (current->state == TASK_SLEEPING) {
                    task_wake(current);
                }
                break;
            }
        }
        kernel_yield();
        if (current) {
            orthox_lwip_clear_dns_waiter(current);
        }
    }
    if (g_dns_result != ERR_OK || !IP_IS_V4(&g_dns_addr)) return -1;
    *out_addr = ip_2_ip4(&g_dns_addr)->addr;
    return 0;
}
