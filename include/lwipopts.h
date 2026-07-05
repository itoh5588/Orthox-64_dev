#ifndef ORTHOX_LWIPOPTS_H
#define ORTHOX_LWIPOPTS_H

#define NO_SYS                       1
#define LWIP_SOCKET                  0
#define LWIP_NETCONN                 0
#define LWIP_NETIF_API               0

#define LWIP_IPV4                    1
#define LWIP_IPV6                    0
#define LWIP_ARP                     1
#define LWIP_ETHERNET                1
#define LWIP_ICMP                    1
#define IP_REASSEMBLY                0
#define IP_FRAG                      0
#define LWIP_RAW                     1
#define LWIP_UDP                     1
#define LWIP_TCP                     1
#define LWIP_DHCP                    1
#define LWIP_DHCP_DOES_ACD_CHECK     0
#define LWIP_AUTOIP                  0
#define LWIP_DNS                     1
#define SO_REUSE                     1
#define LWIP_IGMP                    0
#define PPP_SUPPORT                  0
#define LWIP_HAVE_LOOPIF             0
#define LWIP_NETIF_LOOPBACK          0
#define LWIP_NUM_NETIF_CLIENT_DATA   0

#define MEM_ALIGNMENT                8U
#define MEM_SIZE                     (64U * 1024U)
#define MEMP_NUM_PBUF                32
#define MEMP_NUM_ARP_QUEUE           16
#define MEMP_NUM_TCP_SEG             32
#define MEMP_NUM_TCP_PCB             8
#define MEMP_NUM_TCP_PCB_LISTEN      4
#define MEMP_NUM_SYS_TIMEOUT         20
#define PBUF_POOL_SIZE               32
#define PBUF_POOL_BUFSIZE            1700

#define TCP_MSS                      1460
#define TCP_WND                      (4 * TCP_MSS)
#define TCP_SND_BUF                  (4 * TCP_MSS)

#define SYS_LIGHTWEIGHT_PROT         1
#define LWIP_TIMERS                  1
#define LWIP_TIMERS_CUSTOM           0

#define LWIP_STATS                   0
#define LWIP_NETIF_STATUS_CALLBACK   1
#define LWIP_NETIF_LINK_CALLBACK     0
#define LWIP_CHECKSUM_CTRL_PER_NETIF 0

#define ETH_PAD_SIZE                 0
#define LWIP_CHKSUM_ALGORITHM        3
#define LWIP_RAND()                  ((u32_t)0x12345678UL)
#define LWIP_ASSERT_CORE_LOCKED()

#endif
