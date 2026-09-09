#ifndef _BYTESWAP_H
#define _BYTESWAP_H

#include <machine/endian.h>

#define bswap_16(x) __bswap16(x)
#define bswap_32(x) __bswap32(x)
#define bswap_64(x) __bswap64(x)

#endif
