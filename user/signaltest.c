#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int kill(pid_t pid, int sig);

static void fail(const char* msg) {
    printf("signaltest: FAIL: %s\n", msg);
}

int main(void) {
    int status = 0;
    pid_t child = 0;
    pid_t pid = getpid();
    pid_t pgrp = getpgrp();

    printf("--- Signal / Process Group Test ---\n");
    printf("self: pid=%d pgrp=%d\n", pid, pgrp);

    if (pgrp <= 0) {
        fail("getpgrp returned invalid value");
        return 1;
    }
    if (kill(pid, 0) != 0) {
        fail("kill(pid, 0) failed");
        return 1;
    }

    child = fork();
    if (child < 0) {
        fail("fork failed");
        return 1;
    }
    if (child == 0) {
        if (setpgid(0, 0) != 0) {
            _exit(2);
        }
        for (;;) {
            getpid();
        }
    }

    if (kill(child, 0) != 0) {
        fail("kill(child, 0) failed");
        return 1;
    }
    if (kill(child, SIGTERM) != 0) {
        fail("kill(child, SIGTERM) failed");
        return 1;
    }
    if (waitpid(child, &status, 0) != child) {
        fail("waitpid after SIGTERM failed");
        return 1;
    }
    printf("killed child status=%d exit=%d signaled=%d termsig=%d\n",
           status, WEXITSTATUS(status), WIFSIGNALED(status), WTERMSIG(status));
    if (WEXITSTATUS(status) != 128 + SIGTERM) {
        fail("unexpected exit status after SIGTERM");
        return 1;
    }

    child = fork();
    if (child < 0) {
        fail("second fork failed");
        return 1;
    }
    if (child == 0) {
        _exit(7);
    }
    if (waitpid(child, &status, 0) != child) {
        fail("waitpid after normal exit failed");
        return 1;
    }
    printf("normal child status=%d exit=%d\n", status, WEXITSTATUS(status));
    if (WEXITSTATUS(status) != 7) {
        fail("unexpected exit status after normal exit");
        return 1;
    }

    printf("signaltest: PASS\n");
    return 0;
}
