#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHUNK_SIZE (112 * 1024)
#define BLOCK_SIZE 1024
#define TRIPLE_BASE ((off_t)(9 + 256 + 65536) * BLOCK_SIZE)

static unsigned char write_buf[CHUNK_SIZE];
static unsigned char read_buf[8192];

static int fail(const char *msg) {
    printf("xv6_largewrite_test: FAIL: %s errno=%d\n", msg, errno);
    return 1;
}

static void fill_pattern(unsigned int seed) {
    for (int i = 0; i < CHUNK_SIZE; i++) {
        write_buf[i] = (unsigned char)((i * 17 + seed) & 0xff);
    }
}

static int verify_pattern(int fd, off_t offset, unsigned int seed) {
    size_t done = 0;

    errno = 0;
    if (lseek(fd, offset, SEEK_SET) != offset) {
        printf("xv6_largewrite_test: verify lseek failed offset=%lld errno=%d\n",
               (long long)offset, errno);
        return -1;
    }
    while (done < CHUNK_SIZE) {
        size_t want = CHUNK_SIZE - done;
        if (want > sizeof(read_buf)) want = sizeof(read_buf);
        errno = 0;
        ssize_t n = read(fd, read_buf, want);
        if (n != (ssize_t)want) {
            printf("xv6_largewrite_test: read failed got=%lld want=%lld done=%lld errno=%d\n",
                   (long long)n, (long long)want, (long long)done, errno);
            return -1;
        }
        for (ssize_t i = 0; i < n; i++) {
            unsigned char expected = (unsigned char)(((done + (size_t)i) * 17 + seed) & 0xff);
            if (read_buf[i] != expected) {
                printf("xv6_largewrite_test: mismatch at=%lld got=%u expected=%u\n",
                       (long long)(done + (size_t)i), read_buf[i], expected);
                return -1;
            }
        }
        done += (size_t)n;
    }
    return 0;
}

static int write_one_chunk(const char *path, off_t offset, unsigned int seed) {
    int fd;

    unlink(path);
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    errno = 0;
    if (lseek(fd, offset, SEEK_SET) != offset) {
        printf("xv6_largewrite_test: write lseek failed path=%s offset=%lld errno=%d\n",
               path, (long long)offset, errno);
        close(fd);
        return -1;
    }
    fill_pattern(seed);
    errno = 0;
    ssize_t n = write(fd, write_buf, CHUNK_SIZE);
    if (n != CHUNK_SIZE) {
        printf("xv6_largewrite_test: write failed path=%s got=%lld want=%d errno=%d\n",
               path, (long long)n, CHUNK_SIZE, errno);
        close(fd);
        return -1;
    }
    if (verify_pattern(fd, offset, seed) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int main(void) {
    const char *seq_path = "/work/xv6_largewrite_seq.bin";
    const char *triple_path = "/work/xv6_largewrite_triple.bin";

    if (write_one_chunk(seq_path, 0, 0x31) < 0) {
        return fail("sequential 112KiB write");
    }
    if (write_one_chunk(triple_path, TRIPLE_BASE, 0xa7) < 0) {
        return fail("triple-indirect 112KiB write");
    }
    if (unlink(seq_path) < 0) return fail("unlink sequential file");
    if (unlink(triple_path) < 0) return fail("unlink triple file");

    printf("xv6-largewrite-smoke: PASS\n");
    return 0;
}
