/* orthinfo — Orthox-64 (riscv64) の状態をまとめて出す自作コマンド。
 * user/riscv64-bin/ のビルド導線が通っていることの実例も兼ねる。 */
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

int main(void) {
    struct utsname u;
    struct timespec ts;
    char cwd[256];

    if (uname(&u) == 0) {
        printf("system   : %s %s\n", u.sysname, u.release);
        printf("build    : %s\n", u.version);
        printf("machine  : %s\n", u.machine);
        printf("nodename : %s\n", u.nodename);
    } else {
        printf("system   : (uname failed)\n");
    }

    printf("pid/ppid : %d/%d\n", (int)getpid(), (int)getppid());

    if (getcwd(cwd, sizeof(cwd))) {
        printf("cwd      : %s\n", cwd);
    }

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        printf("uptime   : %lld.%03lds\n",
               (long long)ts.tv_sec, ts.tv_nsec / 1000000L);
    }

    printf("orthinfo : ok\n");
    return 0;
}
