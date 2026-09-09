#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
int main(int argc, char** argv) {
    if (argc < 4) return 1;
    printf("  [cc1] Compiling %s to %s...\n", argv[1], argv[3]);
    int fd = open(argv[3], 0x41);
    if (fd >= 0) {
        const char* s = "MOV RAX, 42\nRET\n";
        write(fd, s, strlen(s));
        close(fd);
    }
    return 0;
}
