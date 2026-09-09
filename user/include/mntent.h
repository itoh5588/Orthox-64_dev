#ifndef _MNTENT_H
#define _MNTENT_H

#include <stdio.h>

#define MNTOPT_DEFAULTS "defaults"
#define MNTOPT_RW "rw"
#define MNTOPT_RO "ro"
#define MNTOPT_NOAUTO "noauto"

struct mntent {
    char *mnt_fsname;
    char *mnt_dir;
    char *mnt_type;
    char *mnt_opts;
    int mnt_freq;
    int mnt_passno;
};

FILE *setmntent(const char *filename, const char *type);
struct mntent *getmntent(FILE *stream);
struct mntent *getmntent_r(FILE *stream, struct mntent *result,
    char *buffer, int buflen);
int addmntent(FILE *stream, const struct mntent *mnt);
int endmntent(FILE *stream);
char *hasmntopt(const struct mntent *mnt, const char *opt);

#endif
