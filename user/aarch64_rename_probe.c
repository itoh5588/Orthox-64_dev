/*
 * rename(2) の検査。
 *
 * **番号がアーキで違うところで落ちていた。** aarch64 の musl は
 * renameat(38) を出すが、カーネルは renameat2(276) しか見ていなかったため
 * `rename()` が丸ごと ENOSYS を返していた。configure も libtool も
 * move-if-change も rename を踏むので、ここが欠けると
 * 「なぜか途中で止まる」形でしか症状が出ない。
 *
 * **`mv` で確かめてはいけない。** busybox の mv は rename が失敗すると
 * copy+unlink に退くので、rename が壊れていても `mv` は成功する。
 * ここでは rename(2) を直に呼び、戻り値と errno をそのまま出す。
 *
 * 見るもの:
 *   1. 単純な付け替え。中身が保たれる
 *   2. 相手が既にあれば黙って置き換える
 *   3. ディレクトリを別の親へ移すと中身が付いてきて ".." も直る
 *   4. 空でないディレクトリは上書きできない (ENOTEMPTY)
 *   5. 自分の子孫へは移せない (EINVAL)
 *   6. 無いものは ENOENT
 *   7. /tmp (RAM) と SD をまたぐと EXDEV。**これは正しい失敗**で、
 *      mv はこれを見て copy+unlink に退く
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* rename(3) の宣言は <stdio.h> にあるが、取り込むと vfprintf ごと付いてくる。
 * 宣言だけ自分で書く */
extern int rename(const char* oldpath, const char* newpath);

/* **printf は使わない。**musl の vfprintf が浮動小数の整形を引き込み、
 * ソフト float (__divtf3) が要るのにカーネル向けの link には無い */
static void put_str(const char* s) {
    size_t len = strlen(s);
    while (len > 0) {
        ssize_t n = write(1, s, len);
        if (n <= 0) return;
        s += (size_t)n;
        len -= (size_t)n;
    }
}

static void put_int(int v) {
    char buf[16];
    int i = (int)sizeof(buf);
    unsigned u = (v < 0) ? (unsigned)(-v) : (unsigned)v;
    buf[--i] = '\0';
    do { buf[--i] = (char)('0' + (u % 10U)); u /= 10U; } while (u);
    if (v < 0) buf[--i] = '-';
    put_str(&buf[i]);
}

static int fails = 0;

static void ok(const char* name) {
    put_str("RENAME-OK "); put_str(name); put_str("\n");
}

static void ng(const char* name, const char* why, int err) {
    put_str("RENAME-BAD "); put_str(name); put_str(": "); put_str(why);
    put_str(" (errno="); put_int(err); put_str(")\n");
    fails++;
}

/* 期待どおりに成功したか */
static void expect_ok(const char* name, int rc) {
    if (rc == 0) ok(name);
    else ng(name, "rename が失敗した", errno);
}

/* 期待どおりの errno で失敗したか */
static void expect_err(const char* name, int rc, int want) {
    if (rc == 0) ng(name, "失敗するはずが成功した", 0);
    else if (errno != want) ng(name, "errno が違う", errno);
    else ok(name);
}

static int put(const char* path, const char* text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (write(fd, text, strlen(text)) != (ssize_t)strlen(text)) { close(fd); return -1; }
    return close(fd);
}

static int has_text(const char* path, const char* text) {
    char buf[128];
    ssize_t n;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) return 0;
    buf[n] = '\0';
    return strcmp(buf, text) == 0;
}

static int exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int main(void) {
    put_str("RENAME-START\n");

    /* 1. 単純な付け替え */
    if (put("/rp-a.txt", "ALPHA") < 0) { ng("setup", "書けない", errno); }
    expect_ok("simple", rename("/rp-a.txt", "/rp-b.txt"));
    if (exists("/rp-a.txt")) ng("simple-old", "古い名前が残っている", 0);
    else if (!has_text("/rp-b.txt", "ALPHA")) ng("simple-body", "中身が違う", 0);
    else ok("simple-body");

    /* 2. 上書き。相手の中身は消える */
    put("/rp-c.txt", "GAMMA");
    expect_ok("overwrite", rename("/rp-b.txt", "/rp-c.txt"));
    if (!has_text("/rp-c.txt", "ALPHA")) ng("overwrite-body", "置き換わっていない", 0);
    else ok("overwrite-body");

    /* 3. 同じ名前どうしは何もせずに成功 (POSIX) */
    expect_ok("self", rename("/rp-c.txt", "/rp-c.txt"));
    if (!has_text("/rp-c.txt", "ALPHA")) ng("self-body", "中身が消えた", 0);
    else ok("self-body");

    /* 4. 無いもの */
    expect_err("missing", rename("/rp-nope.txt", "/rp-d.txt"), ENOENT);

    /* 5. ディレクトリを別の親へ移す。中身が付いてきて ".." も直ること */
    mkdir("/rp-d1", 0755);
    mkdir("/rp-d2", 0755);
    put("/rp-d1/inner.txt", "INNER");
    expect_ok("dir-move", rename("/rp-d1", "/rp-d2/moved"));
    if (!has_text("/rp-d2/moved/inner.txt", "INNER")) ng("dir-body", "中身が付いてきていない", 0);
    else ok("dir-body");
    if (!exists("/rp-d2/moved/..")) ng("dir-dotdot", "'..' を辿れない", errno);
    else if (!exists("/rp-d2/moved/../moved/inner.txt")) ng("dir-dotdot", "'..' が古い親を指している", errno);
    else ok("dir-dotdot");

    /* 6. 空でないディレクトリは上書きできない */
    mkdir("/rp-d3", 0755);
    expect_err("dir-notempty", rename("/rp-d3", "/rp-d2"), ENOTEMPTY);

    /* 7. 自分の子孫へは移せない */
    expect_err("dir-loop", rename("/rp-d2", "/rp-d2/moved/deeper"), EINVAL);

    /* 8. 種別の食い違い */
    put("/rp-file.txt", "F");
    expect_err("dir-onto-file", rename("/rp-d3", "/rp-file.txt"), ENOTDIR);
    expect_err("file-onto-dir", rename("/rp-file.txt", "/rp-d3"), EISDIR);

    /* 9. /tmp (RAM) の中での付け替え */
    if (put("/tmp/rp-t1", "TMP") < 0) ng("tmp-setup", "/tmp に書けない", errno);
    expect_ok("tmp-simple", rename("/tmp/rp-t1", "/tmp/rp-t2"));
    if (!has_text("/tmp/rp-t2", "TMP")) ng("tmp-body", "中身が違う", 0);
    else ok("tmp-body");

    /* 10. **RAM と SD をまたぐ。**ここは EXDEV が正しい。
     *     mv はこれを見て copy+unlink に退く */
    expect_err("crossdev", rename("/tmp/rp-t2", "/rp-t3"), EXDEV);

    if (fails) {
        put_str("RENAME-FAILED "); put_int(fails); put_str("\n");
        return 1;
    }
    put_str("RENAME-DONE\n");
    return 0;
}
