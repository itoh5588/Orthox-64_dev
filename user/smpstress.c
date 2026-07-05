#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../include/syscall.h"

extern char **environ;

static int run_exec_test(const char* path, char* const argv[]) {
    int status = 0;
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        execve(path, argv, environ);
        perror("execve");
        _exit(127);
    }
    if (waitpid(pid, &status, 0) != pid) {
        perror("waitpid");
        return 1;
    }
    if (((status >> 8) & 0xff) != 0) {
        printf("smpstress: FAIL path=%s status=%d cpu=%d\n",
               path, (status >> 8) & 0xff, get_cpu_id());
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    int rounds = 8;
    char* pipe_argv[] = { "pipestress", "8", NULL };
    char* fork_argv[] = { "forkcputest", "8", NULL };
    char* reap_argv[] = { "reaptest", NULL };

    if (argc > 1) {
        rounds = atoi(argv[1]);
        if (rounds <= 0) rounds = 8;
        if (rounds > 64) rounds = 64;
    }

    printf("smpstress: start rounds=%d cpu=%d\n", rounds, get_cpu_id());

    for (int i = 0; i < rounds; i++) {
        if (run_exec_test("/bin/pipestress.elf", pipe_argv) != 0) return 1;
        if (run_exec_test("/bin/forkcputest.elf", fork_argv) != 0) return 1;
        if (run_exec_test("/bin/reaptest.elf", reap_argv) != 0) return 1;
        if ((i + 1) % 2 == 0 || i + 1 == rounds) {
            printf("smpstress: progress=%d/%d cpu=%d\n",
                   i + 1, rounds, get_cpu_id());
        }
    }

    printf("smpstress: PASS rounds=%d cpu=%d\n", rounds, get_cpu_id());
    return 0;
}
