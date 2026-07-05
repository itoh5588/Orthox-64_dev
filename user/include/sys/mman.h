#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <sys/types.h>

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#define PROT_NONE  0x0

#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED   0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON    MAP_ANONYMOUS

#define MAP_FAILED ((void *)-1)

#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED   2
#define MREMAP_DONTUNMAP 4

#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t length, int prot);
void *mremap(void *old_addr, size_t old_len, size_t new_len, int flags, ...);
int msync(void *addr, size_t length, int flags);

#endif
