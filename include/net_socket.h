#ifndef NET_SOCKET_H
#define NET_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include "fs.h"

int net_socket_socket(int domain, int type, int protocol);
int net_socket_connect(int fd, const void* addr, uint32_t addrlen);
int net_socket_bind(int fd, const void* addr, uint32_t addrlen);
int net_socket_listen(int fd, int backlog);
int net_socket_accept(int fd, void* addr, uint32_t* addrlen);
int net_socket_setsockopt(int fd, int level, int optname, const void* optval, uint32_t optlen);
int net_socket_getsockname(int fd, void* addr, uint32_t* addrlen);
int net_socket_getpeername(int fd, void* addr, uint32_t* addrlen);
int net_socket_shutdown(int fd, int how);
int64_t net_socket_sendto(int fd, const void* buf, size_t len, int flags,
                          const void* dest_addr, uint32_t addrlen);
int64_t net_socket_recvfrom(int fd, void* buf, size_t len, int flags, void* src_addr,
                            uint32_t* addrlen);
int64_t net_socket_read_fd(file_descriptor_t* f, void* buf, size_t count);
int64_t net_socket_write_fd(file_descriptor_t* f, const void* buf, size_t count);

#endif
