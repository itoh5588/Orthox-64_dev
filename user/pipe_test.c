#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

int main() {
    int pipefd[2];
    char buf[128];
    
    printf("Pipe test start\n");
    
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return 1;
    }
    
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return 1;
    }
    
    if (pid == 0) {
        // Child: write to pipe
        close(pipefd[0]);
        const char* msg = "Hello from child via pipe!";
        write(pipefd[1], msg, strlen(msg) + 1);
        close(pipefd[1]);
        _exit(0);
    } else {
        // Parent: read from pipe
        close(pipefd[1]);
        read(pipefd[0], buf, sizeof(buf));
        printf("Parent received: %s\n", buf);
        close(pipefd[0]);
        wait(NULL);
    }
    
    printf("Pipe test success!\n");
    return 0;
}
