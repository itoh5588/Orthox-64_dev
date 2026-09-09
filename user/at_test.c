#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(const char* msg, int code) {
    printf("%s\n", msg);
    return code;
}

int main(void) {
    int dirfd;
    int fd;
    int pipefd[2];
    int flags;
    char buf[16];
    struct stat st;

    printf("--- at/pipe2 Test ---\n");

    dirfd = open("/", O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) return fail("open / failed", 1);

    if (mkdirat(dirfd, "atroot", 0700) < 0) return fail("mkdirat atroot failed", 2);
    if (fstatat(dirfd, "atroot", &st, 0) < 0) return fail("fstatat atroot failed", 3);
    if ((st.st_mode & 0170000) != 0040000) return fail("atroot is not a directory", 5);
    if ((st.st_mode & 0777) != 0700) return fail("atroot mode mismatch", 6);

    fd = openat(dirfd, "atroot/file.txt", O_CREAT | O_RDWR | O_TRUNC, 0640);
    if (fd < 0) return fail("openat atroot/file.txt failed", 7);
    if (write(fd, "at-ok\n", 6) != 6) {
        close(fd);
        return fail("write atroot/file.txt failed", 8);
    }
    close(fd);

    if (fstatat(dirfd, "atroot/file.txt", &st, 0) < 0) return fail("fstatat atroot/file.txt failed", 9);
    if ((st.st_mode & 0170000) != 0100000) return fail("atroot/file.txt is not a regular file", 10);
    if ((st.st_mode & 0777) != 0640) return fail("atroot/file.txt mode mismatch", 11);
    if (st.st_size != 6) return fail("atroot/file.txt size mismatch", 12);

    fd = openat(AT_FDCWD, "/cwd.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) return fail("openat AT_FDCWD failed", 13);
    if (write(fd, "cwd\n", 4) != 4) {
        close(fd);
        return fail("write /cwd.txt failed", 14);
    }
    close(fd);

    if (fstatat(AT_FDCWD, "/cwd.txt", &st, 0) < 0) return fail("fstatat AT_FDCWD failed", 15);
    if (st.st_size != 4) return fail("/cwd.txt size mismatch", 16);

    if (pipe2(pipefd, O_CLOEXEC) < 0) return fail("pipe2 failed", 17);
    flags = fcntl(pipefd[0], F_GETFD);
    if ((flags & FD_CLOEXEC) == 0) return fail("pipe2 missing FD_CLOEXEC", 18);
    if (write(pipefd[1], "pipe", 4) != 4) return fail("pipe2 write failed", 19);
    memset(buf, 0, sizeof(buf));
    if (read(pipefd[0], buf, sizeof(buf)) != 4) return fail("pipe2 read failed", 20);
    if (strcmp(buf, "pipe") != 0) return fail("pipe2 data mismatch", 21);
    close(pipefd[0]);
    close(pipefd[1]);

    if (unlinkat(dirfd, "atroot/file.txt", 0) < 0) return fail("unlinkat file failed", 22);
    if (unlinkat(AT_FDCWD, "/cwd.txt", 0) < 0) return fail("unlinkat AT_FDCWD failed", 23);
    if (unlinkat(dirfd, "atroot", AT_REMOVEDIR) < 0) return fail("unlinkat dir failed", 24);
    close(dirfd);

    printf("at_test: PASS\n");
    return 0;
}
