#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H

#include <sys/types.h>

#ifndef _FSBLKCNT_T_DECLARED
typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;
#define _FSBLKCNT_T_DECLARED
#endif

struct statfs {
    long f_type;
    long f_bsize;
    fsblkcnt_t f_blocks;
    fsblkcnt_t f_bfree;
    fsblkcnt_t f_bavail;
    fsfilcnt_t f_files;
    fsfilcnt_t f_ffree;
    long f_fsid[2];
    long f_namelen;
    long f_frsize;
    long f_flags;
    long f_spare[4];
};

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);

#endif
