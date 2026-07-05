#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/random.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "syscall.h"
#include "bearssl.h"
#include "https_ta.h"

#define MAX_REDIRECTS 4

struct http_buffer {
    unsigned char *data;
    size_t len;
    size_t cap;
};

struct http_response {
    int status;
    const char *header_end;
    size_t header_len;
    const unsigned char *body;
    size_t body_len;
    int chunked;
    const char *location;
    const char *content_encoding;
};

static void say(const char *s)
{
    write(1, s, strlen(s));
}

static void say_num(const char *label, unsigned long long value)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%s%llu\n", label, value);
    if (n > 0) write(1, buf, (size_t)n);
}

static void say_ssl_error(const char *where, br_ssl_client_context *sc,
    br_x509_minimal_context *xc)
{
    fprintf(stderr, "%s: ssl=%d x509=%d\n", where,
        br_ssl_engine_last_error(&sc->eng), xc ? xc->err : 0);
}

static int parse_ipv4(const char *s, unsigned long *out)
{
    unsigned long parts[4];
    int count = 0;
    const char *p = s;
    if (!s || !out) return -1;
    while (*p && count < 4) {
        char *end = 0;
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

static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    ssize_t rlen = read(fd, buf, len);
    if (rlen < 0) return -1;
    if (rlen == 0) return 0;
    return (int)rlen;
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    ssize_t wlen = write(fd, buf, len);
    if (wlen <= 0) return -1;
    return (int)wlen;
}

static void set_x509_time_now(br_x509_minimal_context *xc)
{
    struct timeval tv;
    uint32_t days;
    uint32_t seconds;
    if (!xc) return;
    if (gettimeofday(&tv, 0) < 0 || tv.tv_sec < 0) {
        br_x509_minimal_set_time(xc, 739330U, 0U);
        return;
    }
    days = 719528U + (uint32_t)((uint64_t)tv.tv_sec / 86400ULL);
    seconds = (uint32_t)((uint64_t)tv.tv_sec % 86400ULL);
    br_x509_minimal_set_time(xc, days, seconds);
}

static int ascii_tolower(int c)
{
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int ascii_strncasecmp(const char *a, const char *b, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i) {
        int ca = ascii_tolower((unsigned char)a[i]);
        int cb = ascii_tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
    return 0;
}

static char *xstrdup(const char *s)
{
    size_t len;
    char *out;
    if (!s) return 0;
    len = strlen(s);
    out = malloc(len + 1);
    if (!out) return 0;
    memcpy(out, s, len + 1);
    return out;
}

static int httpbuf_append(struct http_buffer *buf, const unsigned char *src,
    size_t len)
{
    size_t new_cap;
    unsigned char *new_data;
    if (buf->len + len + 1 <= buf->cap) {
        memcpy(buf->data + buf->len, src, len);
        buf->len += len;
        buf->data[buf->len] = '\0';
        return 0;
    }
    new_cap = buf->cap ? buf->cap * 2 : 4096;
    while (new_cap < buf->len + len + 1) {
        new_cap *= 2;
    }
    new_data = realloc(buf->data, new_cap);
    if (!new_data) return -1;
    buf->data = new_data;
    buf->cap = new_cap;
    memcpy(buf->data + buf->len, src, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return 0;
}

static const char *find_header_end(const unsigned char *buf, size_t len)
{
    size_t i;
    if (!buf) return 0;
    for (i = 0; i + 3 < len; ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n'
            && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return (const char *)(buf + i + 4);
        }
    }
    return 0;
}

static const char *find_line_end(const char *p, const char *end)
{
    while (p < end && *p != '\n') ++p;
    return p;
}

static const char *skip_spaces(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    return p;
}

static const char *dup_value(const char *start, const char *end)
{
    size_t len;
    char *out;
    while (end > start && (end[-1] == '\r' || end[-1] == '\n'
        || end[-1] == ' ' || end[-1] == '\t')) {
        --end;
    }
    len = (size_t)(end - start);
    out = malloc(len + 1);
    if (!out) return 0;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static int parse_http_response(const struct http_buffer *raw,
    struct http_response *resp)
{
    const char *header_end;
    const char *p;
    const char *line_end;
    const char *headers_end;

    memset(resp, 0, sizeof(*resp));
    header_end = find_header_end(raw->data, raw->len);
    if (!header_end) return -1;

    resp->header_end = header_end;
    resp->header_len = (size_t)(header_end - (const char *)raw->data);
    resp->body = (const unsigned char *)header_end;
    resp->body_len = raw->len - resp->header_len;

    p = (const char *)raw->data;
    headers_end = header_end;
    line_end = find_line_end(p, headers_end);
    if (line_end > p + 12 && !ascii_strncasecmp(p, "HTTP/", 5)) {
        const char *sp = strchr(p, ' ');
        if (sp && sp < line_end) resp->status = atoi(sp + 1);
    }

    p = line_end;
    while (p < headers_end) {
        const char *name;
        const char *value;
        const char *colon;
        if (*p == '\n') {
            ++p;
            continue;
        }
        if (*p == '\r') {
            ++p;
            continue;
        }
        line_end = find_line_end(p, headers_end);
        colon = memchr(p, ':', (size_t)(line_end - p));
        if (!colon) {
            p = line_end;
            continue;
        }
        name = p;
        value = skip_spaces(colon + 1, line_end);
        if ((size_t)(colon - name) == 8
            && !ascii_strncasecmp(name, "Location", 8)) {
            resp->location = dup_value(value, line_end);
        } else if ((size_t)(colon - name) == 17
            && !ascii_strncasecmp(name, "Transfer-Encoding", 17)) {
            const char *enc = dup_value(value, line_end);
            if (enc) {
                resp->chunked = strstr(enc, "chunked") != 0
                    || strstr(enc, "Chunked") != 0;
                free((void *)enc);
            }
        } else if ((size_t)(colon - name) == 16
            && !ascii_strncasecmp(name, "Content-Encoding", 16)) {
            resp->content_encoding = dup_value(value, line_end);
        }
        p = line_end;
    }
    return 0;
}

static int hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_chunked(const unsigned char *src, size_t src_len,
    struct http_buffer *out)
{
    size_t pos = 0;
    while (pos < src_len) {
        unsigned long chunk_len = 0;
        int digit_seen = 0;
        int hv;
        while (pos < src_len) {
            unsigned char c = src[pos++];
            if (c == ';') {
                while (pos + 1 < src_len
                    && !(src[pos] == '\r' && src[pos + 1] == '\n')) {
                    ++pos;
                }
                break;
            }
            if (c == '\r') break;
            hv = hex_value(c);
            if (hv < 0) return -1;
            digit_seen = 1;
            chunk_len = (chunk_len << 4) | (unsigned long)hv;
        }
        if (!digit_seen) return -1;
        if (pos >= src_len || src[pos] != '\n') return -1;
        ++pos;
        if (chunk_len == 0) {
            return 0;
        }
        if (pos + chunk_len + 2 > src_len) return -1;
        if (httpbuf_append(out, src + pos, (size_t)chunk_len) < 0) return -1;
        pos += (size_t)chunk_len;
        if (src[pos] != '\r' || src[pos + 1] != '\n') return -1;
        pos += 2;
    }
    return -1;
}

static int parse_location(const char *location, char **out_host, int *out_port,
    char **out_path)
{
    const char *p;
    const char *host_start;
    const char *host_end;
    const char *path_start;
    char *host;
    char *path;
    int port = 443;

    if (!location || !out_host || !out_port || !out_path) return -1;
    if (!strncmp(location, "https://", 8)) {
        p = location + 8;
    } else {
        return -1;
    }

    host_start = p;
    while (*p && *p != '/' && *p != ':') ++p;
    host_end = p;
    if (*p == ':') {
        port = atoi(p + 1);
        while (*p && *p != '/') ++p;
    }
    path_start = *p ? p : "/";
    host = malloc((size_t)(host_end - host_start) + 1);
    if (!host) return -1;
    memcpy(host, host_start, (size_t)(host_end - host_start));
    host[host_end - host_start] = '\0';
    path = xstrdup(path_start);
    if (!path) {
        free(host);
        return -1;
    }
    *out_host = host;
    *out_port = port;
    *out_path = path;
    return 0;
}

static int write_str(br_sslio_context *ioc, const char *s)
{
    return br_sslio_write_all(ioc, s, strlen(s));
}

static int fetch_once(const char *host, int port, const char *path,
    struct http_buffer *raw, int *out_ssl_err, int *out_x509_err)
{
    int fd;
    unsigned long ip = 0;
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    unsigned char seed[64];
    int read_failed = 0;
    struct sockaddr_in addr;

    raw->len = 0;
    if (raw->data) raw->data[0] = '\0';

    if (parse_ipv4(host, &ip) < 0) {
        if (dns_lookup_ipv4(host, (uint32_t *)&ip) < 0) {
            say("httpsfetch: bad host\n");
            return -1;
        }
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        say("httpsfetch: socket failed\n");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = (unsigned short)(((unsigned)port >> 8) | ((unsigned)port << 8));
    addr.sin_addr.s_addr = (in_addr_t)ip;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        say("httpsfetch: connect failed\n");
        close(fd);
        return -1;
    }

    if (getrandom(seed, sizeof(seed), 0) != (ssize_t)sizeof(seed)) {
        say("httpsfetch: getrandom failed\n");
        close(fd);
        return -1;
    }

    br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM);
    set_x509_time_now(&xc);
    br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof(iobuf), 1);
    br_ssl_engine_inject_entropy(&sc.eng, seed, sizeof(seed));

    if (!br_ssl_client_reset(&sc, host, 0)) {
        say("httpsfetch: tls reset failed\n");
        close(fd);
        return -1;
    }

    br_sslio_init(&ioc, &sc.eng, sock_read, &fd, sock_write, &fd);
    write_str(&ioc, "GET ");
    write_str(&ioc, path);
    write_str(&ioc, " HTTP/1.1\r\nHost: ");
    write_str(&ioc, host);
    write_str(&ioc, "\r\nUser-Agent: curl/8.7.1\r\n");
    write_str(&ioc, "Accept: */*\r\n");
    write_str(&ioc, "Connection: close\r\n\r\n");
    if (br_sslio_flush(&ioc) < 0) {
        say("httpsfetch: tls write failed\n");
        say_ssl_error("httpsfetch: flush", &sc, &xc);
        close(fd);
        return -1;
    }

    say("httpsfetch: tls handshake ok\n");
    for (;;) {
        unsigned char tmp[1024];
        int rlen = br_sslio_read(&ioc, tmp, sizeof(tmp));
        if (rlen < 0) {
            read_failed = 1;
            break;
        }
        if (rlen == 0) break;
        if (httpbuf_append(raw, tmp, (size_t)rlen) < 0) {
            say("httpsfetch: out of memory\n");
            close(fd);
            return -1;
        }
    }

    *out_ssl_err = br_ssl_engine_last_error(&sc.eng);
    *out_x509_err = xc.err;
    close(fd);
    if (read_failed && !(*out_ssl_err == 0 && *out_x509_err == BR_ERR_X509_OK)) {
        say_ssl_error("httpsfetch: read", &sc, &xc);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char *host = 0;
    char *path = 0;
    int port = 443;
    int redirects = 0;
    struct http_buffer raw = {0};
    struct http_buffer decoded = {0};
    struct http_response resp;
    int ssl_err = 0;
    int x509_err = 0;

    signal(SIGPIPE, SIG_IGN);

    host = xstrdup(argc >= 2 ? argv[1] : "example.com");
    path = xstrdup(argc >= 4 ? argv[3] : "/");
    if (argc >= 3) {
        port = atoi(argv[2]);
        if (port <= 0 || port > 65535) {
            say("httpsfetch: bad port\n");
            free(host);
            free(path);
            return 1;
        }
    }
    if (!host || !path) {
        say("httpsfetch: out of memory\n");
        free(host);
        free(path);
        return 1;
    }

    for (;;) {
        if (fetch_once(host, port, path, &raw, &ssl_err, &x509_err) < 0) {
            free(raw.data);
            free(decoded.data);
            free(host);
            free(path);
            return 1;
        }
        if (parse_http_response(&raw, &resp) < 0) {
            say("httpsfetch: malformed HTTP response\n");
            free(raw.data);
            free(decoded.data);
            free(host);
            free(path);
            return 1;
        }

        if ((resp.status == 301 || resp.status == 302
            || resp.status == 303 || resp.status == 307 || resp.status == 308)
            && resp.location && redirects < MAX_REDIRECTS) {
            char *new_host = 0;
            char *new_path = 0;
            int new_port = 443;
            if (parse_location(resp.location, &new_host, &new_port, &new_path) == 0) {
                say("httpsfetch: redirect follow\n");
                free((void *)resp.location);
                free((void *)resp.content_encoding);
                free(host);
                free(path);
                host = new_host;
                path = new_path;
                port = new_port;
                ++redirects;
                continue;
            }
        }
        break;
    }

    say("httpsfetch: response headers\n");
    write(1, raw.data, resp.header_len);

    if (resp.chunked) {
        decoded.len = 0;
        if (decoded.data) decoded.data[0] = '\0';
        if (decode_chunked(resp.body, resp.body_len, &decoded) == 0) {
            resp.body = decoded.data;
            resp.body_len = decoded.len;
        } else {
            say("httpsfetch: chunked decode failed\n");
        }
    }

    if (resp.content_encoding && ascii_strncasecmp(resp.content_encoding, "identity", 8) != 0) {
        say("\nhttpsfetch: response body\n");
        say("[content-encoding not yet decoded]\n");
        write(1, resp.body, resp.body_len);
        if (resp.body_len && resp.body[resp.body_len - 1] != '\n') say("\n");
    } else {
        say("\nhttpsfetch: response body\n");
        if (resp.body_len) {
            write(1, resp.body, resp.body_len);
            if (resp.body[resp.body_len - 1] != '\n') say("\n");
        }
    }

    say_num("httpsfetch: body bytes=", (unsigned long long)resp.body_len);
    say("httpsfetch: response end\n");

    free((void *)resp.location);
    free((void *)resp.content_encoding);
    free(raw.data);
    free(decoded.data);
    free(host);
    free(path);
    (void)ssl_err;
    (void)x509_err;
    return 0;
}
