#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char buf[64];
    int dupfd;
    ssize_t n;

    printf("--- tty/readlink Test ---\n");
    if (!isatty(0) || !isatty(1) || !isatty(2)) {
        printf("standard fds are not tty\n");
        return 1;
    }

    dupfd = dup(1);
    if (dupfd < 0) {
        printf("dup stdout failed\n");
        return 2;
    }
    if (!isatty(dupfd)) {
        printf("dup stdout not tty\n");
        close(dupfd);
        return 3;
    }
    close(dupfd);

    n = readlink("/dev/stdout", buf, sizeof(buf));
    if (n <= 0) {
        printf("readlink /dev/stdout failed\n");
        return 4;
    }
    if ((size_t)n >= sizeof(buf)) {
        printf("readlink /dev/stdout overflow\n");
        return 5;
    }
    buf[n] = '\0';
    printf("/dev/stdout -> %s\n", buf);
    if (strcmp(buf, "/dev/tty") != 0) {
        printf("unexpected stdout link target\n");
        return 6;
    }

    n = readlink("/dev/fd/0", buf, sizeof(buf));
    if (n <= 0) {
        printf("readlink /dev/fd/0 failed\n");
        return 7;
    }
    if ((size_t)n >= sizeof(buf)) {
        printf("readlink /dev/fd/0 overflow\n");
        return 8;
    }
    buf[n] = '\0';
    printf("/dev/fd/0 -> %s\n", buf);
    if (strcmp(buf, "/dev/tty") != 0) {
        printf("unexpected fd0 link target\n");
        return 9;
    }

    printf("ttylinktest: PASS\n");
    return 0;
}
