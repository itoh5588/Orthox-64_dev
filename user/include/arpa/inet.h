#ifndef _ARPA_INET_H
#define _ARPA_INET_H

#include <netinet/in.h>
#include <machine/endian.h>

in_addr_t inet_addr(const char* cp);
char* inet_ntoa(struct in_addr in);
int inet_aton(const char* cp, struct in_addr* inp);

#define htonl(x) __htonl(x)
#define htons(x) __htons(x)
#define ntohl(x) __ntohl(x)
#define ntohs(x) __ntohs(x)

#endif
