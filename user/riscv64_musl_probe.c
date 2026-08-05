#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* clone(SIGCHLD, 0) を **a2 以降にゴミを載せた状態で** 直接発行する。
 *
 * musl の _Fork.c は __syscall(SYS_clone, SIGCHLD, 0) と引数を 2 つしか渡さず、
 * a2 は直前の呼び出しが残した値になる。カーネルが a2 == 0 を条件に入れていると
 * 「たまたま a2 が 0 だったビルドでだけ fork が通る」という状態になり、
 * コンパイラを替えた瞬間に ENOSYS で落ちる (実際に踏んだ)。
 * ここでゴミを明示的に載せて、条件に入っていないことを決定的に確かめる。
 *
 * 戻り値: 親では子の pid、子では 0、失敗なら負の errno。
 */
static long raw_clone_with_garbage(void) {
    register long a0 __asm__("a0") = 17;                    /* SIGCHLD */
    register long a1 __asm__("a1") = 0;                     /* 子スタック = fork 相当 */
    register long a2 __asm__("a2") = 0x5a5a5a5a;            /* parent_tid: 見てはいけない */
    register long a3 __asm__("a3") = 0x3c3c3c3c;            /* tls: 同上 */
    register long a4 __asm__("a4") = 0x7e7e7e7e;            /* child_tid: 同上 */
    register long a7 __asm__("a7") = 220;                   /* SYS_clone */
    __asm__ __volatile__("ecall"
                         : "+r"(a0)
                         : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a7)
                         : "memory");
    return a0;
}

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

    /* a2 以降にゴミを載せた clone が通ること (詳細は raw_clone_with_garbage) */
    {
        long r = raw_clone_with_garbage();
        if (r == 0) {
            _exit(write_all(1, "CLONEG-CHILD\n", 13) < 0 ? 28 : 0);
        }
        if (r < 0) return 29;
        if (waitpid((pid_t)r, &status, 0) < 0) return 30;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 31;
        if (write_all(1, "CLONEG-OK\n", 10) < 0) return 32;
    }

    /* 1 回の write で xv6fs のログ容量 (126 ブロック = 126KB) を超える。
     *
     * xv6fs_write_file が書き込みを分割していないと、この 1 回で
     *   KASSERT(lg.lh.n < XV6FS_LOGBLOCKS)  @ xv6log_write
     * に当たってカーネルパニックする。Orthox 上の gcc が .o を書いた瞬間に
     * 落ちたのがこれで、x86 (kernel/fs.c) では元から分割されていた。
     * xv6fs が無い構成では書けないので、その場合は静かに飛ばす。 */
    {
        static char big[200 * 1024];
        int wfd = open("/tmp/bigwrite.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (wfd >= 0) {
            ssize_t n;
            memset(big, 'B', sizeof(big));
            n = write(wfd, big, sizeof(big));
            close(wfd);
            if (n != (ssize_t)sizeof(big)) return 33;
            if (write_all(1, "BIGWRITE-OK\n", 12) < 0) return 34;
            unlink("/tmp/bigwrite.bin");
        }
    }
    return 0;
}
