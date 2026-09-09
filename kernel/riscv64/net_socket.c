/* riscv64 にはネットワークが無い (lwIP も kernel/net_socket.c も
 * ビルドしていない)。**だが共有の kernel/linux_syscall.c は socket 系を
 * 呼ぶ**ので、リンクを通すためのスタブがここに要る。
 *
 * **黙って 0 を返さないこと。** 成功に見えると、呼んだ側は使えない fd を
 * 掴んで先へ進んでしまう。全部 -ENOSYS で断る。
 *
 * riscv64 だけがビルドできなくなる事故は既に 2 度起きている
 * (2026-08-31 の symlinkat/readlinkat、そして今回)。**共有の
 * ディスパッチャに関数を足したら、riscv64 側にも口を用意する。** */
#include "net_socket.h"
#include "linux_errno.h"

void net_socket_dup_fd(file_descriptor_t* f) {
    (void)f;
}

int net_socket_socket(int domain, int type, int protocol) {
    (void)domain; (void)type; (void)protocol;
    return -LINUX_ENOSYS;
}

int net_socket_connect(int fd, const void* addr, uint32_t addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    return -LINUX_ENOSYS;
}

int net_socket_bind(int fd, const void* addr, uint32_t addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    return -LINUX_ENOSYS;
}

int net_socket_listen(int fd, int backlog) {
    (void)fd; (void)backlog;
    return -LINUX_ENOSYS;
}

int net_socket_accept(int fd, void* addr, uint32_t* addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    return -LINUX_ENOSYS;
}

int net_socket_setsockopt(int fd, int level, int optname, const void* optval, uint32_t optlen) {
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
    return -LINUX_ENOSYS;
}

int net_socket_getsockopt(int fd, int level, int optname, void* optval, uint32_t* optlen) {
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
    return -LINUX_ENOSYS;
}

int net_socket_getsockname(int fd, void* addr, uint32_t* addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    return -LINUX_ENOSYS;
}

int net_socket_getpeername(int fd, void* addr, uint32_t* addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    return -LINUX_ENOSYS;
}

int net_socket_shutdown(int fd, int how) {
    (void)fd; (void)how;
    return -LINUX_ENOSYS;
}

int64_t net_socket_sendto(int fd, const void* buf, size_t len, int flags,
                          const void* dest_addr, uint32_t addrlen) {
    (void)fd; (void)buf; (void)len; (void)flags; (void)dest_addr; (void)addrlen;
    return -LINUX_ENOSYS;
}

int64_t net_socket_recvfrom(int fd, void* buf, size_t len, int flags, void* src_addr,
                            uint32_t* addrlen) {
    (void)fd; (void)buf; (void)len; (void)flags; (void)src_addr; (void)addrlen;
    return -LINUX_ENOSYS;
}

int64_t net_socket_read_fd(file_descriptor_t* f, void* buf, size_t count) {
    (void)f; (void)buf; (void)count;
    return -LINUX_ENOSYS;
}

int64_t net_socket_write_fd(file_descriptor_t* f, const void* buf, size_t count) {
    (void)f; (void)buf; (void)count;
    return -LINUX_ENOSYS;
}

/* poll(2) の readiness。**riscv64 にネットワークは無い。**
 * 「ソケットでない」を返す = 呼び出し側は通常ファイル扱いに退く */
int net_socket_poll_state(file_descriptor_t* f) {
    (void)f;
    return -1;
}
