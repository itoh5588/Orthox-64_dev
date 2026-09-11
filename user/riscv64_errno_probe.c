/*
 * riscv64 の errno 検証プローブ (musl 静的リンク)
 *
 * カーネルが「よく分からない失敗はとりあえず -1」を返していると、Linux の
 * errno 規約では -1 = EPERM なので、ユーザーランドには
 * "Operation not permitted" として出てくる。busybox は ENOENT を見て挙動を
 * 変える箇所が多く (`rm -f` が典型)、EPERM だと誤動作する。
 *
 * 失敗するはずの呼び出しを並べて、返る errno を 1 行ずつ報告する。
 * 期待どおりなら OK、違えば BAD を出す。printf は long double 経路
 * (__divtf3) を踏むので使わず、write と自前の 10 進変換だけで報告する。
 */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
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

static void put_int(int v) {
    char buf[16];
    int i = (int)sizeof(buf);
    unsigned u = (unsigned)(v < 0 ? -v : v);
    buf[--i] = '\0';
    do {
        buf[--i] = (char)('0' + (u % 10U));
        u /= 10U;
    } while (u > 0);
    if (v < 0) buf[--i] = '-';
    put_str(&buf[i]);
}

/* ret は失敗を表す戻り値 (負 or -1)。want は期待する errno */
static void check(const char* name, int failed, int got, int want) {
    put_str("ERRNO ");
    put_str(name);
    put_str(" got=");
    put_int(got);
    put_str(" want=");
    put_int(want);
    if (!failed) {
        put_str(" BAD(not-failed)\n");
        g_bad++;
    } else if (got != want) {
        put_str(" BAD\n");
        g_bad++;
    } else {
        put_str(" ok\n");
    }
}

int main(void) {
    struct stat st;
    char buf[8];
    int fd;

    put_str("ERRNO-PROBE-START\n");

    errno = 0;
    fd = open("/no-such-file", O_RDONLY);
    check("open-missing", fd < 0, errno, ENOENT);

    errno = 0;
    check("stat-missing", stat("/no-such-file", &st) < 0, errno, ENOENT);

    errno = 0;
    check("unlink-missing", unlink("/no-such-file") < 0, errno, ENOENT);

    errno = 0;
    check("rmdir-missing", rmdir("/no-such-dir") < 0, errno, ENOENT);

    errno = 0;
    check("chdir-missing", chdir("/no-such-dir") < 0, errno, ENOENT);

    errno = 0;
    check("mkdir-existing", mkdir("/etc", 0755) < 0, errno, EEXIST);

    errno = 0;
    check("mkdir-no-parent", mkdir("/no-such-dir/x", 0755) < 0, errno, ENOENT);

    errno = 0;
    check("read-badfd", read(99, buf, sizeof(buf)) < 0, errno, EBADF);

    errno = 0;
    check("write-badfd", write(99, "x", 1) < 0, errno, EBADF);

    errno = 0;
    check("close-badfd", close(99) < 0, errno, EBADF);

    errno = 0;
    check("lseek-badfd", lseek(99, 0, SEEK_SET) < 0, errno, EBADF);

    errno = 0;
    check("fstat-badfd", fstat(99, &st) < 0, errno, EBADF);

    errno = 0;
    check("unlink-dir", unlink("/etc") < 0, errno, EISDIR);

    /* ここから下は kernel/riscv64/syscall.c 側 (fs.c 以外) の失敗経路。
     * mmap は「戻り値そのものが -errno」なので -1 を返すと MAP_FAILED では
     * なく EPERM になる。wait4 の ECHILD は ash のジョブ回収が見ている */

    errno = 0;
    check("wait-nochild", wait(NULL) < 0, errno, ECHILD);

    errno = 0;
    check("mmap-zero-len",
          mmap(NULL, 0, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED,
          errno, EINVAL);

    errno = 0;
    /* 無名マップ以外は未対応 (fd を渡した形) */
    check("mmap-with-fd",
          mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, 0, 0) == MAP_FAILED,
          errno, EINVAL);

    errno = 0;
    /* ページ境界でない addr */
    check("munmap-misaligned", munmap((void*)0x1001, 4096) < 0, errno, EINVAL);

    errno = 0;
    check("munmap-zero-len", munmap((void*)0x2000, 0) < 0, errno, EINVAL);

    errno = 0;
    /* カーネルの先頭 (scripts/kernel-riscv64.ld の 0x80200000)。riscv64 は
     * カーネルが下位半分に居て、ユーザーの表は下の段をカーネルと共有している。
     * **範囲を見ない munmap はここを外せてしまい、全プロセスからカーネルの
     * .text が消える** (このページには uart の出力関数が載っているので、
     * 次の write で止まる) */
    check("munmap-kernel", munmap((void*)0x80200000, 4096) < 0, errno, EINVAL);

    /* 逆向き: **mmap で取った範囲は外せること。**munmap-kernel は範囲検査で
     * 断っているので、その検査が mmap の返す番地まで断っていないかを見る
     * (失敗経路だけ並べると、全部断る実装でも緑になる) */
    {
        volatile char* p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int r = -1;
        if (p != MAP_FAILED) {
            p[0] = 1;
            errno = 0;
            r = munmap((void*)p, 4096);
        }
        put_str("ERRNO munmap-own ret=");
        put_int(r);
        put_str(" errno=");
        put_int(errno);
        if (r == 0) {
            put_str(" ok\n");
        } else {
            put_str(" BAD\n");
            g_bad++;
        }
    }

    /* RAM の上端近く (DTB の 0x9fe00000 より手前、pmm は下から配るので空き)。
     * riscv64 のカーネルは RAM 全体を下位半分に恒等写像しているので、
     * **ユーザーの写像かどうかを見ない mprotect は、ここをユーザーに見える
     * ページとして貼り直せてしまう。**Linux / x86 と同じく ENOMEM を期待する。
     * 通ってしまったら 1 バイト読んで、本当に読めることを報告する
     * (書くと何かを壊しうるので読むだけ) */
    {
        volatile unsigned char* kp = (volatile unsigned char*)0x9f000000UL;
        int r;
        errno = 0;
        r = mprotect((void*)kp, 4096, PROT_READ);
        if (r == 0) {
            put_str("ERRNO mprotect-kernel-ram readable byte=");
            put_int(kp[0]);
            put_str("\n");
        }
        check("mprotect-kernel-ram", r < 0, errno, ENOMEM);
    }

    /* 逆向き: mmap で取ったページは mprotect できること (munmap-own と同じ理由) */
    {
        void* p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int r = -1;
        int e = 0;
        if (p != MAP_FAILED) {
            errno = 0;
            r = mprotect(p, 4096, PROT_READ);
            e = errno;
            (void)munmap(p, 4096);
        }
        put_str("ERRNO mprotect-own ret=");
        put_int(r);
        put_str(" errno=");
        put_int(e);
        if (r == 0) {
            put_str(" ok\n");
        } else {
            put_str(" BAD\n");
            g_bad++;
        }
    }

    errno = 0;
    {
        struct timespec ts;
        check("clock_gettime-badclock", clock_gettime(99, &ts) < 0, errno, EINVAL);
    }

    errno = 0;
    /* コンソール以外の ioctl は無い。ENOTTY でないと musl/busybox が
     * 「tty ではない」と判断できない */
    check("ioctl-unknown", ioctl(1, 0x1234, 0) < 0, errno, ENOTTY);

    errno = 0;
    check("lseek-bad-whence", lseek(1, 0, 99) < 0, errno, EINVAL);

    if (g_bad == 0) {
        put_str("ERRNO-PROBE-OK\n");
        return 0;
    }
    put_str("ERRNO-PROBE-BAD ");
    put_int(g_bad);
    put_str("\n");
    return 1;
}
