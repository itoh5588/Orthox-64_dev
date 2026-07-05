#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_all(int fd, const char* buf, size_t len) {
    while (len > 0) {
        ssize_t written = write(fd, buf, len);
        if (written <= 0) return -1;
        buf += (size_t)written;
        len -= (size_t)written;
    }
    return 0;
}

int main(void) {
    char cwd[64];
    char hdr[4];
    struct stat st;
    int fd;
    void* map;
    int status = 0;
    pid_t pid;

    if (!getcwd(cwd, sizeof(cwd))) return 10;
    if (write_all(1, "MUSL:", 5) < 0) return 11;
    if (write_all(1, cwd, strlen(cwd)) < 0) return 12;
    if (write_all(1, "\n", 1) < 0) return 13;

    fd = open("/bootstrap-user", O_RDONLY);
    if (fd < 0) return 14;
    if (fstat(fd, &st) < 0) return 15;
    if (read(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) return 16;
    if (memcmp(hdr, "\x7f""ELF", 4) != 0) return 17;
    if (write_all(1, "ELF\n", 4) < 0) return 18;
    if (close(fd) < 0) return 19;

    map = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) return 20;
    memcpy(map, "OK", 2);
    if (memcmp(map, "OK", 2) != 0) return 21;
    if (write_all(1, "MAP\n", 4) < 0) return 22;

    pid = fork();
    if (pid < 0) return 23;
    if (pid == 0) {
        _exit(write_all(1, "CHILD\n", 6) < 0 ? 24 : 0);
    }
    if (waitpid(pid, &status, 0) < 0) return 25;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 26;
    if (write_all(1, "DONE\n", 5) < 0) return 27;
    return 0;
}
