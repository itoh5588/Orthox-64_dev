#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

static void write_str(const char* s) {
    size_t len = 0;
    while (s[len] != '\0') len++;
    (void)write(STDOUT_FILENO, s, len);
}

static void write_dec(int value) {
    char buf[16];
    int i = 0;
    unsigned int u = (value < 0) ? (unsigned int)(-value) : (unsigned int)value;

    if (value < 0) {
        buf[i++] = '-';
    }

    if (u == 0) {
        buf[i++] = '0';
    } else {
        char tmp[16];
        int j = 0;
        while (u > 0) {
            tmp[j++] = (char)('0' + (u % 10));
            u /= 10;
        }
        while (j > 0) {
            buf[i++] = tmp[--j];
        }
    }

    (void)write(STDOUT_FILENO, buf, (size_t)i);
}

static void write_hex_byte(unsigned char value) {
    static const char hex[] = "0123456789ABCDEF";
    char out[2];
    out[0] = hex[(value >> 4) & 0xF];
    out[1] = hex[value & 0xF];
    (void)write(STDOUT_FILENO, out, sizeof(out));
}

static void dump_bytes(const unsigned char* buf, int n) {
    for (int i = 0; i < n; i++) {
        write_hex_byte(buf[i]);
        if (i + 1 < n) write_str(" ");
    }
    write_str("\n");
}

int main(void) {
    unsigned char hdr[16];
    int fd = open("doom1.wad", O_RDONLY);
    int n;

    write_str("wadheadtest: open doom1.wad\n");
    if (fd < 0) {
        write_str("open failed\n");
        return 1;
    }

    n = (int)read(fd, hdr, sizeof(hdr));
    write_str("read0=");
    write_dec(n);
    write_str("\n");
    if (n > 0) dump_bytes(hdr, n);

    if (lseek(fd, 0, SEEK_SET) < 0) {
        write_str("lseek0 failed\n");
        close(fd);
        return 2;
    }

    n = (int)read(fd, hdr, 4);
    write_str("read1=");
    write_dec(n);
    write_str("\n");
    if (n > 0) dump_bytes(hdr, n);

    if (lseek(fd, 8, SEEK_SET) < 0) {
        write_str("lseek8 failed\n");
        close(fd);
        return 3;
    }

    n = (int)read(fd, hdr, 8);
    write_str("read2=");
    write_dec(n);
    write_str("\n");
    if (n > 0) dump_bytes(hdr, n);

    close(fd);
    write_str("wadheadtest: PASS\n");
    return 0;
}
