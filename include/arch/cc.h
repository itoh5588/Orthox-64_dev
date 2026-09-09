#ifndef ORTHOX_LWIP_ARCH_CC_H
#define ORTHOX_LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>

typedef unsigned long sys_prot_t;

#define BYTE_ORDER LITTLE_ENDIAN

#define LWIP_NO_UNISTD_H 1
#define LWIP_NO_STDIO_H 1
#define LWIP_NO_CTYPE_H 1
#define LWIP_NO_INTTYPES_H 1
#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "04x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "08x"
#define SZT_F "u"

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_FLD_8(x) x
#define PACK_STRUCT_FLD_S(x) x

void orthox_lwip_diag(const char* msg);
void orthox_lwip_assert(const char* msg, const char* file, int line);
sys_prot_t sys_arch_protect(void);
void sys_arch_unprotect(sys_prot_t pval);
uint32_t sys_now(void);

#define LWIP_PLATFORM_DIAG(x) do { orthox_lwip_diag x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { orthox_lwip_assert((x), __FILE__, __LINE__); } while (0)

#endif
