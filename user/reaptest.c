#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>

int main() {
    printf("--- Task Reaping & Exit Test ---\n");
    int n = 5;
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            int exit_code = 10 + i;
            printf("  [Child] PID %d exiting with %d\n", getpid(), exit_code);
            exit(exit_code);
        } else if (pid < 0) {
            perror("fork");
            return 1;
        }
    }
    int reaped = 0;
    while (reaped < n) {
        int status;
        pid_t child_pid = wait(&status);
        if (child_pid > 0) {
            int code = (status >> 8) & 0xFF;
            printf("  [Parent] Reaped PID %d, exit code: %d\n", child_pid, code);
            reaped++;
        } else {
            printf("  [Parent] wait() error\n");
            break;
        }
    }
    if (reaped == n) printf("Reaping test SUCCESS.\n");
    return 0;
}
