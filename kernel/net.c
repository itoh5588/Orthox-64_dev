#include <stdint.h>
#include "net.h"
#include "virtio_net.h"
#include "lwip_port.h"
#include "fs.h"          /* resolv.conf と O_* は fs.h にある (2026-09-05) */
#include "arch_time.h"   /* arch_time_now_ms。3 アーキに振り分ける */

/* DHCP を待つ上限。実測では 1 秒ほどで返る (QEMU / 実機とも)。
 * **線が抜けていても起動を止めない**ための上限 */
#define NET_RESOLV_WAIT_MS  5000ULL
#define NET_RESOLV_PATH     "/etc/resolv.conf"

/* ---- 時計合わせ (2026-09-05、TLS の手2) ----------------------------------
 *
 * **接続先は /etc/ntp.conf で変えられる。**書式は 1 行だけ:
 *
 *     server ntp.example.jp
 *
 * ファイルが無ければ下の既定に出る。**「外に出ない」を選びたいときは
 * 空のファイルを置く** (server 行が無ければ何もしない)。 */
#define NET_NTP_PATH        "/etc/ntp.conf"
#define NET_NTP_TIMEOUT_MS  3000U

/* **既定の時刻サーバは複数持つ (2026-09-05、T-7)。**
 *
 * `pool.ntp.org` は A レコードを 4 つ返すが、`lwip_port_lookup_ipv4()` が
 * 渡してくるのは**そのうち 1 つだけ**。以前はそれが無応答だとそれきり
 * 諦めていた。**実機で 4 分の 1 の確率で時計が合わなかった** ——
 * ホストから 4 つ全部に投げて測ったところ、ちょうど 1 つ
 * (172.233.91.137) が 3 秒待っても返さなかった。
 *
 * **名前を変えて引き直せば別のホストに当たる。**運営者が別なので
 * 同時に落ちている見込みは薄い。
 *
 * 時計が合わないと **有効期間の残りが短い証明書を「期限切れ」と誤って
 * 弾く** ので、TLS にとっては実害がある (合わなかった実機の時計は
 * 43 日先だった) */
static const char* const g_ntp_defaults[] = {
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com",
};

uint32_t lwip_port_dns_server_ipv4(void);
int      lwip_port_sntp_sync(uint32_t server_ipv4, uint32_t timeout_ms);
int      lwip_port_lookup_ipv4(const char* hostname, uint32_t* out_addr);

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

/* ---- /etc/resolv.conf を書く (2026-09-05、TLS の手3) ----------------------
 *
 * **名前解決は libc の仕事にする。**Linux では DHCP クライアントがこの
 * ファイルを書き、libc がそれを読んで自分で UDP を投げる。Orthox の
 * カーネルにも lwIP の DNS クライアントは有るが、**Linux ABI には名前解決の
 * syscall が無い** —— x86 だけが独自の ORTH_SYS_DNS_LOOKUP を持っており、
 * aarch64/riscv64 の musl からは呼べなかった。同じ形に揃える。
 *
 * **DHCP を待つのは上限つき。**ネットワークの無い機械 (や線の抜けた機械)
 * で起動を止めない。待っている間にタイマ割り込みが net_poll を回すので、
 * ここは時計を見るだけでよい。
 *
 * **中身が同じなら書かない。**起動のたびに SD へ書きに行かせない。
 *
 * 戻り値: 書いたら 1、書く必要が無かった / 書けなかったら 0 */
int net_write_resolv_conf(void) {
    uint32_t dns;
    uint64_t deadline;
    char line[64];
    char cur[64];
    unsigned n = 0;
    int fd;

    /* ネットワークそのものが無ければ即座に諦める (待たない) */
    if (!net_is_ready()) return 0;

    deadline = arch_time_now_ms() + NET_RESOLV_WAIT_MS;
    for (;;) {
        dns = lwip_port_dns_server_ipv4();
        if (dns != 0) break;
        if (arch_time_now_ms() > deadline) return 0;   /* DHCP が返らなかった */
    }

    /* "nameserver a.b.c.d
" を組む。番地はネットワークバイト順 */
    {
        static const char pre[] = "nameserver ";
        for (const char* q = pre; *q; q++) line[n++] = *q;
        for (int i = 0; i < 4; i++) {
            unsigned v = (dns >> (i * 8)) & 0xffU;
            if (v >= 100U) line[n++] = (char)('0' + v / 100U);
            if (v >= 10U)  line[n++] = (char)('0' + (v / 10U) % 10U);
            line[n++] = (char)('0' + v % 10U);
            line[n++] = (i == 3) ? '\n' : '.';
        }
        line[n] = '\0';
    }

    /* 既に同じ中身なら触らない */
    fd = fs_open(NET_RESOLV_PATH, O_RDONLY, 0);
    if (fd >= 0) {
        int64_t got = fs_read(fd, cur, sizeof(cur) - 1);
        fs_close(fd);
        if (got == (int64_t)n) {
            int same = 1;
            for (unsigned i = 0; i < n; i++) if (cur[i] != line[i]) { same = 0; break; }
            if (same) return 0;
        }
    }

    fd = fs_open(NET_RESOLV_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    {
        int64_t wrote = fs_write(fd, line, n);
        fs_close(fd);
        if (wrote != (int64_t)n) return 0;
    }
    return 1;
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

/* 時刻サーバを 1 つ試す。**どの段で落ちたかを戻り値で区別する** ——
 * 以前は「合った / 合わない」の 2 値しか無く、名前を引けなかったのか
 * 応答が無かったのかが**ログから分からなかった** */
static int net_try_one_ntp(const char* host) {
    uint32_t ip = 0;
    if (lwip_port_lookup_ipv4(host, &ip) != 0 || ip == 0) return NET_CLOCK_ERR_DNS;
    if (!lwip_port_sntp_sync(ip, NET_NTP_TIMEOUT_MS)) return NET_CLOCK_ERR_NOREPLY;
    return NET_CLOCK_SYNCED;
}

/* ---- 壁時計を合わせる (2026-09-05、TLS の手2) ------------------------------
 *
 * **CLOCK_REALTIME が実時刻とずれていると TLS が証明書を弾く。**
 * 2026-09-05 の実測で実機は +39 日進んでおり、起動ごとに +1 日離れていた
 * (xv6fs の通し番号を秒として返していたため)。Pi 4 に RTC は無く、ルータも
 * NTP を返さないので、外部の時刻サーバに出る。
 *
 * **出す先は /etc/ntp.conf で決める。**空のファイルを置けば外に出ない。
 * 書いていなければ g_ntp_defaults を順に試す。
 * **合わなくても起動は止めない。**
 *
 * 戻り値: include/net.h の NET_CLOCK_*。**真偽値ではない** ——
 * 失敗は負で返るので `if (net_sync_wallclock())` と書くと「合った」に見える */
int net_sync_wallclock(void) {
    char host[128];
    unsigned hn = 0;
    int fd;

    if (!net_is_ready()) return NET_CLOCK_SKIPPED;

    /* /etc/ntp.conf の "server <名前>" を読む。無ければ既定 */
    fd = fs_open(NET_NTP_PATH, O_RDONLY, 0);
    if (fd >= 0) {
        char cfg[256];
        int64_t got = fs_read(fd, cfg, sizeof(cfg) - 1);
        fs_close(fd);
        if (got > 0) {
            cfg[got] = 0;
            /* 行ごとに見て、最初の "server " を採る */
            for (int64_t i = 0; i < got; ) {
                int64_t bol = i;
                while (i < got && cfg[i] != '\n') i++;
                {
                    int64_t len = i - bol;
                    const char* kw = "server ";
                    int match = (len > 7);
                    for (int k = 0; match && k < 7; k++) {
                        if (cfg[bol + k] != kw[k]) match = 0;
                    }
                    if (match) {
                        int64_t j = bol + 7;
                        while (j < bol + len && (cfg[j] == ' ' || cfg[j] == '\t')) j++;
                        while (j < bol + len && hn < sizeof(host) - 1 &&
                               cfg[j] != ' ' && cfg[j] != '\t' && cfg[j] != '\r') {
                            host[hn++] = cfg[j++];
                        }
                        break;
                    }
                }
                if (i < got) i++;
            }
        }
        host[hn] = 0;
        /* **ファイルは有るが server 行が無い = 「外に出るな」という意思表示。**
         * 既定に落とさない */
        if (hn == 0) return NET_CLOCK_SKIPPED;
    } else {
        /* **設定が無ければ既定を順に試す。**1 つが無応答でも次へ行く */
        int last = NET_CLOCK_ERR_DNS;
        unsigned i;
        for (i = 0; i < sizeof(g_ntp_defaults) / sizeof(g_ntp_defaults[0]); i++) {
            int r = net_try_one_ntp(g_ntp_defaults[i]);
            if (r == NET_CLOCK_SYNCED) return NET_CLOCK_SYNCED;
            last = r;
        }
        return last;
    }

    /* **利用者が名前を書いたなら、それだけを使う。**書いていない相手へ
     * 勝手に出ていくのは意思に反する */
    return net_try_one_ntp(host);
}
