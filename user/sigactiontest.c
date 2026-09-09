#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static void local_sigemptyset(sigset_t* set) {
    for (unsigned long i = 0; i < sizeof(set->__bits) / sizeof(set->__bits[0]); i++) {
        set->__bits[i] = 0;
    }
}

static int local_sigismember(const sigset_t* set, int sig) {
    return (set->__bits[0] & (1UL << sig)) != 0;
}

static void fail(const char* msg) {
    printf("sigactiontest: FAIL: %s\n", msg);
}

static void dummy_handler(int sig) {
    (void)sig;
}

int main(void) {
    struct sigaction oldact;
    struct sigaction act;
    struct sigaction cur;
    sigset_t pending;

    printf("--- Sigaction Registration Test ---\n");

    local_sigemptyset(&pending);
    act.sa_handler = dummy_handler;
    local_sigemptyset(&act.sa_mask);
    act.sa_flags = 0x1234;
    if (sigaction(SIGUSR1, &act, &oldact) != 0) {
        fail("sigaction install failed");
        return 1;
    }
    cur.sa_handler = SIG_DFL;
    local_sigemptyset(&cur.sa_mask);
    cur.sa_flags = 0;
    if (sigaction(SIGUSR1, 0, &cur) != 0) {
        fail("sigaction query failed");
        return 1;
    }
    printf("queried handler=%p flags=%x mask=%lu\n",
           (void*)cur.sa_handler, cur.sa_flags, (unsigned long)cur.sa_mask.__bits[0]);
    if (cur.sa_handler != dummy_handler || cur.sa_flags != 0x1234) {
        fail("sigaction query mismatch");
        return 1;
    }

    act.sa_handler = SIG_IGN;
    local_sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    if (sigaction(SIGUSR1, &act, 0) != 0) {
        fail("sigaction ignore install failed");
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
    printf("pending after SIG_IGN=%lu\n", pending.__bits[0]);
    if (local_sigismember(&pending, SIGUSR1)) {
        fail("ignored SIGUSR1 still pending");
        return 1;
    }

    printf("sigactiontest: PASS\n");
    return 0;
}
