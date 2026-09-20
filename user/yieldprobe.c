/* sched_yield が通るかだけを見る小さな試験 (2026-09-20)。
 *
 * **番号がアーキで違う。**x86_64 は 24、aarch64 / riscv64 の generic ABI は
 * 124。実装は共有層 (kernel/sys_task.c) にあり、kernel/linux_syscall.c が
 * 124 を繋ぐ。繋ぎ忘れると -ENOSYS が返る (日報2026-09-19 §9-5)。
 *
 * printf は使わない —— libgcc のソフト浮動小数点に頼らず 3 アーキで同じに
 * 組めるようにするため (user/cowstress.c と同じ方針)。
 */
#include <stddef.h>
#include <sched.h>
#include <errno.h>
#include <unistd.h>

static void put(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    (void)write(1, s, n);
}

static void put_int(int v) {
    char buf[16];
    int i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) buf[i++] = '0';
    while (v > 0) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) buf[i++] = '-';
    while (i > 0) { char c = buf[--i]; (void)write(1, &c, 1); }
}

int main(void) {
    for (int i = 0; i < 3; i++) {
        errno = 0;
        int r = sched_yield();
        if (r != 0) {
            put("yieldprobe: FAIL r="); put_int(r);
            put(" errno="); put_int(errno); put("\n");
            return 1;
        }
    }
    put("yieldprobe: PASS sched_yield x3\n");
    return 0;
}
