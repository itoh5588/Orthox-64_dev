#include <stdint.h>
#include "lwip_port.h"
#include "net.h"
#include "arch_time.h"   /* arch_time_now_ms。3 アーキに振り分ける */
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
#include "lwip/tcp.h"
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

/* TCP の疎通確認 (N-9 の続き、2026-09-04)。0=まだ 1=進行中 2=終わった */
static struct tcp_pcb* g_tcp_probe = 0;
static int g_tcp_probe_state = 0;
static uint64_t g_last_arp_probe_ms = 0;
static uint64_t g_last_ping_ms = 0;
static uint64_t g_rx_frames = 0;
static uint16_t g_ping_seq = 0;
static uint16_t g_ping_sent = 0;
static uint16_t g_ping_recv = 0;
#define PING_REPLIES_WANTED 3
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

/* **既定では黙る (2026-09-05)。**フレーム 1 枚につき 1 行出るので、実機で
 * ash を使っていると出力が流れ続け、**シリアルから送ったコマンドを
 * 取りこぼす** —— 実際に `/usr/bin/gpr` まで届いたところで切れて、
 * コマンドが実行されなかった。
 *
 * **ping reply と tcp probe はそのまま残す。**tests/aarch64_net_smoke.sh が
 * 判定に使っている。ping は応答が 3 回返ったところで止まる
 * (lwip_port_poll) ので、出続けはしない。
 *
 * 見たいときは AARCH64_VERBOSE_DIAG=1 で組む (60 秒ごとの計器と同じ口) */
static void orthox_lwip_log_rx_type(uint16_t etype) {
#ifdef AARCH64_VERBOSE_DIAG
    puts("[lwip] rx frame type=0x");
    puthex(etype);
    puts(" count=0x");
    puthex(g_rx_frames);
    puts("\r\n");
#else
    (void)etype;
#endif
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

/* ---- TCP の疎通確認 -------------------------------------------------------
 *
 * **ICMP (ping) と UDP (DHCP/DNS) が通っても TCP が通るとは限らない。**
 * TCP には状態遷移・再送・ウィンドウがあり、送信の完了通知 (tcp_sent) や
 * 受信の返却 (tcp_recved) を正しく回せて初めて成立する。GENET を実機で
 * 動かしたところなので (2026-09-04)、そこまで通ることを 1 度だけ確かめる。
 *
 * 接続先はゲートウェイの 80 番。**外には出ない** —— ルータの管理画面は
 * たいてい開いており、DHCP で得た既知の相手なので余計な探索も要らない。
 * 開いていなければ tcp_err が呼ばれて「refused」と出るだけで、その場合も
 * 「SYN を出して RST を受け取れた」= TCP の往復は動いた証拠になる。 */
static void orthox_lwip_tcp_probe_done(const char* how) {
    puts("[lwip] tcp probe: ");
    puts(how);
    puts("\r\n");
    g_tcp_probe_state = 2;
}

static err_t orthox_lwip_tcp_probe_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) {
    (void)arg;
    if (err != ERR_OK) {
        orthox_lwip_tcp_probe_done("recv error");
        return err;
    }
    if (!p) {
        /* 相手が閉じた (FIN)。ここまで来れば往復は完全に成立している */
        g_tcp_probe = 0;
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_close(pcb);
        orthox_lwip_tcp_probe_done("closed by peer (ok)");
        return ERR_OK;
    }

    {
        /* 先頭を出す。HTTP なら "HTTP/1.0 200 OK" のような行が見える。
         * **印字できない文字は . にする** —— 相手が何を返すか分からない */
        char head[33];
        uint16_t n = p->tot_len < 32U ? p->tot_len : 32U;
        uint16_t i;
        (void)pbuf_copy_partial(p, head, n, 0);
        for (i = 0; i < n; i++) {
            char c = head[i];
            if (c < 0x20 || c > 0x7e) head[i] = '.';
        }
        head[n] = '\0';
        puts("[lwip] tcp probe: recv ");
        putdec_u16(p->tot_len);
        puts(" bytes head=\"");
        puts(head);
        puts("\"\r\n");
    }

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void orthox_lwip_tcp_probe_err(void* arg, err_t err) {
    (void)arg;
    /* err が来た時点で pcb は lwIP 側で解放済み。掴んだままにしない */
    g_tcp_probe = 0;
    if (err == ERR_RST) {
        /* SYN に RST が返った = 相手まで届いて返事も来た。TCP は動いている */
        orthox_lwip_tcp_probe_done("connection refused (RST) - the round trip works");
    } else if (err == ERR_ABRT) {
        orthox_lwip_tcp_probe_done("aborted");
    } else {
        orthox_lwip_tcp_probe_done("error");
    }
}

static err_t orthox_lwip_tcp_probe_connected(void* arg, struct tcp_pcb* pcb, err_t err) {
    /* **Host ヘッダは付けない。** HTTP/1.0 では任意で、しかも相手の
     * ルータは Host を検証していて不正な値だと**無言で閉じる**
     * (DNS リバインディング対策。curl -H "Host: gw" でも同じ挙動を
     * 再現した。2026-09-04)。ここで詰まると TCP 側の不具合に見える */
    static const char req[] = "GET / HTTP/1.0\r\n\r\n";
    err_t werr;
    (void)arg;

    if (err != ERR_OK) {
        orthox_lwip_tcp_probe_done("connect failed");
        return err;
    }
    puts("[lwip] tcp probe: connected to gw:80\r\n");

    werr = tcp_write(pcb, req, (u16_t)(sizeof(req) - 1U), TCP_WRITE_FLAG_COPY);
    if (werr != ERR_OK) {
        orthox_lwip_tcp_probe_done("tcp_write failed");
        return werr;
    }
    tcp_output(pcb);
    return ERR_OK;
}

static void orthox_lwip_tcp_probe_start(void) {
    g_tcp_probe_state = 1;
    g_tcp_probe = tcp_new();
    if (!g_tcp_probe) {
        orthox_lwip_tcp_probe_done("tcp_new failed");
        return;
    }
    tcp_arg(g_tcp_probe, NULL);
    tcp_recv(g_tcp_probe, orthox_lwip_tcp_probe_recv);
    tcp_err(g_tcp_probe, orthox_lwip_tcp_probe_err);

    puts("[lwip] tcp probe: connecting to gw:80\r\n");
    if (tcp_connect(g_tcp_probe, &g_ping_target, 80, orthox_lwip_tcp_probe_connected) != ERR_OK) {
        tcp_abort(g_tcp_probe);
        g_tcp_probe = 0;
        orthox_lwip_tcp_probe_done("tcp_connect failed");
    }
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

/* **どの問い合わせに対する答えかを世代番号で照合する** (2026-09-05)。
 * 時間切れで諦めた問い合わせの答えが後から届いても、次の問い合わせの
 * 結果として拾わないため。番号は arg に載せて lwIP に預ける */
static uint32_t g_dns_gen;

static void orthox_lwip_dns_found(const char* name, const ip_addr_t* ipaddr, void* arg) {
    (void)name;
    /* 諦めた古い問い合わせの答え。捨てる */
    if ((uint32_t)(uintptr_t)arg != g_dns_gen) return;
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

/* **アーキ依存の生 asm をやめ、共有層 (include/spinlock.h) の
 * irq_save_disable/irq_restore に乗せた。**x86_64 は kernel/spinlock.c、
 * riscv64/aarch64 はそれぞれの runtime.c が実装しており、意味は元の
 * pushfq/cli + 条件付き sti と同じ (2026-09-04、N-9 手1 で aarch64 に
 * 繋ぐために切り出した)。 */
sys_prot_t sys_arch_protect(void) {
    return (sys_prot_t)irq_save_disable();
}

void sys_arch_unprotect(sys_prot_t pval) {
    irq_restore((uint64_t)pval);
}

uint32_t sys_now(void) {
    return (uint32_t)arch_time_now_ms();
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
    uint64_t now = arch_time_now_ms();
    if (g_dhcp_ready && !g_gateway_seen && now - g_last_arp_probe_ms >= 1000) {
        g_last_arp_probe_ms = now;
        puts("[lwip] arp probe gw\r\n");
        (void)etharp_request(&g_netif, netif_ip4_gw(&g_netif));
    }
    /* **応答が PING_REPLIES_WANTED 回返ったら ping をやめる。**疎通の確認は
     * 起動時に済めば足り、3 秒ごとに 2 行出し続けると ash のプロンプトの
     * 直後に割り込んで、シリアル越しの判定がプロンプトを見逃す
     * (日報2026-09-18)。tests/aarch64_net_smoke.sh の判定は「3 回以上」 */
    if (g_gateway_seen && g_ping_recv < PING_REPLIES_WANTED &&
        now - g_last_ping_ms >= 3000) {
        g_last_ping_ms = now;
        orthox_lwip_ping_gateway();
    }
    /* ゲートウェイの MAC が分かってから 1 度だけ TCP を試す */
    if (g_gateway_seen && g_tcp_probe_state == 0) {
        orthox_lwip_tcp_probe_start();
    }
    g_lwip_poll_busy = 0;
}

int lwip_port_is_ready(void) {
    return g_lwip_ready;
}

/* ==========================================================================
 * SNTP —— 壁時計を合わせる (2026-09-05、TLS の手2)
 *
 * **なぜ要るか。**CLOCK_REALTIME はこれまで xv6fs_now_sec() を返していた。
 * あれは「単調に増える通し番号を秒の形で持っているだけで、実際の日時とは
 * 対応しない」と xv6fs.c 自身が書いているとおりで、**マウントのたびに
 * 86400 秒 (1 日) 進む**。2026-09-05 の実測で実機は実時刻より +39 日進んで
 * おり、起動ごとに +1 日ずつ離れていた。
 *
 * TLS は証明書の有効期限をこの時計で見る。有効期間は 90 日程度 (Let's
 * Encrypt) なので、**このまま 50 回ほど起動すると、正しい証明書が
 * 全部「期限切れ」で弾かれる**。実害に届く。
 *
 * Raspberry Pi 4 には RTC が無く、ルータも NTP を返さない (2026-09-05 に
 * 192.168.11.1 と QEMU の 10.0.2.2 の両方へ問い合わせて実測、どちらも
 * 無応答)。**外部の時刻サーバに出るしかない。**
 *
 * 出す先は /etc/ntp.conf で変えられる (kernel/net.c)。**合わなくても
 * 起動は止めない** —— 時計が無いより起動しないほうが困る。
 * ========================================================================== */
#define SNTP_PORT              123
#define SNTP_PKT_LEN           48
/* 1900-01-01 から 1970-01-01 までの秒数。NTP は 1900 起点で数える */
#define SNTP_EPOCH_OFFSET      2208988800UL
/* 送信タイムスタンプの位置 (RFC 4330 の Transmit Timestamp) */
#define SNTP_XMIT_OFFSET       40

static struct udp_pcb* g_sntp_pcb;
static volatile int    g_sntp_done;
static uint32_t        g_sntp_unix_sec;   /* 受け取った時刻 (Unix 秒) */
static uint64_t        g_sntp_at_ms;      /* それを受け取った時点の起動経過 ms */

static void orthox_lwip_sntp_recv(void* arg, struct udp_pcb* pcb, struct pbuf* p,
                                  const ip_addr_t* addr, u16_t port) {
    uint8_t buf[SNTP_PKT_LEN];
    uint32_t ntp_sec;
    (void)arg; (void)pcb; (void)addr; (void)port;

    if (!p) return;
    if (p->tot_len >= SNTP_PKT_LEN &&
        pbuf_copy_partial(p, buf, SNTP_PKT_LEN, 0) == SNTP_PKT_LEN) {
        ntp_sec = ((uint32_t)buf[SNTP_XMIT_OFFSET + 0] << 24) |
                  ((uint32_t)buf[SNTP_XMIT_OFFSET + 1] << 16) |
                  ((uint32_t)buf[SNTP_XMIT_OFFSET + 2] << 8) |
                  ((uint32_t)buf[SNTP_XMIT_OFFSET + 3]);
        /* **1900 起点より前は受け取らない。**0 や壊れた値を掴んで
         * 時計を 1970 年に戻すと、証明書が全部「まだ有効でない」になる */
        if (ntp_sec > SNTP_EPOCH_OFFSET) {
            g_sntp_unix_sec = ntp_sec - SNTP_EPOCH_OFFSET;
            g_sntp_at_ms = arch_time_now_ms();
            g_sntp_done = 1;
        }
    }
    pbuf_free(p);
}

/* 合わせた壁時計 (Unix 秒)。**まだ合っていなければ 0。**
 * 0 を返したら呼んだ側が xv6fs の通し番号に退く (kernel/linux_syscall.c) */
uint32_t lwip_port_wallclock_sec(void) {
    if (!g_sntp_done) return 0;
    return g_sntp_unix_sec + (uint32_t)((arch_time_now_ms() - g_sntp_at_ms) / 1000ULL);
}

/* server へ SNTP を 1 回投げて答えを待つ。合ったら 1、駄目なら 0。
 * **タスク文脈から呼ぶこと** (kernel_yield で待つ) */
int lwip_port_sntp_sync(uint32_t server_ipv4, uint32_t timeout_ms) {
    struct task* current = get_current_task();
    ip_addr_t dst;
    struct pbuf* p;
    uint64_t deadline;

    if (!g_lwip_ready || !g_dhcp_ready || !server_ipv4) return 0;
    if (g_sntp_done) return 1;

    if (!g_sntp_pcb) {
        g_sntp_pcb = udp_new();
        if (!g_sntp_pcb) return 0;
        if (udp_bind(g_sntp_pcb, IP4_ADDR_ANY, 0) != ERR_OK) {
            udp_remove(g_sntp_pcb);
            g_sntp_pcb = 0;
            return 0;
        }
        udp_recv(g_sntp_pcb, orthox_lwip_sntp_recv, NULL);
    }

    p = pbuf_alloc(PBUF_TRANSPORT, SNTP_PKT_LEN, PBUF_RAM);
    if (!p) return 0;
    {
        uint8_t* d = (uint8_t*)p->payload;
        for (int i = 0; i < SNTP_PKT_LEN; i++) d[i] = 0;
        /* LI=0 (警告なし) / VN=4 / Mode=3 (client) */
        d[0] = 0x23;
    }
    ip_addr_set_ip4_u32(&dst, server_ipv4);
    if (udp_sendto(g_sntp_pcb, p, &dst, SNTP_PORT) != ERR_OK) {
        pbuf_free(p);
        return 0;
    }
    pbuf_free(p);

    deadline = arch_time_now_ms() + (uint64_t)timeout_ms;
    while (!g_sntp_done) {
        if (arch_time_now_ms() > deadline) return 0;
        if (current) kernel_yield();
    }
    return 1;
}

/* DHCP が配ってきた DNS サーバ (ネットワークバイト順)。まだなら 0。
 *
 * **`/etc/resolv.conf` を書くのに使う** (2026-09-05、TLS の手3)。
 * lwIP の DNS クライアントはカーネル内にあるが、**Linux ABI には
 * 名前解決の syscall が無い** —— Linux では libc が resolv.conf を読んで
 * 自分で UDP を投げる。その形に揃えるため、番地だけを外に出す */
uint32_t lwip_port_dns_server_ipv4(void) {
    const ip_addr_t* d;
    if (!g_lwip_ready || !g_dhcp_ready) return 0;
    d = dns_getserver(0);
    if (!d || !IP_IS_V4(d)) return 0;
    return ip4_addr_get_u32(ip_2_ip4(d));
}

/* 名前を引く上限 (ms)。**これが無かった。**
 * 答えが来なければ while ループが永久に回り、しかも g_dns_pending が
 * 残るので**以後の問い合わせが全部塞がる**という形だった。呼ぶのが
 * ユーザープロセスのうちは「そのプロセスが固まる」で済んでいたが、
 * 2026-09-05 に SNTP を起動経路へ入れたことで**起動そのものが
 * 止まりうる**ようになったので、ここで上限を付けた */
#define ORTHOX_DNS_TIMEOUT_MS 5000ULL

int lwip_port_lookup_ipv4(const char* hostname, uint32_t* out_addr) {
    struct task* current = get_current_task();
    uint64_t deadline;
    uint32_t gen;

    if (!g_lwip_ready || !g_dhcp_ready || !hostname || !out_addr) return -1;
    if (g_dns_pending) return -1;

    gen = ++g_dns_gen;
    g_dns_pending = 1;
    g_dns_done = 0;
    g_dns_result = ERR_OK;
    ip_addr_set_zero(&g_dns_addr);

    err_t err = dns_gethostbyname(hostname, &g_dns_addr, orthox_lwip_dns_found,
                                  (void*)(uintptr_t)gen);
    if (err == ERR_OK) {
        g_dns_pending = 0;
        g_dns_done = 1;
    } else if (err != ERR_INPROGRESS) {
        g_dns_pending = 0;
        return -1;
    }

    deadline = arch_time_now_ms() + ORTHOX_DNS_TIMEOUT_MS;
    while (!g_dns_done) {
        if (arch_time_now_ms() > deadline) {
            /* **諦める。**世代番号を進めてあるので、後から答えが来ても
             * 次の問い合わせの結果と混ざらない */
            if (current) orthox_lwip_clear_dns_waiter(current);
            g_dns_pending = 0;
            return -1;
        }
        if (current) {
            /* **時限つきで寝る。**task_mark_sleeping だと、答えが来ない
             * 限り誰も起こさないので上の時間切れ判定に戻れない
             * (2026-09-05)。100ms ごとに起きて期限を見る */
            task_mark_io_wait_until(current, arch_time_now_ms() + 100ULL);
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
