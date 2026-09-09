#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fail(const char *msg) {
    printf("preadpwrite_test: FAIL: %s errno=%d\n", msg, errno);
    return 1;
}

static int expect_offset(int fd, off_t want, const char *where) {
    off_t got = lseek(fd, 0, SEEK_CUR);
    if (got != want) {
        errno = 0;
        printf("preadpwrite_test: offset mismatch at %s got=%lld want=%lld\n",
               where, (long long)got, (long long)want);
        return 1;
    }
    return 0;
}

int main(void) {
    const char *path = "/work/preadpwrite_test.bin";
    char buf[16];
    int fd;

    unlink(path);
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fail("open");

    if (write(fd, "abcdefghij", 10) != 10) {
        close(fd);
        return fail("initial write");
    }
    if (lseek(fd, 4, SEEK_SET) != 4) {
        close(fd);
        return fail("seek 4");
    }

    memset(buf, 0, sizeof(buf));
    if (pread(fd, buf, 3, 2) != 3 || memcmp(buf, "cde", 3) != 0) {
        close(fd);
        return fail("pread content");
    }
    if (expect_offset(fd, 4, "after pread") != 0) {
        close(fd);
        return 1;
    }

    if (pwrite(fd, "XYZ", 3, 6) != 3) {
        close(fd);
        return fail("pwrite content");
    }
    if (expect_offset(fd, 4, "after pwrite") != 0) {
        close(fd);
        return 1;
    }

    memset(buf, 0, sizeof(buf));
    if (read(fd, buf, 6) != 6 || memcmp(buf, "efXYZj", 6) != 0) {
        close(fd);
        return fail("read after pwrite");
    }

    errno = 0;
    if (pread(fd, buf, 1, -1) != -1 || errno != EINVAL) {
        close(fd);
        return fail("negative pread offset should set EINVAL");
    }
    if (expect_offset(fd, 10, "after negative pread") != 0) {
        close(fd);
        return 1;
    }

    errno = 0;
    if (pwrite(fd, "Q", 1, -1) != -1 || errno != EINVAL) {
        close(fd);
        return fail("negative pwrite offset should set EINVAL");
    }
    if (expect_offset(fd, 10, "after negative pwrite") != 0) {
        close(fd);
        return 1;
    }

    close(fd);
    if (unlink(path) < 0) return fail("unlink");

    printf("preadpwrite-smoke: PASS\n");
    return 0;
}
