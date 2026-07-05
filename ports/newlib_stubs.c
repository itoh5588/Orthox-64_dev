#include <errno.h>
#include <sys/types.h>
#include <time.h>

// Python のビルドに必要な、Orthox-64 の既存スタブにない関数群

int fdatasync(int fd) {
    return 0; // 成功を装う
}

int fsync(int fd) {
    return 0; // 成功を装う
}

int symlink(const char *path1, const char *path2) {
    errno = ENOSYS;
    return -1;
}

int clock_getres(clockid_t clock_id, struct timespec *res) {
    if (res) {
        res->tv_sec = 0;
        res->tv_nsec = 1000000; // 1ms resolution
    }
    return 0;
}
