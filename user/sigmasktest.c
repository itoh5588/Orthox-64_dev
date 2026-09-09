#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static void fail(const char* msg) {
    printf("sigmasktest: FAIL: %s\n", msg);
}

static void local_sigemptyset(sigset_t* set) {
    for (unsigned long i = 0; i < sizeof(set->__bits) / sizeof(set->__bits[0]); i++) {
        set->__bits[i] = 0;
    }
}

static void local_sigaddset(sigset_t* set, int sig) {
    set->__bits[0] |= (1UL << sig);
}

static int local_sigismember(const sigset_t* set, int sig) {
    return (set->__bits[0] & (1UL << sig)) != 0;
}

int main(void) {
    sigset_t set;
    sigset_t old;
    sigset_t pending;

    printf("--- Signal Mask / Pending Test ---\n");

    local_sigemptyset(&set);
    local_sigemptyset(&old);
    local_sigemptyset(&pending);
    local_sigaddset(&set, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &set, &old) != 0) {
        fail("sigprocmask(SIG_BLOCK) failed");
        return 1;
    }
    if (kill(getpid(), SIGUSR1) != 0) {
        fail("kill(SIGUSR1) failed");
        return 1;
    }
    if (sigpending(&pending) != 0) {
        fail("sigpending failed");
        return 1;
    }
    printf("pending after block=%lu\n", pending.__bits[0]);
    if (!local_sigismember(&pending, SIGUSR1)) {
        fail("SIGUSR1 not reported pending while blocked");
        return 1;
    }

    if (sigprocmask(SIG_SETMASK, &old, 0) != 0) {
        fail("sigprocmask(SIG_SETMASK restore) failed");
        return 1;
    }
    local_sigemptyset(&pending);
    if (sigpending(&pending) != 0) {
        fail("sigpending after restore failed");
        return 1;
    }
    printf("pending after restore=%lu\n", pending.__bits[0]);
    if (local_sigismember(&pending, SIGUSR1)) {
        fail("SIGUSR1 still reported pending after unmask");
        return 1;
    }

    printf("sigmasktest: PASS\n");
    return 0;
}
