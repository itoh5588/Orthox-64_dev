#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void sync(void);

static int write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

int main(void) {
    const char *path = "/vblk_stress.txt";
    char expected[128];
    char actual[128];

    printf("vblkstress: start\n");

    for (int i = 0; i < 32; i++) {
        int expected_len = snprintf(expected, sizeof(expected),
                                    "orthox virtio blk stress iteration %02d\n", i);
        if (expected_len < 0 || (size_t)expected_len >= sizeof(expected)) {
            printf("vblkstress: format failed\n");
            return 1;
        }

        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror("vblkstress: open write");
            return 1;
        }
        if (write_all(fd, expected, (size_t)expected_len) < 0) {
            perror("vblkstress: write");
            close(fd);
            return 1;
        }
        close(fd);
        sync();

        fd = open(path, O_RDONLY);
        if (fd < 0) {
            perror("vblkstress: open read");
            return 1;
        }
        ssize_t n = read(fd, actual, sizeof(actual) - 1);
        if (n < 0) {
            perror("vblkstress: read");
            close(fd);
            return 1;
        }
        close(fd);
        actual[n] = '\0';

        if (n != expected_len || strcmp(actual, expected) != 0) {
            printf("vblkstress: mismatch at iteration %d\n", i);
            printf("vblkstress: got [%s]\n", actual);
            return 1;
        }
    }

    printf("vblkstress: PASS\n");
    return 0;
}
