#include <stdint.h>
#include <stddef.h>
#include "lwip_port.h"
#include "net_socket.h"
#include "sys_internal.h"

int sys_socket(int domain, int type, int protocol) {
    return net_socket_socket(domain, type, protocol);
}

int sys_connect(int fd, const void* addr, uint32_t addrlen) {
    return net_socket_connect(fd, addr, addrlen);
}

int sys_bind(int fd, const void* addr, uint32_t addrlen) {
    return net_socket_bind(fd, addr, addrlen);
}

int sys_listen(int fd, int backlog) {
    return net_socket_listen(fd, backlog);
}

int sys_accept(int fd, void* addr, uint32_t* addrlen) {
    return net_socket_accept(fd, addr, addrlen);
}

int sys_setsockopt(int fd, int level, int optname, const void* optval, uint32_t optlen) {
    return net_socket_setsockopt(fd, level, optname, optval, optlen);
}

int sys_getsockname(int fd, void* addr, uint32_t* addrlen) {
    return net_socket_getsockname(fd, addr, addrlen);
}

int sys_getpeername(int fd, void* addr, uint32_t* addrlen) {
    return net_socket_getpeername(fd, addr, addrlen);
}

int sys_shutdown(int fd, int how) {
    return net_socket_shutdown(fd, how);
}

int64_t sys_sendto(int fd, const void* buf, size_t len, int flags,
                   const void* dest_addr, uint32_t addrlen) {
    return net_socket_sendto(fd, buf, len, flags, dest_addr, addrlen);
}

int64_t sys_recvfrom(int fd, void* buf, size_t len, int flags, void* src_addr,
                     uint32_t* addrlen) {
    return net_socket_recvfrom(fd, buf, len, flags, src_addr, addrlen);
}

int sys_dns_lookup(const char* hostname, uint32_t* out_addr) {
    return lwip_port_lookup_ipv4(hostname, out_addr);
}
