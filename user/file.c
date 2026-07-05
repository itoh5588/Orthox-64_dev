#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int is_text(const unsigned char *buf, ssize_t n) {
    ssize_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (c == '\0') {
            return 0;
        }
        if (c == '\n' || c == '\r' || c == '\t' || c == '\b' || c == '\f') {
            continue;
        }
        if (!isprint(c)) {
            return 0;
        }
    }
    return 1;
}

static const char *elf_class(unsigned char c) {
    switch (c) {
    case 1:
        return "ELF 32-bit";
    case 2:
        return "ELF 64-bit";
    default:
        return "ELF";
    }
}

static const char *elf_data(unsigned char c) {
    switch (c) {
    case 1:
        return "LSB";
    case 2:
        return "MSB";
    default:
        return "unknown data";
    }
}

static int describe(const char *path) {
    unsigned char buf[512];
    struct stat st;
    ssize_t n;
    int fd;

    if (stat(path, &st) < 0) {
        printf("%s: cannot stat: %s\n", path, strerror(errno));
        return 1;
    }

    if ((st.st_mode & 0170000) == 0040000) {
        printf("%s: directory\n", path);
        return 0;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("%s: cannot open: %s\n", path, strerror(errno));
        return 1;
    }

    n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n < 0) {
        printf("%s: cannot read: %s\n", path, strerror(errno));
        return 1;
    }

    if (n == 0) {
        printf("%s: empty\n", path);
        return 0;
    }

    if (n >= 5 && buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F') {
        printf("%s: %s %s executable\n", path, elf_class(buf[4]), elf_data(n >= 6 ? buf[5] : 0));
        return 0;
    }

    if (is_text(buf, n)) {
        printf("%s: ASCII text\n", path);
        return 0;
    }

    printf("%s: data\n", path);
    return 0;
}

int main(int argc, char **argv) {
    int rc = 0;
    int i;

    if (argc < 2) {
        fprintf(stderr, "Usage: file FILE...\n");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        if (describe(argv[i]) != 0) {
            rc = 1;
        }
    }
    return rc;
}
