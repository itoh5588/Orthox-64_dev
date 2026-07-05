#include <stdint.h>
#include <stddef.h>
#include "net_socket.h"
#include "task.h"
#include "pmm.h"
#include "vmm.h"
#include "spinlock.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip.h"

void puts(const char* s);
void puthex(uint64_t v);

#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_TYPE_MASK 0x0f
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC 0x80000
#define MSG_NOSIGNAL 0x4000
#define INADDR_ANY 0U
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_KEEPALIVE 9
#define ORTH_LINUX_O_NONBLOCK 0x800
#define ORTH_ERR_EAGAIN 11
#define ORTH_ERR_EINVAL 22
#define ORTH_ERR_ECONNRESET 104
#define ORTH_ERR_ECONNREFUSED 111
#define ORTH_ERR_ECONNABORTED 113
#define ORTH_ERR_ETIMEDOUT 116
#define ORTH_ERR_EHOSTUNREACH 118
#define ORTH_ERR_ENOTCONN 128

struct orth_in_addr {
    uint32_t s_addr;
};

struct orth_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct orth_in_addr sin_addr;
    uint8_t sin_zero[8];
};

typedef struct socket_rx_chunk {
    struct socket_rx_chunk* next;
    uint32_t len;
    uint32_t offset;
    uint16_t port;
    uint16_t reserved;
    uint32_t addr;
    uint8_t data[PAGE_SIZE - 24];
} socket_rx_chunk_t;

typedef struct net_socket_backend {
    int ref_count;
    int domain;
    int type;
    int protocol;
    int connected;
    int connecting;
    int listening;
    int eof;
    int error;
    int reuseaddr;
    uint16_t local_port;
    uint16_t peer_port;
    uint32_t local_addr;
    uint32_t peer_addr;
    union {
        struct udp_pcb* udp;
        struct tcp_pcb* tcp;
        void* any;
    } pcb;
    socket_rx_chunk_t* rx_head;
    socket_rx_chunk_t* rx_tail;
    struct task* rx_waiter;
    struct task* tx_waiter;
    struct task* connect_waiter;
    struct task* accept_waiter;
    struct net_socket_backend* accept_head;
    struct net_socket_backend* accept_tail;
    struct net_socket_backend* accept_next;
} net_socket_backend_t;

static void free_accept_queue(net_socket_backend_t* sock);
static void destroy_socket_backend(net_socket_backend_t* sock);

static int socket_errno_from_lwip(err_t err) {
    switch (err) {
        case ERR_OK:
            return 0;
        case ERR_TIMEOUT:
            return ORTH_ERR_ETIMEDOUT;
        case ERR_RTE:
            return ORTH_ERR_EHOSTUNREACH;
        case ERR_ABRT:
            return ORTH_ERR_ECONNABORTED;
        case ERR_RST:
            return ORTH_ERR_ECONNRESET;
        case ERR_CLSD:
        case ERR_CONN:
            return ORTH_ERR_ENOTCONN;
        case ERR_VAL:
        default:
            return ORTH_ERR_EINVAL;
    }
}

static int64_t socket_send_error(err_t err, size_t sent) {
    puts("[sock] send err=0x");
    puthex((uint64_t)(uint32_t)err);
    puts("\r\n");
    return sent ? (int64_t)sent : -(int64_t)socket_errno_from_lwip(err);
}

static void socket_wake_waiter(struct task** waiter) {
    if (!waiter || !*waiter) return;
    if ((*waiter)->state == TASK_SLEEPING) {
        task_wake(*waiter);
    }
    *waiter = 0;
}

static void socket_set_waiter(struct task** waiter, struct task* task) {
    if (!waiter) return;
    *waiter = task;
}

static void socket_clear_waiter(struct task** waiter, struct task* task) {
    if (!waiter) return;
    if (*waiter == task) *waiter = 0;
}

static uint16_t be16_to_cpu(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static uint16_t cpu_to_be16(uint16_t v) {
    return be16_to_cpu(v);
}

static int alloc_fd_slot(struct task* current) {
    if (!current) return -1;
    for (int fd = 3; fd < MAX_FDS; fd++) {
        if (!current->fds[fd].in_use) return fd;
    }
    return -1;
}

static void release_socket_file(struct fs_file* file) {
    net_socket_backend_t* sock;
    if (!file) return;
    sock = (net_socket_backend_t*)file->private_data;
    if (!sock) return;
    sock->ref_count--;
    if (sock->ref_count > 0) return;
    destroy_socket_backend(sock);
}

static const fs_file_ops_t g_socket_file_ops = {
    .release = release_socket_file,
};

static fs_file_t* alloc_socket_file(net_socket_backend_t* sock) {
    void* page;
    fs_file_t* file;
    if (!sock) return 0;
    page = pmm_alloc(1);
    if (!page) return 0;
    file = (fs_file_t*)PHYS_TO_VIRT(page);
    for (size_t i = 0; i < sizeof(*file); i++) ((uint8_t*)file)[i] = 0;
    file->ref_count = 1;
    file->type = FT_SOCKET;
    file->ops = &g_socket_file_ops;
    file->private_data = sock;
    return file;
}

static net_socket_backend_t* socket_backend_from_fd(file_descriptor_t* f) {
    if (!f || !f->in_use || fs_fd_type(f) != FT_SOCKET) return 0;
    return (net_socket_backend_t*)fs_fd_data(f);
}

static net_socket_backend_t* alloc_socket_backend(void) {
    void* page = pmm_alloc(1);
    if (!page) return 0;
    net_socket_backend_t* sock = (net_socket_backend_t*)PHYS_TO_VIRT(page);
    for (size_t i = 0; i < sizeof(*sock); i++) ((uint8_t*)sock)[i] = 0;
    sock->ref_count = 1;
    return sock;
}

static socket_rx_chunk_t* alloc_rx_chunk(void) {
    void* page = pmm_alloc(1);
    if (!page) return 0;
    socket_rx_chunk_t* chunk = (socket_rx_chunk_t*)PHYS_TO_VIRT(page);
    for (size_t i = 0; i < sizeof(*chunk); i++) ((uint8_t*)chunk)[i] = 0;
    return chunk;
}

static void free_rx_chunk(socket_rx_chunk_t* chunk) {
    if (!chunk) return;
    pmm_free((void*)VIRT_TO_PHYS((uint64_t)chunk), 1);
}

static void queue_rx_chunk(net_socket_backend_t* sock, socket_rx_chunk_t* chunk) {
    if (!sock || !chunk) return;
    if (sock->rx_tail) sock->rx_tail->next = chunk;
    else sock->rx_head = chunk;
    sock->rx_tail = chunk;
}

static int queue_pbuf_data(net_socket_backend_t* sock, struct pbuf* p, const ip_addr_t* addr, uint16_t port) {
    uint16_t copied = 0;
    while (copied < p->tot_len) {
        socket_rx_chunk_t* chunk = alloc_rx_chunk();
        if (!chunk) return -1;
        chunk->len = p->tot_len - copied;
        if (chunk->len > sizeof(chunk->data)) chunk->len = sizeof(chunk->data);
        chunk->offset = 0;
        chunk->port = cpu_to_be16(port);
        chunk->addr = (addr && IP_IS_V4(addr)) ? ip_2_ip4(addr)->addr : 0;
        if (chunk->len > 0) {
            (void)pbuf_copy_partial(p, chunk->data, (uint16_t)chunk->len, copied);
        }
        queue_rx_chunk(sock, chunk);
        copied += (uint16_t)chunk->len;
    }
    return 0;
}

static void free_rx_queue(net_socket_backend_t* sock) {
    socket_rx_chunk_t* chunk;
    if (!sock) return;
    chunk = sock->rx_head;
    while (chunk) {
        socket_rx_chunk_t* next = chunk->next;
        free_rx_chunk(chunk);
        chunk = next;
    }
    sock->rx_head = 0;
    sock->rx_tail = 0;
}

static void free_accept_queue(net_socket_backend_t* sock) {
    if (!sock) return;
    while (sock->accept_head) {
        net_socket_backend_t* child = sock->accept_head;
        sock->accept_head = child->accept_next;
        child->accept_next = 0;
        destroy_socket_backend(child);
    }
    sock->accept_tail = 0;
}

static void destroy_socket_backend(net_socket_backend_t* sock) {
    if (!sock) return;
    free_rx_queue(sock);
    free_accept_queue(sock);
    if (sock->type == SOCK_DGRAM && sock->pcb.udp) {
        udp_remove(sock->pcb.udp);
        sock->pcb.udp = 0;
    } else if (sock->type == SOCK_STREAM && sock->pcb.tcp) {
        tcp_arg(sock->pcb.tcp, NULL);
        {
            err_t err = tcp_close(sock->pcb.tcp);
            if (err != ERR_OK) tcp_abort(sock->pcb.tcp);
        }
        sock->pcb.tcp = 0;
    }
    pmm_free((void*)VIRT_TO_PHYS((uint64_t)sock), 1);
}

static void udp_socket_recv(void* arg, struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* addr, uint16_t port) {
    (void)pcb;
    net_socket_backend_t* sock = (net_socket_backend_t*)arg;
    if (!sock || !p || !addr || !IP_IS_V4(addr)) {
        if (p) pbuf_free(p);
        return;
    }
    if (queue_pbuf_data(sock, p, addr, port) < 0) {
        pbuf_free(p);
        return;
    }
    pbuf_free(p);
    socket_wake_waiter(&sock->rx_waiter);
}

static err_t tcp_stream_recv(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err) {
    net_socket_backend_t* sock = (net_socket_backend_t*)arg;
    (void)tpcb;
    if (!sock) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        sock->error = err;
        return err;
    }
    if (!p) {
        sock->eof = 1;
        socket_wake_waiter(&sock->rx_waiter);
        return ERR_OK;
    }
    if (queue_pbuf_data(sock, p, NULL, 0) < 0) {
        pbuf_free(p);
        sock->error = ERR_MEM;
        socket_wake_waiter(&sock->rx_waiter);
        return ERR_MEM;
    }
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    socket_wake_waiter(&sock->rx_waiter);
    return ERR_OK;
}

static void tcp_stream_err(void* arg, err_t err) {
    net_socket_backend_t* sock = (net_socket_backend_t*)arg;
    if (!sock) return;
    sock->pcb.tcp = 0;
    sock->eof = 1;
    sock->error = err;
    socket_wake_waiter(&sock->rx_waiter);
    socket_wake_waiter(&sock->tx_waiter);
    socket_wake_waiter(&sock->connect_waiter);
}

static err_t tcp_stream_sent(void* arg, struct tcp_pcb* tpcb, u16_t len) {
    net_socket_backend_t* sock = (net_socket_backend_t*)arg;
    (void)tpcb;
    (void)len;
    if (sock) socket_wake_waiter(&sock->tx_waiter);
    return ERR_OK;
}

static err_t tcp_client_connected(void* arg, struct tcp_pcb* tpcb, err_t err) {
    net_socket_backend_t* sock = (net_socket_backend_t*)arg;
    if (!sock) return err;
    sock->connecting = 0;
    if (err != ERR_OK) {
        sock->error = err;
        socket_wake_waiter(&sock->connect_waiter);
        return err;
    }
    sock->connected = 1;
    sock->local_port = cpu_to_be16(tpcb->local_port);
    sock->peer_port = cpu_to_be16(tpcb->remote_port);
    sock->local_addr = ip_2_ip4(&tpcb->local_ip)->addr;
    sock->peer_addr = ip_2_ip4(&tpcb->remote_ip)->addr;
    socket_wake_waiter(&sock->connect_waiter);
    return ERR_OK;
}

static err_t tcp_listener_accept(void* arg, struct tcp_pcb* newpcb, err_t err) {
    net_socket_backend_t* listener = (net_socket_backend_t*)arg;
    if (!listener || !newpcb || err != ERR_OK) return ERR_ABRT;

    net_socket_backend_t* child = alloc_socket_backend();
    if (!child) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    child->domain = AF_INET;
    child->type = SOCK_STREAM;
    child->protocol = 0;
    child->connected = 1;
    child->local_port = cpu_to_be16(newpcb->local_port);
    child->peer_port = cpu_to_be16(newpcb->remote_port);
    child->local_addr = ip_2_ip4(&newpcb->local_ip)->addr;
    child->peer_addr = ip_2_ip4(&newpcb->remote_ip)->addr;
    child->pcb.tcp = newpcb;

    tcp_arg(newpcb, child);
    tcp_recv(newpcb, tcp_stream_recv);
    tcp_sent(newpcb, tcp_stream_sent);
    tcp_err(newpcb, tcp_stream_err);

    if (listener->accept_tail) listener->accept_tail->accept_next = child;
    else listener->accept_head = child;
    listener->accept_tail = child;
    socket_wake_waiter(&listener->accept_waiter);
    return ERR_OK;
}

static int fill_sockaddr_in(void* addr, uint32_t* addrlen, uint32_t ip, uint16_t port_be) {
    if (!addr || !addrlen || *addrlen < sizeof(struct orth_sockaddr_in)) return 0;
    struct orth_sockaddr_in* in = (struct orth_sockaddr_in*)addr;
    in->sin_family = AF_INET;
    in->sin_port = port_be;
    in->sin_addr.s_addr = ip;
    for (int i = 0; i < 8; i++) in->sin_zero[i] = 0;
    *addrlen = sizeof(struct orth_sockaddr_in);
    return 1;
}

static int64_t socket_recv_stream(file_descriptor_t* f, net_socket_backend_t* sock, void* buf, size_t len) {
    struct task* current = get_current_task();
    while (!sock->rx_head) {
        if (sock->eof) return 0;
        if (f->flags & ORTH_LINUX_O_NONBLOCK) return -ORTH_ERR_EAGAIN;
        task_mark_sleeping(current);
        socket_set_waiter(&sock->rx_waiter, current);
        if (sock->rx_head || sock->eof) {
            socket_clear_waiter(&sock->rx_waiter, current);
            if (current->state == TASK_SLEEPING) task_wake(current);
            continue;
        }
        kernel_yield();
        socket_clear_waiter(&sock->rx_waiter, current);
    }

    size_t copied = 0;
    while (copied < len && sock->rx_head) {
        socket_rx_chunk_t* chunk = sock->rx_head;
        size_t avail = chunk->len - chunk->offset;
        size_t take = len - copied;
        if (take > avail) take = avail;
        for (size_t i = 0; i < take; i++) {
            ((uint8_t*)buf)[copied + i] = chunk->data[chunk->offset + i];
        }
        chunk->offset += (uint32_t)take;
        copied += take;
        if (chunk->offset >= chunk->len) {
            sock->rx_head = chunk->next;
            if (!sock->rx_head) sock->rx_tail = 0;
            free_rx_chunk(chunk);
        }
        if (take == 0) break;
    }
    return (int64_t)copied;
}

static int64_t socket_recv_dgram(file_descriptor_t* f, net_socket_backend_t* sock, void* buf, size_t len, void* src_addr, uint32_t* addrlen) {
    struct task* current = get_current_task();
    while (!sock->rx_head) {
        if (f->flags & ORTH_LINUX_O_NONBLOCK) return -ORTH_ERR_EAGAIN;
        task_mark_sleeping(current);
        socket_set_waiter(&sock->rx_waiter, current);
        if (sock->rx_head) {
            socket_clear_waiter(&sock->rx_waiter, current);
            if (current->state == TASK_SLEEPING) task_wake(current);
            continue;
        }
        kernel_yield();
        socket_clear_waiter(&sock->rx_waiter, current);
    }

    socket_rx_chunk_t* chunk = sock->rx_head;
    sock->rx_head = chunk->next;
    if (!sock->rx_head) sock->rx_tail = 0;

    size_t copied = len;
    if (copied > chunk->len) copied = chunk->len;
    for (size_t i = 0; i < copied; i++) {
        ((uint8_t*)buf)[i] = chunk->data[i];
    }
    (void)fill_sockaddr_in(src_addr, addrlen, chunk->addr, chunk->port);
    free_rx_chunk(chunk);
    return (int64_t)copied;
}

static int64_t socket_send_stream(file_descriptor_t* f, net_socket_backend_t* sock, const void* buf, size_t len) {
    size_t sent = 0;
    struct task* current = get_current_task();
    if (!sock->pcb.tcp) return -1;
    while (sent < len) {
        uint16_t wnd = tcp_sndbuf(sock->pcb.tcp);
        if (wnd == 0) {
            if (f->flags & ORTH_LINUX_O_NONBLOCK) return sent ? (int64_t)sent : -ORTH_ERR_EAGAIN;
            task_mark_sleeping(current);
            socket_set_waiter(&sock->tx_waiter, current);
            if (tcp_sndbuf(sock->pcb.tcp) != 0) {
                socket_clear_waiter(&sock->tx_waiter, current);
                if (current->state == TASK_SLEEPING) task_wake(current);
                continue;
            }
            kernel_yield();
            socket_clear_waiter(&sock->tx_waiter, current);
            continue;
        }
        uint16_t chunk = (uint16_t)(len - sent);
        if (chunk > wnd) chunk = wnd;
        err_t err = tcp_write(sock->pcb.tcp, (const uint8_t*)buf + sent, chunk, TCP_WRITE_FLAG_COPY);
        if (err == ERR_MEM) {
            if (f->flags & ORTH_LINUX_O_NONBLOCK) return sent ? (int64_t)sent : -ORTH_ERR_EAGAIN;
            (void)tcp_output(sock->pcb.tcp);
            task_mark_sleeping(current);
            socket_set_waiter(&sock->tx_waiter, current);
            if (tcp_sndbuf(sock->pcb.tcp) != 0) {
                socket_clear_waiter(&sock->tx_waiter, current);
                if (current->state == TASK_SLEEPING) task_wake(current);
                continue;
            }
            kernel_yield();
            socket_clear_waiter(&sock->tx_waiter, current);
            continue;
        }
        if (err != ERR_OK) return socket_send_error(err, sent);
        sent += chunk;
        err = tcp_output(sock->pcb.tcp);
        if (err != ERR_OK) return socket_send_error(err, sent);
    }
    return (int64_t)sent;
}

int net_socket_socket(int domain, int type, int protocol) {
    struct task* current = get_current_task();
    int base_type = type & SOCK_TYPE_MASK;
    int extra_flags = type & ~SOCK_TYPE_MASK;

    if (!current || domain != AF_INET) return -1;
    if (extra_flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) return -1;
    if (base_type != SOCK_DGRAM && base_type != SOCK_STREAM) return -1;
    if (base_type == SOCK_DGRAM && protocol != 0 && protocol != 17) return -1;
    if (base_type == SOCK_STREAM && protocol != 0 && protocol != 6) return -1;

    int fd = alloc_fd_slot(current);
    if (fd < 0) return -1;

    net_socket_backend_t* sock = alloc_socket_backend();
    if (!sock) return -1;

    sock->domain = domain;
    sock->type = base_type;
    sock->protocol = protocol;

    if (base_type == SOCK_DGRAM) {
        sock->pcb.udp = udp_new();
        if (!sock->pcb.udp) {
            destroy_socket_backend(sock);
            return -1;
        }
        udp_recv(sock->pcb.udp, udp_socket_recv, sock);
    } else {
        sock->pcb.tcp = tcp_new();
        if (!sock->pcb.tcp) {
            destroy_socket_backend(sock);
            return -1;
        }
    }

    fs_file_t* file = alloc_socket_file(sock);
    if (!file) {
        destroy_socket_backend(sock);
        return -1;
    }
    current->fds[fd].type = FT_SOCKET;
    current->fds[fd].file = file;
    current->fds[fd].data = 0;
    current->fds[fd].size = 0;
    current->fds[fd].offset = 0;
    current->fds[fd].in_use = 1;
    current->fds[fd].flags = (extra_flags & SOCK_NONBLOCK) ? ORTH_LINUX_O_NONBLOCK : 0;
    current->fds[fd].fd_flags = 0;
    current->fds[fd].name[0] = '\0';
    current->fds[fd].aux0 = 0;
    current->fds[fd].aux1 = 0;
    return fd;
}

int net_socket_bind(int fd, const void* addr, uint32_t addrlen) {
    struct task* current = get_current_task();
    if (!current || !addr) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    net_socket_backend_t* sock = socket_backend_from_fd(&current->fds[fd]);
    if (!sock || addrlen < sizeof(struct orth_sockaddr_in)) return -1;

    const struct orth_sockaddr_in* in = (const struct orth_sockaddr_in*)addr;
    if (in->sin_family != AF_INET) return -1;

    ip_addr_t ipaddr;
    if (in->sin_addr.s_addr == INADDR_ANY) ip_addr_set_any(0, &ipaddr);
    else ip_addr_copy_from_ip4(ipaddr, *(const ip4_addr_t*)&in->sin_addr.s_addr);

    if (sock->type == SOCK_DGRAM) {
        if (udp_bind(sock->pcb.udp, &ipaddr, be16_to_cpu(in->sin_port)) != ERR_OK) return -1;
    } else {
        if (tcp_bind(sock->pcb.tcp, &ipaddr, be16_to_cpu(in->sin_port)) != ERR_OK) return -1;
    }

    sock->local_port = in->sin_port;
    sock->local_addr = in->sin_addr.s_addr;
    return 0;
}

int net_socket_connect(int fd, const void* addr, uint32_t addrlen) {
    struct task* current = get_current_task();
    if (!current || !addr) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    net_socket_backend_t* sock = socket_backend_from_fd(&current->fds[fd]);
    if (!sock || addrlen < sizeof(struct orth_sockaddr_in)) return -1;

    const struct orth_sockaddr_in* in = (const struct orth_sockaddr_in*)addr;
    if (in->sin_family != AF_INET) return -1;

    ip_addr_t ipaddr;
    ip_addr_copy_from_ip4(ipaddr, *(const ip4_addr_t*)&in->sin_addr.s_addr);

    if (sock->type == SOCK_STREAM) {
        if (!sock->pcb.tcp || sock->listening) return -1;
        if (sock->connected) return 0;

        sock->error = 0;
        sock->eof = 0;
        sock->connecting = 1;
        sock->peer_port = in->sin_port;
        sock->peer_addr = in->sin_addr.s_addr;

        tcp_arg(sock->pcb.tcp, sock);
        tcp_recv(sock->pcb.tcp, tcp_stream_recv);
        tcp_sent(sock->pcb.tcp, tcp_stream_sent);
        tcp_err(sock->pcb.tcp, tcp_stream_err);

        err_t err = tcp_connect(sock->pcb.tcp, &ipaddr, be16_to_cpu(in->sin_port), tcp_client_connected);
        if (err != ERR_OK) {
            sock->connecting = 0;
            sock->error = err;
            return -socket_errno_from_lwip(err);
        }

        while (sock->connecting) {
            task_mark_sleeping(current);
            socket_set_waiter(&sock->connect_waiter, current);
            if (!sock->connecting) {
                socket_clear_waiter(&sock->connect_waiter, current);
                if (current->state == TASK_SLEEPING) task_wake(current);
                continue;
            }
            kernel_yield();
            socket_clear_waiter(&sock->connect_waiter, current);
        }
        if (sock->connected) return 0;
        return -socket_errno_from_lwip((err_t)sock->error);
    }

    if (udp_connect(sock->pcb.udp, &ipaddr, be16_to_cpu(in->sin_port)) != ERR_OK) return -1;

    sock->connected = 1;
    sock->peer_port = in->sin_port;
    sock->peer_addr = in->sin_addr.s_addr;
    return 0;
}

int net_socket_listen(int fd, int backlog) {
    struct task* current = get_current_task();
    if (!current) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    net_socket_backend_t* sock = socket_backend_from_fd(&current->fds[fd]);
    if (!sock || sock->type != SOCK_STREAM || sock->listening) return -1;

    err_t err = ERR_OK;
    struct tcp_pcb* listener = tcp_listen_with_backlog_and_err(sock->pcb.tcp, (u8_t)((backlog > 0) ? backlog : 1), &err);
    if (!listener || err != ERR_OK) return -1;
    sock->pcb.tcp = listener;
    sock->listening = 1;
    tcp_arg(listener, sock);
    tcp_accept(listener, tcp_listener_accept);
    return 0;
}

int net_socket_accept(int fd, void* addr, uint32_t* addrlen) {
    struct task* current = get_current_task();
    if (!current) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    file_descriptor_t* listen_fd = &current->fds[fd];
    net_socket_backend_t* listener = socket_backend_from_fd(listen_fd);
    if (!listener || listener->type != SOCK_STREAM || !listener->listening) return -1;

    while (!listener->accept_head) {
        if (listen_fd->flags & ORTH_LINUX_O_NONBLOCK) return -ORTH_ERR_EAGAIN;
        task_mark_sleeping(current);
        socket_set_waiter(&listener->accept_waiter, current);
        if (listener->accept_head) {
            socket_clear_waiter(&listener->accept_waiter, current);
            if (current->state == TASK_SLEEPING) task_wake(current);
            continue;
        }
        kernel_yield();
        socket_clear_waiter(&listener->accept_waiter, current);
    }

    int newfd = alloc_fd_slot(current);
    if (newfd < 0) return -1;

    net_socket_backend_t* child = listener->accept_head;
    listener->accept_head = child->accept_next;
    if (!listener->accept_head) listener->accept_tail = 0;
    child->accept_next = 0;

    fs_file_t* file = alloc_socket_file(child);
    if (!file) {
        destroy_socket_backend(child);
        return -1;
    }
    current->fds[newfd].type = FT_SOCKET;
    current->fds[newfd].file = file;
    current->fds[newfd].data = 0;
    current->fds[newfd].size = 0;
    current->fds[newfd].offset = 0;
    current->fds[newfd].in_use = 1;
    current->fds[newfd].flags = 0;
    current->fds[newfd].fd_flags = 0;
    current->fds[newfd].name[0] = '\0';
    current->fds[newfd].aux0 = 0;
    current->fds[newfd].aux1 = 0;

    (void)fill_sockaddr_in(addr, addrlen, child->peer_addr, child->peer_port);
    return newfd;
}

int net_socket_setsockopt(int fd, int level, int optname, const void* optval, uint32_t optlen) {
    struct task* current = get_current_task();
    if (!current) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    net_socket_backend_t* sock = socket_backend_from_fd(&current->fds[fd]);
    if (!sock) return -1;
    if (level != SOL_SOCKET) return -1;
    if (optname == SO_REUSEADDR) {
        int enable = 1;
        struct ip_pcb* ip_pcb = 0;
        if (optval && optlen >= sizeof(int)) {
            enable = (*(const int*)optval != 0);
        }
        sock->reuseaddr = enable ? 1 : 0;
        if (sock->type == SOCK_DGRAM && sock->pcb.udp) ip_pcb = (struct ip_pcb*)sock->pcb.udp;
        else if (sock->type == SOCK_STREAM && sock->pcb.tcp) ip_pcb = (struct ip_pcb*)sock->pcb.tcp;
        if (ip_pcb) {
            if (enable) ip_set_option(ip_pcb, SOF_REUSEADDR);
            else ip_reset_option(ip_pcb, SOF_REUSEADDR);
        }
        return 0;
    }
    if (optname == SO_KEEPALIVE) {
        return 0;
    }
    return -1;
}

int net_socket_getsockname(int fd, void* addr, uint32_t* addrlen) {
    struct task* current = get_current_task();
    if (!current) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    net_socket_backend_t* sock = socket_backend_from_fd(&current->fds[fd]);
    if (!sock) return -1;
    return fill_sockaddr_in(addr, addrlen, sock->local_addr, sock->local_port) ? 0 : -1;
}

int net_socket_getpeername(int fd, void* addr, uint32_t* addrlen) {
    struct task* current = get_current_task();
    if (!current) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    net_socket_backend_t* sock = socket_backend_from_fd(&current->fds[fd]);
    if (!sock || !sock->connected) return -1;
    return fill_sockaddr_in(addr, addrlen, sock->peer_addr, sock->peer_port) ? 0 : -1;
}

int net_socket_shutdown(int fd, int how) {
    struct task* current = get_current_task();
    (void)how;
    if (!current) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    net_socket_backend_t* sock = socket_backend_from_fd(&current->fds[fd]);
    if (!sock) return -1;
    if (sock->type == SOCK_STREAM && sock->pcb.tcp) {
        (void)tcp_output(sock->pcb.tcp);
    }
    return 0;
}

int64_t net_socket_sendto(int fd, const void* buf, size_t len, int flags, const void* dest_addr, uint32_t addrlen) {
    struct task* current = get_current_task();
    if (!current || !buf) return -1;
    if (flags & ~MSG_NOSIGNAL) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    file_descriptor_t* f = &current->fds[fd];
    net_socket_backend_t* sock = socket_backend_from_fd(f);
    if (!sock) return -1;

    if (sock->type == SOCK_STREAM) {
        if (dest_addr) return -1;
        return socket_send_stream(f, sock, buf, len);
    }

    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, (uint16_t)len, PBUF_RAM);
    if (!p) return -1;
    if (len > 0 && pbuf_take(p, buf, (uint16_t)len) != ERR_OK) {
        pbuf_free(p);
        return -1;
    }

    err_t err;
    if (dest_addr) {
        if (addrlen < sizeof(struct orth_sockaddr_in)) {
            pbuf_free(p);
            return -1;
        }
        const struct orth_sockaddr_in* in = (const struct orth_sockaddr_in*)dest_addr;
        if (in->sin_family != AF_INET) {
            pbuf_free(p);
            return -1;
        }
        ip_addr_t ipaddr;
        ip_addr_copy_from_ip4(ipaddr, *(const ip4_addr_t*)&in->sin_addr.s_addr);
        err = udp_sendto(sock->pcb.udp, p, &ipaddr, be16_to_cpu(in->sin_port));
    } else {
        if (!sock->connected) {
            pbuf_free(p);
            return -1;
        }
        err = udp_send(sock->pcb.udp, p);
    }
    pbuf_free(p);
    if (err != ERR_OK) return -1;
    return (int64_t)len;
}

int64_t net_socket_recvfrom(int fd, void* buf, size_t len, int flags, void* src_addr, uint32_t* addrlen) {
    struct task* current = get_current_task();
    if (!current || !buf) return -1;
    if (flags != 0) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    file_descriptor_t* f = &current->fds[fd];
    net_socket_backend_t* sock = socket_backend_from_fd(f);
    if (!sock) return -1;

    if (sock->type == SOCK_STREAM) {
        if (src_addr || addrlen) return -1;
        return socket_recv_stream(f, sock, buf, len);
    }
    return socket_recv_dgram(f, sock, buf, len, src_addr, addrlen);
}

int64_t net_socket_read_fd(file_descriptor_t* f, void* buf, size_t count) {
    net_socket_backend_t* sock = socket_backend_from_fd(f);
    if (!sock) return -1;
    if (sock->type == SOCK_STREAM) return socket_recv_stream(f, sock, buf, count);
    return socket_recv_dgram(f, sock, buf, count, 0, 0);
}

int64_t net_socket_write_fd(file_descriptor_t* f, const void* buf, size_t count) {
    net_socket_backend_t* sock = socket_backend_from_fd(f);
    if (!sock) return -1;
    if (sock->type == SOCK_STREAM) return socket_send_stream(f, sock, buf, count);
    if (!sock->connected) return -1;

    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, (uint16_t)count, PBUF_RAM);
    if (!p) return -1;
    if (count > 0 && pbuf_take(p, buf, (uint16_t)count) != ERR_OK) {
        pbuf_free(p);
        return -1;
    }
    err_t err = udp_send(sock->pcb.udp, p);
    pbuf_free(p);
    if (err != ERR_OK) return -1;
    return (int64_t)count;
}
