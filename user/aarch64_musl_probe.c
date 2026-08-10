/*
 * P2: musl 静的リンクプログラムが aarch64 の EL0 で完走するかの検査。
 *
 * riscv64 版 (user/riscv64_musl_probe.c) から **fork / clone を外した**もの。
 * aarch64 は arch_vm_clone_address_space と aarch64_task_fork_child_return が
 * まだ未実装なので、入れれば必ず落ちる。fork は P3 の領分。
 *
 * 逆に BIGWRITE / DUPRW は残す。**どちらも共有コード (kernel/fs.c /
 * xv6fs / sys_fs.c) の退行検査**で、riscv64 がセルフホストで踏んだ穴を
 * aarch64 でも踏まないことを見るため。
 *
 * カーネルに埋め込まず、ディスクの /bin/musl-probe を exec して走る (P1 と
 * 同じ方針)。自分自身を open できることが「ELF の読み込み経路が生きている」
 * ことの確認になる。
 */
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* スモークがイメージに入れる場所。カーネルが exec するパスと同じ */
#define SELF_PATH "/bin/musl-probe"

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

    /* getcwd が通る = カーネルとの往復と musl の内部状態が両方生きている。
     * **libc を通した最初の syscall** なので、ここで落ちたら crt0 か
     * __libc_start_main の側を疑う */
    if (!getcwd(cwd, sizeof(cwd))) return 10;
    if (write_all(1, "MUSL:", 5) < 0) return 11;
    if (write_all(1, cwd, strlen(cwd)) < 0) return 12;
    if (write_all(1, "\n", 1) < 0) return 13;

    /* 自分自身を開いて ELF マジックまで照合する。**開けただけでは
     * 別のブロックを返していても気づけない** */
    fd = open(SELF_PATH, O_RDONLY);
    if (fd < 0) return 14;
    if (fstat(fd, &st) < 0) return 15;
    if (st.st_size <= 0) return 16;
    if (read(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) return 17;
    if (memcmp(hdr, "\x7f""ELF", 4) != 0) return 18;
    if (write_all(1, "ELF\n", 4) < 0) return 19;
    if (close(fd) < 0) return 20;

    /* 匿名 mmap。**書いて読み返す** — 返ってきたアドレスが実際に張られて
     * いなければここでアボートする */
    map = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) return 21;
    memcpy(map, "OK", 2);
    if (memcmp(map, "OK", 2) != 0) return 22;
    if (write_all(1, "MAP\n", 4) < 0) return 23;

    /* 1 回の write で xv6fs のログ容量 (126 ブロック) を超える。
     *
     * xv6fs_write_file が書き込みを分割していないと
     *   KASSERT(lg.lh.n < XV6FS_LOGBLOCKS) @ xv6log_write
     * でカーネルパニックする。riscv64 では Orthox 上の gcc が .o を書いた
     * 瞬間に落ちたのがこれ。**共有コードなので aarch64 でも同じ道を通る。**
     * 書ける FS が無い構成では静かに飛ばす。 */
    {
        static char big[200 * 1024];
        int wfd;
        /* **段階ごとにマーカーを出す。**
         *
         * ディスク書き込みを伴う syscall は、失敗ではなく**沈黙で止まる**
         * ことがある。svc 処理中に IRQ がマスクされていたとき、例外も
         * エラーも出さずに vblk_rw の完了待ちループで永久停止した
         * (gdbstub で PC=vblk_rw+288 / PSTATE.I=1 を実測)。
         *
         * 段階を分けておかないと「open なのか write なのか、大きさの問題か」
         * が読めず、原因の切り分けに毎回カーネルを焼き直すことになる */
        if (write_all(1, "BW-START\n", 9) < 0) return 24;
        /* **root 直下とサブディレクトリを別々に見る。** 起動時の自己診断は
         * /via-vfs.txt (root 直下) しか作らないので、サブディレクトリの
         * 解決はここでしか通らない。分けておくと差が出たとき即座に絞れる */
        {
            int rfd = open("/bw-root.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (rfd >= 0) {
                if (write_all(1, "BW-ROOTOPEN\n", 12) < 0) return 35;
                close(rfd);
                unlink("/bw-root.bin");
            } else {
                if (write_all(1, "BW-ROOTFAIL\n", 12) < 0) return 36;
            }
        }
        wfd = open("/tmp/bigwrite.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (wfd >= 0) {
            ssize_t n;
            if (write_all(1, "BW-OPEN\n", 8) < 0) return 25;
            memset(big, 'B', sizeof(big));
            if (write_all(1, "BW-MEMSET\n", 10) < 0) return 26;
            /* まずログ容量 (126KB) に収まる 4KB。ここが通れば書き込み経路
             * そのものは生きていて、問題は「大きさ」に絞られる */
            n = write(wfd, big, 4096);
            if (n != 4096) return 27;
            if (write_all(1, "BW-4K\n", 6) < 0) return 28;
            /* 本命: 1 回で xv6fs のログ容量を超える */
            n = write(wfd, big, sizeof(big));
            close(wfd);
            if (n != (ssize_t)sizeof(big)) return 29;
            if (write_all(1, "BIGWRITE-OK\n", 12) < 0) return 30;
            unlink("/tmp/bigwrite.bin");
        } else {
            if (write_all(1, "BW-NOOPEN\n", 10) < 0) return 31;
        }
    }

    /* dup した fd から書き込み内容を読み戻せること。
     *
     * file_descriptor_t は size を **fd ごとの写し**で持つため、複製した fd は
     * 元の fd が書いた分を知らず、read が即 EOF を返していた。binutils の ar が
     * まさにこの形で動くので、**rc=0 のまま 0 バイトの .a が出来上がる**。
     * ar は何も言わないのでリンク段階まで誰も気付かない (riscv64 で踏んだ)。 */
    {
        int wfd = open("/tmp/duprw.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (wfd >= 0) {
            char rbuf[16];
            int rfd = dup(wfd);
            if (rfd < 0) return 41;
            if (write(wfd, "hello", 5) != 5) return 42;
            if (close(wfd) < 0) return 43;
            if (lseek(rfd, 0, SEEK_SET) != 0) return 44;
            memset(rbuf, 0, sizeof(rbuf));
            if (read(rfd, rbuf, sizeof(rbuf)) != 5) return 45;
            if (memcmp(rbuf, "hello", 5) != 0) return 46;
            if (close(rfd) < 0) return 47;
            if (write_all(1, "DUPRW-OK\n", 9) < 0) return 48;
            unlink("/tmp/duprw.bin");
        }
    }

    if (write_all(1, "DONE\n", 5) < 0) return 49;
    return 0;
}
