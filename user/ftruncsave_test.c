#define _GNU_SOURCE

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

static int fail(const char *msg) {
    printf("ftruncsave_test: FAIL: %s errno=%d\n", msg, errno);
    return 1;
}

static int read_all(const char *path, char *buf, size_t buf_size) {
    int fd = open(path, O_RDONLY);
    int n;
    if (fd < 0) return -1;
    n = (int)read(fd, buf, buf_size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

static int save_like_kilo(const char *path, const char *payload) {
    int fd;
    size_t len = strlen(payload);

    fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)len) < 0) {
        close(fd);
        return -1;
    }
    if (write(fd, payload, len) != (ssize_t)len) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int dir_contains(const char *dir_path, const char *name) {
    DIR *dir = opendir(dir_path);
    struct dirent *ent;

    if (!dir) return -1;
    while ((ent = readdir(dir)) != 0) {
        if (strcmp(ent->d_name, name) == 0) {
            closedir(dir);
            return 1;
        }
    }
    closedir(dir);
    return 0;
}

static int check_ramfs_full_write(void) {
    const char *path = "/work/ramfs_full.bin";
    char block[4096];
    char extra = 'x';
    struct iovec iov;
    int fd;

    memset(block, 'A', sizeof(block));
    unlink(path);

    fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return fail("open ramfs_full.bin failed");

    for (int i = 0; i < 256; i++) {
        if (write(fd, block, sizeof(block)) != (ssize_t)sizeof(block)) {
            close(fd);
            return fail("fill ramfs_full.bin failed");
        }
    }

    errno = 0;
    if (write(fd, &extra, 1) != -1 || errno != EFBIG) {
        close(fd);
        return fail("full RAMFS write should set EFBIG");
    }

    iov.iov_base = &extra;
    iov.iov_len = 1;
    errno = 0;
    if (writev(fd, &iov, 1) != -1 || errno != EFBIG) {
        close(fd);
        return fail("full RAMFS writev should set EFBIG");
    }

    close(fd);
    if (unlink(path) < 0) return fail("cleanup ramfs_full.bin failed");
    return 0;
}

int main(void) {
    const char *path = "/work/ftruncsave.txt";
    const char *leaf = "ftruncsave.txt";
    const char *long_text = "alpha\nbeta\ngamma\n";
    const char *short_text = "short\n";
    char buf[128];
    int n;

    unlink(path);

    if (save_like_kilo(path, long_text) < 0) {
        return fail("initial save_like_kilo failed");
    }
    n = read_all(path, buf, sizeof(buf));
    if (n < 0 || strcmp(buf, long_text) != 0) {
        errno = 0;
        return fail("initial readback mismatch");
    }
    if (dir_contains("/work", leaf) != 1) {
        errno = 0;
        return fail("saved file missing from /work directory listing");
    }

    if (save_like_kilo(path, short_text) < 0) {
        return fail("short overwrite save_like_kilo failed");
    }
    n = read_all(path, buf, sizeof(buf));
    if (n < 0 || strcmp(buf, short_text) != 0) {
        errno = 0;
        return fail("short overwrite left stale tail");
    }

    if (truncate(path, 2) < 0) {
        return fail("truncate(path) failed");
    }
    n = read_all(path, buf, sizeof(buf));
    if (n != 2 || strncmp(buf, "sh", 2) != 0) {
        errno = 0;
        return fail("truncate(path) readback mismatch");
    }

    errno = 0;
    if (ftruncate(-1, 4) != -1 || errno != EBADF) {
        return fail("ftruncate invalid fd should set EBADF");
    }

    if (check_ramfs_full_write() != 0) {
        return 1;
    }

    if (unlink(path) < 0) {
        return fail("cleanup unlink failed");
    }

    printf("ftruncsave_test: PASS\n");
    return 0;
}
