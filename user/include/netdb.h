#ifndef _NETDB_H
#define _NETDB_H

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

struct hostent {
    char* h_name;
    char** h_aliases;
    int h_addrtype;
    int h_length;
    char** h_addr_list;
};

#define h_addr h_addr_list[0]

struct servent {
    char* s_name;
    char** s_aliases;
    int s_port;
    char* s_proto;
};

struct netent {
    char* n_name;
    char** n_aliases;
    int n_addrtype;
    unsigned long n_net;
};

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr* ai_addr;
    char* ai_canonname;
    struct addrinfo* ai_next;
};

#define AI_CANONNAME    0x0002
#define AI_NUMERICHOST  0x0004

#define NI_NUMERICHOST  0x0001
#define NI_NUMERICSERV  0x0002
#define NI_NAMEREQD     0x0004
#define NI_NUMERICSCOPE 0x0008

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res);
void freeaddrinfo(struct addrinfo* res);
const char* gai_strerror(int errcode);
struct hostent* gethostbyname(const char* name);
struct servent* getservbyname(const char* name, const char* proto);
struct netent* getnetbyname(const char* name);
struct netent* getnetbyaddr(unsigned long net, int type);
char* hstrerror(int err);
int getnameinfo(const struct sockaddr* addr, socklen_t addrlen,
                char* host, socklen_t hostlen,
                char* serv, socklen_t servlen, int flags);

extern int h_errno;

#endif
