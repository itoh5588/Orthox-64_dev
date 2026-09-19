#ifndef ORTHOX_LWIP_ARCH_CC_H
#define ORTHOX_LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>

typedef unsigned long sys_prot_t;

#define BYTE_ORDER LITTLE_ENDIAN

#define LWIP_NO_UNISTD_H 1

/* **libc の limits.h が見えるときは ssize_t を自前で持つ (2026-09-19)。**
 * lwIP の arch.h は「SSIZE_MAX があれば ssize_t は unistd.h にある」とみなすが、
 * 上で unistd.h を読まないようにしているので、どこにも無くなる。
 * Orthox 上の gcc で aarch64 のカーネルを組むと (scripts/stage_aarch64_native_build.sh)
 * musl の limits.h が SSIZE_MAX を出し、lwip/etharp.h で
 *   error: unknown type name 'ssize_t'
 * になった。ホストの clang (freestanding の limits.h) には SSIZE_MAX が無く、
 * arch.h が typedef int ssize_t するので、ここは何もしない。
 * 型は musl (LP64) と同じ long にしておく (同じ型の typedef の重複は C11 で可) */
#include <limits.h>
#ifdef SSIZE_MAX
typedef long ssize_t;
#endif
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
