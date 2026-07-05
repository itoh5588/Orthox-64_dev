#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

struct fmt_out {
    char* dst;
    size_t size;
    size_t pos;
    size_t total;
};

struct fmt_spec {
    int left;
    int zero;
    int width;
    int long_count;
    char spec;
};

static void fmt_putc(struct fmt_out* out, char c) {
    if (out->dst && out->size && out->pos + 1 < out->size) {
        out->dst[out->pos] = c;
    }
    out->pos++;
    out->total++;
}

static void fmt_repeat(struct fmt_out* out, char c, int count) {
    while (count-- > 0) {
        fmt_putc(out, c);
    }
}

static size_t fmt_strlen(const char* s) {
    size_t n = 0;
    if (!s) s = "(null)";
    while (s[n]) n++;
    return n;
}

static size_t fmt_uint_to_buf(char* dst, size_t dst_size, uint64_t value, unsigned base, int uppercase) {
    char tmp[32];
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    size_t n = 0;

    if (!dst || dst_size == 0 || base < 2 || base > 16) return 0;
    if (value == 0) {
        dst[0] = '0';
        return 1;
    }

    while (value && n < sizeof(tmp)) {
        tmp[n++] = digits[value % base];
        value /= base;
    }
    if (n > dst_size) n = dst_size;
    for (size_t i = 0; i < n; i++) {
        dst[i] = tmp[n - 1U - i];
    }
    return n;
}

static size_t fmt_int_to_buf(char* dst, size_t dst_size, int64_t value, int* negative) {
    uint64_t magnitude;
    if (negative) *negative = 0;
    if (value < 0) {
        if (negative) *negative = 1;
        magnitude = (uint64_t)(-(value + 1)) + 1U;
    } else {
        magnitude = (uint64_t)value;
    }
    return fmt_uint_to_buf(dst, dst_size, magnitude, 10, 0);
}

static void fmt_put_padded(struct fmt_out* out, const char* s, size_t len, const struct fmt_spec* spec, char pad) {
    int width = spec ? spec->width : 0;
    int left = spec ? spec->left : 0;
    int pads = (width > (int)len) ? width - (int)len : 0;

    if (!left) fmt_repeat(out, pad, pads);
    for (size_t i = 0; i < len; i++) {
        fmt_putc(out, s[i]);
    }
    if (left) fmt_repeat(out, ' ', pads);
}

static void fmt_put_string(struct fmt_out* out, const char* s, const struct fmt_spec* spec) {
    if (!s) s = "(null)";
    fmt_put_padded(out, s, fmt_strlen(s), spec, ' ');
}

static void fmt_put_char(struct fmt_out* out, char c, const struct fmt_spec* spec) {
    fmt_put_padded(out, &c, 1, spec, ' ');
}

static void fmt_put_uint_padded(struct fmt_out* out, uint64_t value, unsigned base, int uppercase, const struct fmt_spec* spec) {
    char tmp[32];
    size_t len = fmt_uint_to_buf(tmp, sizeof(tmp), value, base, uppercase);
    char pad = (spec && spec->zero && !spec->left) ? '0' : ' ';
    fmt_put_padded(out, tmp, len, spec, pad);
}

static void fmt_put_int_padded(struct fmt_out* out, int64_t value, const struct fmt_spec* spec) {
    char tmp[32];
    int negative = 0;
    size_t digits = fmt_int_to_buf(tmp, sizeof(tmp), value, &negative);
    int width = spec ? spec->width : 0;
    int left = spec ? spec->left : 0;
    int zero = spec ? spec->zero : 0;
    int len = (int)digits + (negative ? 1 : 0);
    int pads = (width > len) ? width - len : 0;

    if (zero && !left) {
        if (negative) fmt_putc(out, '-');
        fmt_repeat(out, '0', pads);
    } else {
        if (!left) fmt_repeat(out, ' ', pads);
        if (negative) fmt_putc(out, '-');
    }
    for (size_t i = 0; i < digits; i++) {
        fmt_putc(out, tmp[i]);
    }
    if (left) fmt_repeat(out, ' ', pads);
}

static void fmt_put_ptr(struct fmt_out* out, void* ptr, const struct fmt_spec* spec) {
    char tmp[2 + 32];
    size_t len;
    tmp[0] = '0';
    tmp[1] = 'x';
    len = 2 + fmt_uint_to_buf(tmp + 2, sizeof(tmp) - 2, (uint64_t)(uintptr_t)ptr, 16, 0);
    fmt_put_padded(out, tmp, len, spec, (spec && spec->zero && !spec->left) ? '0' : ' ');
}

static void fmt_finish(struct fmt_out* out) {
    if (!out->dst || out->size == 0) return;
    if (out->pos < out->size) {
        out->dst[out->pos] = '\0';
    } else {
        out->dst[out->size - 1] = '\0';
    }
}

static int fmt_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static void fmt_parse_spec(const char** fmtp, struct fmt_spec* spec) {
    const char* fmt = *fmtp;
    *spec = (struct fmt_spec){0, 0, 0, 0, 0};

    for (;;) {
        if (*fmt == '-') {
            spec->left = 1;
            fmt++;
            continue;
        }
        if (*fmt == '0') {
            spec->zero = 1;
            fmt++;
            continue;
        }
        break;
    }

    while (fmt_is_digit(*fmt)) {
        if (spec->width < 1000000) {
            spec->width = spec->width * 10 + (*fmt - '0');
        }
        fmt++;
    }

    while (*fmt == 'l' && spec->long_count < 2) {
        spec->long_count++;
        fmt++;
    }

    spec->spec = *fmt;
    if (*fmt) fmt++;
    *fmtp = fmt;
}

int vsnprintf(char* dst, size_t size, const char* fmt, va_list ap) {
    struct fmt_out out = { dst, size, 0, 0 };

    if (!fmt) {
        fmt_finish(&out);
        return 0;
    }

    while (*fmt) {
        struct fmt_spec spec;

        if (*fmt != '%') {
            fmt_putc(&out, *fmt++);
            continue;
        }

        fmt++;
        if (*fmt == '%') {
            fmt_putc(&out, '%');
            fmt++;
            continue;
        }

        fmt_parse_spec(&fmt, &spec);
        if (!spec.spec) {
            fmt_putc(&out, '%');
            break;
        }

        switch (spec.spec) {
            case 'c':
                fmt_put_char(&out, (char)va_arg(ap, int), &spec);
                break;
            case 's':
                fmt_put_string(&out, va_arg(ap, const char*), &spec);
                break;
            case 'd':
            case 'i':
                if (spec.long_count >= 2) {
                    fmt_put_int_padded(&out, va_arg(ap, long long), &spec);
                } else if (spec.long_count == 1) {
                    fmt_put_int_padded(&out, va_arg(ap, long), &spec);
                } else {
                    fmt_put_int_padded(&out, va_arg(ap, int), &spec);
                }
                break;
            case 'u':
                if (spec.long_count >= 2) {
                    fmt_put_uint_padded(&out, va_arg(ap, unsigned long long), 10, 0, &spec);
                } else if (spec.long_count == 1) {
                    fmt_put_uint_padded(&out, va_arg(ap, unsigned long), 10, 0, &spec);
                } else {
                    fmt_put_uint_padded(&out, va_arg(ap, unsigned int), 10, 0, &spec);
                }
                break;
            case 'x':
            case 'X':
                if (spec.long_count >= 2) {
                    fmt_put_uint_padded(&out, va_arg(ap, unsigned long long), 16, spec.spec == 'X', &spec);
                } else if (spec.long_count == 1) {
                    fmt_put_uint_padded(&out, va_arg(ap, unsigned long), 16, spec.spec == 'X', &spec);
                } else {
                    fmt_put_uint_padded(&out, va_arg(ap, unsigned int), 16, spec.spec == 'X', &spec);
                }
                break;
            case 'p':
                fmt_put_ptr(&out, va_arg(ap, void*), &spec);
                break;
            default:
                fmt_putc(&out, '%');
                if (spec.left) fmt_putc(&out, '-');
                if (spec.zero) fmt_putc(&out, '0');
                if (spec.width > 0) fmt_put_uint_padded(&out, (uint64_t)spec.width, 10, 0, 0);
                for (int i = 0; i < spec.long_count; i++) fmt_putc(&out, 'l');
                fmt_putc(&out, spec.spec);
                break;
        }
    }

    fmt_finish(&out);
    return (int)out.total;
}

int snprintf(char* dst, size_t size, const char* fmt, ...) {
    va_list ap;
    int rc;
    va_start(ap, fmt);
    rc = vsnprintf(dst, size, fmt, ap);
    va_end(ap);
    return rc;
}
