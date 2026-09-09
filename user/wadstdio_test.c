#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void write_str(const char* s) {
    size_t len = 0;
    while (s[len] != '\0') len++;
    (void)write(STDOUT_FILENO, s, len);
}

static void write_dec(int value) {
    char buf[16];
    char tmp[16];
    int i = 0;
    int j = 0;
    unsigned int u;

    if (value < 0) {
        buf[i++] = '-';
        u = (unsigned int)(-value);
    } else {
        u = (unsigned int)value;
    }

    if (u == 0) {
        buf[i++] = '0';
    } else {
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

static void dump_bytes(const unsigned char* buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        write_hex_byte(buf[i]);
        if (i + 1 < n) write_str(" ");
    }
    write_str("\n");
}

int main(void) {
    unsigned char hdr[16];
    FILE* fp;
    size_t n;

    write_str("wadstdio: fopen doom1.wad\n");
    fp = fopen("doom1.wad", "rb");
    if (!fp) {
        write_str("fopen failed\n");
        return 1;
    }

    n = fread(hdr, 1, sizeof(hdr), fp);
    write_str("fread0=");
    write_dec((int)n);
    write_str("\n");
    dump_bytes(hdr, n);

    write_str("strncmp(IWAD)=");
    write_dec(strncmp((const char*)hdr, "IWAD", 4));
    write_str("\n");

    if (fseek(fp, 0, SEEK_SET) != 0) {
        write_str("fseek0 failed\n");
        fclose(fp);
        return 2;
    }

    n = fread(hdr, 1, 4, fp);
    write_str("fread1=");
    write_dec((int)n);
    write_str("\n");
    dump_bytes(hdr, n);

    if (fseek(fp, 8, SEEK_SET) != 0) {
        write_str("fseek8 failed\n");
        fclose(fp);
        return 3;
    }

    n = fread(hdr, 1, 8, fp);
    write_str("fread2=");
    write_dec((int)n);
    write_str("\n");
    dump_bytes(hdr, n);

    fclose(fp);
    write_str("wadstdio: PASS\n");
    return 0;
}
