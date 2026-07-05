#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../include/syscall.h"

int main(int argc, char** argv) {
    int children = 4;
    if (argc > 1) {
        children = atoi(argv[1]);
        if (children <= 0) children = 4;
        if (children > 16) children = 16;
    }

    printf("forkcputest: parent pid=%d cpu=%d children=%d\n",
           getpid(), get_cpu_id(), children);

    for (int i = 0; i < children; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            printf("forkcputest: child index=%d pid=%d cpu=%d\n",
                   i, getpid(), get_cpu_id());
            _exit(20 + i);
        }
    }

    for (int reaped = 0; reaped < children; reaped++) {
        int status = 0;
        pid_t pid = wait(&status);
        if (pid < 0) {
            perror("wait");
            return 1;
        }
        printf("forkcputest: reaped pid=%d status=%d cpu=%d\n",
               pid, (status >> 8) & 0xff, get_cpu_id());
    }

    printf("forkcputest: done parent cpu=%d\n", get_cpu_id());
    return 0;
}
