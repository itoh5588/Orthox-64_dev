#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#include <sys/types.h>
#include <sys/socket.h>

#ifndef _IN_ADDR_T_DECLARED
typedef uint32_t in_addr_t;
#define _IN_ADDR_T_DECLARED
#endif
#ifndef _IN_PORT_T_DECLARED
typedef uint16_t in_port_t;
#define _IN_PORT_T_DECLARED
#endif

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

struct in6_addr {
    unsigned char s6_addr[16];
};

struct sockaddr_in6 {
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

#define INADDR_ANY ((in_addr_t)0)

#endif
