#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fail(const char *msg) {
    printf("xv6_reclaim_test: FAIL: %s errno=%d\n", msg, errno);
    return 1;
}

static int write_blocks(const char *path, int blocks, unsigned char seed) {
    unsigned char buf[1024];
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    for (int i = 0; i < blocks; i++) {
        memset(buf, seed + i, sizeof(buf));
        if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
            close(fd);
            return -1;
        }
    }
    close(fd);
    return 0;
}

int main(void) {
    const char *trunc_path = "/work/xv6_reclaim_trunc.bin";
    const char *unlink_path = "/work/xv6_reclaim_unlink.bin";
    int fd;

    unlink(trunc_path);
    unlink(unlink_path);

    if (write_blocks(trunc_path, 192, 0x20) < 0) {
        return fail("write truncate target");
    }
    fd = open(trunc_path, O_RDWR);
    if (fd < 0) return fail("open truncate target");
    if (ftruncate(fd, 0) < 0) {
        close(fd);
        return fail("ftruncate zero");
    }
    close(fd);

    if (write_blocks(unlink_path, 96, 0x60) < 0) {
        return fail("write unlink target");
    }
    if (unlink(unlink_path) < 0) {
        return fail("unlink target");
    }
    if (unlink(trunc_path) < 0) {
        return fail("cleanup truncate target");
    }

    printf("xv6-reclaim-smoke: PASS\n");
    return 0;
}
