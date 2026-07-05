#ifndef _SYS_DIRENT_H_
#define _SYS_DIRENT_H_

#include <sys/types.h>

#ifndef MAXNAMLEN
#define MAXNAMLEN 255
#endif

struct dirent {
    ino_t d_ino;
    off_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[MAXNAMLEN + 1];
};

typedef struct {
    int dd_fd;
    off_t dd_loc;
    off_t dd_size;
    struct dirent dd_dirent;
} DIR;

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK    10
#define DT_SOCK   12

#endif
