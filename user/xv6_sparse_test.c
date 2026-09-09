#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fail(const char *msg) {
    printf("xv6_sparse_test: FAIL: %s errno=%d\n", msg, errno);
    return 1;
}

int main(void) {
    const char *path = "/work/xv6_sparse_probe.bin";
    unsigned char buf[4097];
    unsigned char marker = 0x5a;
    int fd;
    ssize_t n;

    unlink(path);
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fail("open");

    if (lseek(fd, 4096, SEEK_SET) != 4096) {
        close(fd);
        return fail("lseek hole");
    }
    if (write(fd, &marker, 1) != 1) {
        close(fd);
        return fail("write marker");
    }
    if (lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        return fail("lseek start");
    }

    memset(buf, 0xcc, sizeof(buf));
    n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n != (ssize_t)sizeof(buf)) return fail("read sparse file");
    for (int i = 0; i < 4096; i++) {
        if (buf[i] != 0) return fail("hole was not zero-filled");
    }
    if (buf[4096] != marker) return fail("marker mismatch");

    if (unlink(path) < 0) return fail("cleanup");
    printf("xv6-sparse-smoke: PASS\n");
    return 0;
}
