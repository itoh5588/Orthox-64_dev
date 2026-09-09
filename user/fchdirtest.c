#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    char cwd[128];
    int fd;

    printf("--- fchdir Test ---\n");
    if (!getcwd(cwd, sizeof(cwd))) {
        printf("getcwd initial failed\n");
        return 1;
    }
    printf("initial cwd=%s\n", cwd);

    fd = open("/bin", O_DIRECTORY | O_RDONLY);
    if (fd < 0) {
        printf("open /bin failed\n");
        return 2;
    }
    if (fchdir(fd) < 0) {
        printf("fchdir /bin failed\n");
        close(fd);
        return 3;
    }
    close(fd);

    if (!getcwd(cwd, sizeof(cwd))) {
        printf("getcwd after fchdir failed\n");
        return 4;
    }
    printf("cwd after fchdir=%s\n", cwd);
    if (cwd[0] != '/' || cwd[1] != 'b' || cwd[2] != 'i' || cwd[3] != 'n' || cwd[4] != '\0') {
        printf("unexpected cwd after fchdir\n");
        return 5;
    }

    fd = open("/", O_DIRECTORY | O_RDONLY);
    if (fd < 0) {
        printf("open / failed\n");
        return 6;
    }
    if (fchdir(fd) < 0) {
        printf("fchdir / failed\n");
        close(fd);
        return 7;
    }
    close(fd);

    if (!getcwd(cwd, sizeof(cwd))) {
        printf("getcwd after root fchdir failed\n");
        return 8;
    }
    printf("cwd after root=%s\n", cwd);
    if (cwd[0] != '/' || cwd[1] != '\0') {
        printf("unexpected cwd after root fchdir\n");
        return 9;
    }

    printf("fchdirtest: PASS\n");
    return 0;
}
