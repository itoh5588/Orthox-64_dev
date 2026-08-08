/*
 * dup / fork で複製した fd が offset を共有しているかの検証プローブ
 * (musl 静的リンク。riscv64 用だが中身は POSIX だけなので x86 でも同じ)
 *
 * Linux では dup / fork で作った fd は同じ open file description を指すので、
 * 片方で読み書きするともう片方の offset も進む。offset が fd ごとの写しに
 * なっていると、この性質が崩れる。
 *
 *   riscv64  kernel/riscv64/fs.c:1070 fs_clone_fd() が *dst = *src で丸ごと
 *            複製する。offset は fd ごとに独立したまま
 *   x86      kernel/fs.c:446 fs_clone_fd() が fd->file (fs_file_t) の参照を
 *            増やすだけ。offset は fs_file_t 側にあるので共有される
 *
 * 日報2026-08-03 の残件 2 (B 案) がどこまで出来ているかを実測するためのもの。
 * 読解だけで判断しないこと (日報2026-08-06 の教訓)。
 *
 * printf は long double 経路 (__divtf3) を踏むので使わず、write と自前の
 * 10 進変換だけで報告する (riscv64_errno_probe.c と同じ作法)。
 */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_bad;

static int write_all(int fd, const char* buf, size_t len) {
    while (len > 0) {
        ssize_t written = write(fd, buf, len);
        if (written <= 0) return -1;
        buf += (size_t)written;
        len -= (size_t)written;
    }
    return 0;
}

static void put_str(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    (void)write_all(1, s, n);
}

static void put_int(long v) {
    char buf[24];
    int i = (int)sizeof(buf);
    unsigned long u = (unsigned long)(v < 0 ? -v : v);
    buf[--i] = '\0';
    do {
        buf[--i] = (char)('0' + (unsigned)(u % 10UL));
        u /= 10UL;
    } while (u > 0);
    if (v < 0) buf[--i] = '-';
    put_str(&buf[i]);
}

static void report(const char* name, long got, long want) {
    put_str("OFFS ");
    put_str(name);
    put_str(" got=");
    put_int(got);
    put_str(" want=");
    put_int(want);
    if (got == want) {
        put_str(" ok\n");
    } else {
        put_str(" BAD\n");
        g_bad++;
    }
}

/* Orthox は / が書けるのでそこに置く。ホスト Linux で期待値そのものを
 * 検かめたいときは -DTMPPATH='"/tmp/offsprobe.tmp"' で差し替える */
#ifndef TMPPATH
#define TMPPATH "/offsprobe.tmp"
#endif

/* 1. dup した fd で書き、元の fd の offset が進んでいるか */
static void case_dup_write(void) {
    int a, b;
    long off;

    a = open(TMPPATH, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (a < 0) { put_str("OFFS dup-write OPEN-FAIL\n"); g_bad++; return; }
    b = dup(a);
    if (b < 0) { put_str("OFFS dup-write DUP-FAIL\n"); g_bad++; close(a); return; }

    if (write(b, "0123456789", 10) != 10) {
        put_str("OFFS dup-write WRITE-FAIL\n"); g_bad++;
    } else {
        off = (long)lseek(a, 0, SEEK_CUR);
        report("dup-write", off, 10);
    }
    close(b);
    close(a);
}

/* 2. dup した fd で読み、元の fd の offset が進んでいるか。
 *    binutils の ar が踏むのはこの形 (日報2026-08-03) */
static void case_dup_read(void) {
    int a, b;
    long off;
    char buf[8];

    a = open(TMPPATH, O_RDONLY);
    if (a < 0) { put_str("OFFS dup-read OPEN-FAIL\n"); g_bad++; return; }
    b = dup(a);
    if (b < 0) { put_str("OFFS dup-read DUP-FAIL\n"); g_bad++; close(a); return; }

    if (read(b, buf, 4) != 4) {
        put_str("OFFS dup-read READ-FAIL\n"); g_bad++;
    } else {
        off = (long)lseek(a, 0, SEEK_CUR);
        report("dup-read", off, 4);
    }
    close(b);
    close(a);
}

/* 3. 片方で lseek したら、もう片方から見た位置も動くか */
static void case_dup_lseek(void) {
    int a, b;
    long off;

    a = open(TMPPATH, O_RDONLY);
    if (a < 0) { put_str("OFFS dup-lseek OPEN-FAIL\n"); g_bad++; return; }
    b = dup(a);
    if (b < 0) { put_str("OFFS dup-lseek DUP-FAIL\n"); g_bad++; close(a); return; }

    if (lseek(b, 7, SEEK_SET) != 7) {
        put_str("OFFS dup-lseek SEEK-FAIL\n"); g_bad++;
    } else {
        off = (long)lseek(a, 0, SEEK_CUR);
        report("dup-lseek", off, 7);
    }
    close(b);
    close(a);
}

/* 4. fork した子が読んだら、親の offset も進んでいるか。
 *    シェルが `{ read a; read b; } < f` を子で回す形に相当する */
static void case_fork_read(void) {
    int a;
    long off;
    pid_t pid;
    int status = 0;
    char buf[8];

    a = open(TMPPATH, O_RDONLY);
    if (a < 0) { put_str("OFFS fork-read OPEN-FAIL\n"); g_bad++; return; }

    pid = fork();
    if (pid < 0) { put_str("OFFS fork-read FORK-FAIL\n"); g_bad++; close(a); return; }
    if (pid == 0) {
        if (read(a, buf, 5) != 5) _exit(1);
        _exit(0);
    }
    if (waitpid(pid, &status, 0) < 0) {
        put_str("OFFS fork-read WAIT-FAIL\n"); g_bad++; close(a); return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        put_str("OFFS fork-read CHILD-FAIL\n"); g_bad++; close(a); return;
    }
    off = (long)lseek(a, 0, SEEK_CUR);
    report("fork-read", off, 5);
    close(a);
}

/* 5. 別々に open した 2 本は独立していること (共有しすぎの検出) */
static void case_separate_open(void) {
    int a, b;
    long off;
    char buf[8];

    a = open(TMPPATH, O_RDONLY);
    b = open(TMPPATH, O_RDONLY);
    if (a < 0 || b < 0) {
        put_str("OFFS separate-open OPEN-FAIL\n"); g_bad++;
        if (a >= 0) close(a);
        if (b >= 0) close(b);
        return;
    }
    if (read(b, buf, 6) != 6) {
        put_str("OFFS separate-open READ-FAIL\n"); g_bad++;
    } else {
        off = (long)lseek(a, 0, SEEK_CUR);
        report("separate-open", off, 0);
    }
    close(b);
    close(a);
}

int main(void) {
    put_str("OFFS-PROBE-START\n");

    case_dup_write();
    case_dup_read();
    case_dup_lseek();
    case_fork_read();
    case_separate_open();

    unlink(TMPPATH);

    if (g_bad == 0) {
        put_str("OFFS-PROBE-OK\n");
        return 0;
    }
    put_str("OFFS-PROBE-BAD ");
    put_int(g_bad);
    put_str("\n");
    return 1;
}
