#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>

void deep_recursion(int depth) {
    if (depth <= 0) return;
    getpid();
    deep_recursion(depth - 1);
}

int main() {
    printf("--- Robustness Test (Task 4) ---\n");
    for (int j = 0; j < 10; j++) {
        pid_t pid = fork();
        if (pid == 0) {
            deep_recursion(50);
            exit(0);
        } else {
            int status;
            waitpid(pid, &status, 0);
        }
        if (j % 2 == 0) printf("  Iteration %d/10...\n", j + 1);
    }
    printf("Robustness test COMPLETED.\n");
    return 0;
}
