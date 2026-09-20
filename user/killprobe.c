/* sys_kill が正しく動くかを見る試験 (2026-09-20)。
 *
 * kernel/sys_proc.c の sys_kill は、**BKL を持っているだけで task_list を
 * 辿っていた**。BKL は task_list を守るロックではない (守るのは g_task_lock)
 * ので、kernel/task.c の task_signal_pid へ移した (日報2026-09-19 §9-7)。
 * そのときに挙動を変えていないことを見る。
 *
 * printf は使わない (user/cowstress.c と同じ方針)。
 */
#include <stddef.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

/* **x86 の musl sysroot は signal.h から kill を出さない** (aarch64 /
 * riscv64 側では出る)。3 アーキで同じソースを使いたいので、ここで宣言する */
extern int kill(pid_t pid, int sig);

static void put(const char* s) {
    size_t n = 0; while (s[n]) n++; (void)write(1, s, n);
}
static void put_int(int v) {
    char b[16]; int i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) b[i++] = '0';
    while (v > 0) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) b[i++] = '-';
    while (i > 0) { char c = b[--i]; (void)write(1, &c, 1); }
}
static int fail(const char* what, int got) {
    put("killprobe: FAIL "); put(what); put(" got="); put_int(got);
    put(" errno="); put_int(errno); put("\n");
    return 1;
}

int main(void) {
    int st = 0;
    pid_t child = fork();
    if (child < 0) return fail("fork", (int)child);
    if (child == 0) { for (;;) sleep(1); }   /* 親に終わらせてもらう */

    sleep(1);

    /* 1. 生きている相手への kill(pid, 0) は 0 */
    errno = 0;
    if (kill(child, 0) != 0) return fail("kill(child,0)", -1);

    /* 2. 居ない相手は -1 / ESRCH */
    errno = 0;
    if (kill(99999, 0) != -1 || errno != ESRCH) return fail("kill(99999,0)", errno);

    /* 3. SIGTERM で終わる */
    errno = 0;
    if (kill(child, SIGTERM) != 0) return fail("kill(child,SIGTERM)", -1);
    if (waitpid(child, &st, 0) != child) return fail("waitpid", -1);

    /* 4. 回収済みの相手は -1 / ESRCH */
    errno = 0;
    if (kill(child, 0) != -1 || errno != ESRCH) return fail("kill(reaped,0)", errno);

    put("killprobe: PASS kill(0)/ESRCH/SIGTERM/reaped\n");
    return 0;
}
