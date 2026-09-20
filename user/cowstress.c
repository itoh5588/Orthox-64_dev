/*
 * fork の copy-on-write の負荷試験 (2026-09-19)。
 *
 * fork を 3 アーキ共通の CoW (kernel/vm_cow.c) にしたので、**複数の CPU で
 * 同時に共有と写しを起こして、中身が混ざらないこと**を確かめる。
 * user/aarch64_fork_probe.c は 1 回の fork で経路が通るかを見るもので、
 * こちらは数と並行性を見る。
 *
 * 1 ラウンドの流れ (worker ごとに rounds 回):
 *
 *   親: 全ページを模様 A で埋める → pipe を作って fork
 *   子: 全ページが A であること (共有中のページが読める)
 *       奇数ページはユーザーの store で、偶数ページは read(2) で模様 B を書く
 *       (read はカーネルがユーザーのバッファへ直接書く = カーネル側の
 *       CoW フォルト。aarch64 なら EL1、riscv64 なら S モード)
 *       全ページが B であること
 *   親: 子が走っている間に前半のページを模様 C で上書きする
 *       (同じページを親子が別の CPU で同時に写す競争)
 *       子を回収して、前半が C・後半が A のままであること
 *
 * worker は既定で 8 本。4 コアに散って、TLB の古い変換が残る状況
 * (fork の後で親が別の CPU へ移る) を作りやすくする。CPU を移すのは
 * タイマの切り替えに任せる。**sched_yield は使わない** —— 散り方をタイマ
 * 任せにして、呼び出しの回数で結果が変わらないようにするため。
 * (2026-09-19 の時点では aarch64 に無くて ENOSYS だったのが理由だったが、
 *  2026-09-20 に 3 アーキとも通るようにした。それでも使わない方針は変えない)
 *
 * 模様は 8 バイトごとに (tag, ページ番号, 語番号) を詰めた値なので、
 * 外れたときにどのページのどの語が誰の模様だったかまで出せる。
 *
 *   cowstress [workers] [rounds]      既定 8 50
 *   成功: "cowstress: PASS workers=8 rounds=50"
 *   失敗: "cowstress: FAIL ..." (どこで何が見えたか)
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE     4096
#define PAGES    32
#define WORDS    (PAGE / 8)
#define CHUNK    2048   /* パイプは 4096 バイト。自分で書いて自分で読むので半分ずつ */

/* **bss に置き、ページ境界に揃える。**ページ単位で共有と写しが起きるので、
 * 1 ページ = 1 単位で模様を持たせる */
static uint64_t g_buf[PAGES][WORDS] __attribute__((aligned(PAGE)));

/* printf は使わない (libgcc に頼らず 3 アーキで同じに組めるように) */
static void put(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    (void)write(1, s, n);
}

static void put_num(uint64_t v) {
    char b[24];
    int i = 23;
    b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    put(&b[i]);
}

static void put_hex(uint64_t v) {
    char b[19];
    b[0] = '0'; b[1] = 'x'; b[18] = 0;
    for (int i = 17; i >= 2; i--) { b[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    put(b);
}

static uint64_t pattern(uint64_t tag, int page, int word) {
    return (tag << 32) | ((uint64_t)page << 16) | (uint64_t)word;
}

static void fill_page(uint64_t* p, uint64_t tag, int page) {
    for (int w = 0; w < WORDS; w++) p[w] = pattern(tag, page, w);
}

/* 外れた最初の語を報告して 1 を返す */
static int check_page(const char* who, uint64_t tag, int page) {
    for (int w = 0; w < WORDS; w++) {
        uint64_t want = pattern(tag, page, w);
        uint64_t got = g_buf[page][w];
        if (got != want) {
            put("cowstress: FAIL "); put(who);
            put(" page="); put_num((uint64_t)page);
            put(" word="); put_num((uint64_t)w);
            put(" want="); put_hex(want);
            put(" got="); put_hex(got);
            put("\n");
            return 1;
        }
    }
    return 0;
}

/* 子の側。戻り値が終了コード */
static int child_round(uint64_t tag_a, uint64_t tag_b) {
    int fds[2];
    uint64_t tmp[CHUNK / 8];

    for (int p = 0; p < PAGES; p++) {
        if (check_page("child-sees-parent", tag_a, p)) return 2;
    }
    if (pipe(fds) < 0) return 3;
    for (int p = 0; p < PAGES; p++) {
        if (p & 1) {
            /* ユーザーの store。共有中なら EL0 / U モードのフォルトで写る */
            fill_page(g_buf[p], tag_b, p);
        } else {
            /* read(2) でカーネルに書かせる。**このページには自分では
             * 触らない**ので、写すのはカーネル側のフォルト */
            for (int half = 0; half < PAGE / CHUNK; half++) {
                int base = half * (CHUNK / 8);
                for (int w = 0; w < CHUNK / 8; w++) tmp[w] = pattern(tag_b, p, base + w);
                if (write(fds[1], tmp, CHUNK) != CHUNK) return 4;
                if (read(fds[0], (char*)g_buf[p] + half * CHUNK, CHUNK) != CHUNK) return 5;
            }
        }
    }
    close(fds[0]);
    close(fds[1]);
    for (int p = 0; p < PAGES; p++) {
        if (check_page("child-own-write", tag_b, p)) return 6;
    }
    return 0;
}

static int worker(int id, int rounds) {
    for (int r = 0; r < rounds; r++) {
        uint64_t tag_a = ((uint64_t)id << 20) | ((uint64_t)r << 4) | 0xa;
        uint64_t tag_b = ((uint64_t)id << 20) | ((uint64_t)r << 4) | 0xb;
        uint64_t tag_c = ((uint64_t)id << 20) | ((uint64_t)r << 4) | 0xc;
        int status = 0;
        pid_t pid;

        for (int p = 0; p < PAGES; p++) fill_page(g_buf[p], tag_a, p);

        pid = fork();
        if (pid < 0) {
            put("cowstress: FAIL fork\n");
            return 10;
        }
        if (pid == 0) _exit(child_round(tag_a, tag_b));

        /* 子が走っている間に前半を上書きする (同じページを両側で写す競争) */
        for (int p = 0; p < PAGES / 2; p++) {
            fill_page(g_buf[p], tag_c, p);
        }

        if (waitpid(pid, &status, 0) != pid) {
            put("cowstress: FAIL waitpid\n");
            return 11;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            put("cowstress: FAIL child worker="); put_num((uint64_t)id);
            put(" round="); put_num((uint64_t)r);
            put(" status="); put_num((uint64_t)status);
            put("\n");
            return 12;
        }
        /* 子の書き込み (B) が親に見えていないこと */
        for (int p = 0; p < PAGES; p++) {
            if (check_page(p < PAGES / 2 ? "parent-overwrote" : "parent-untouched",
                           p < PAGES / 2 ? tag_c : tag_a, p)) {
                return 13;
            }
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    int workers = argc > 1 ? atoi(argv[1]) : 8;
    int rounds = argc > 2 ? atoi(argv[2]) : 50;
    int fails = 0;

    if (workers < 1) workers = 1;
    if (rounds < 1) rounds = 1;

    put("cowstress: start workers="); put_num((uint64_t)workers);
    put(" rounds="); put_num((uint64_t)rounds); put("\n");

    for (int i = 0; i < workers; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            put("cowstress: FAIL fork worker\n");
            return 1;
        }
        if (pid == 0) _exit(worker(i, rounds));
    }
    for (int i = 0; i < workers; i++) {
        int status = 0;
        if (wait(&status) < 0) {
            put("cowstress: FAIL wait\n");
            return 1;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) fails++;
    }
    if (fails) {
        put("cowstress: FAIL workers-failed="); put_num((uint64_t)fails); put("\n");
        return 1;
    }
    put("cowstress: PASS workers="); put_num((uint64_t)workers);
    put(" rounds="); put_num((uint64_t)rounds); put("\n");
    return 0;
}
