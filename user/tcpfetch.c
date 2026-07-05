#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

static void say(const char* s) {
    write(1, s, strlen(s));
}

static int parse_ipv4(const char* s, unsigned long* out) {
    unsigned long parts[4];
    int count = 0;
    const char* p = s;
    if (!s || !out) return -1;
    while (*p && count < 4) {
        char* end = 0;
        unsigned long v = strtoul(p, &end, 10);
        if (end == p || v > 255) return -1;
        parts[count++] = v;
        if (*end == '.') p = end + 1;
        else if (*end == '\0') p = end;
        else return -1;
    }
    if (count != 4 || *p != '\0') return -1;
    *out = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    return 0;
}

int main(int argc, char** argv) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    unsigned long ip = 0x22D8B85DUL; /* 93.184.216.34 */
    int port = 80;
    const char* host = "example.com";
    const char* path = "/";
    if (fd < 0) {
        say("tcpfetch: socket failed\n");
        return 1;
    }

    if (argc >= 2) {
        if (parse_ipv4(argv[1], &ip) < 0) {
            if (dns_lookup_ipv4(argv[1], (uint32_t*)&ip) < 0) {
                say("tcpfetch: bad host\n");
                close(fd);
                return 1;
            }
            host = argv[1];
        }
    }
    if (argc >= 3) {
        port = atoi(argv[2]);
        if (port <= 0 || port > 65535) {
            say("tcpfetch: bad port\n");
            close(fd);
            return 1;
        }
    }
    if (argc >= 4) host = argv[3];
    if (argc >= 5) path = argv[4];

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = (unsigned short)(((unsigned)port >> 8) | ((unsigned)port << 8));
    addr.sin_addr.s_addr = (in_addr_t)ip;

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        say("tcpfetch: connect failed\n");
        close(fd);
        return 1;
    }

    char req[512];
    int req_len = 0;
    req_len += snprintf(req + req_len, sizeof(req) - (size_t)req_len,
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);
    if (req_len <= 0 || (size_t)req_len >= sizeof(req)) {
        say("tcpfetch: request too long\n");
        close(fd);
        return 1;
    }

    if (write(fd, req, (size_t)req_len) < 0) {
        say("tcpfetch: write failed\n");
        close(fd);
        return 1;
    }

    say("tcpfetch: response begin\n");
    for (;;) {
        char buf[256];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            say("tcpfetch: read failed\n");
            close(fd);
            return 1;
        }
        if (n == 0) break;
        write(1, buf, (size_t)n);
    }
    say("\ntcpfetch: response end\n");
    close(fd);
    return 0;
}
