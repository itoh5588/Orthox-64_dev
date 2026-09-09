#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_str(const char* s) {
    write(1, s, strlen(s));
}

int main(void) {
    int status = -1;
    pid_t pid;
    char buf[64];

    write_str("muslforkprobe:start\n");
    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        write_str("muslforkprobe:child\n");
        _exit(42);
    }

    snprintf(buf, sizeof(buf), "muslforkprobe:parent child=%d\n", (int)pid);
    write_str(buf);
    if (waitpid(pid, &status, 0) != pid) {
        perror("waitpid");
        return 2;
    }
    snprintf(buf, sizeof(buf), "muslforkprobe:status=%d\n", (status >> 8) & 0xff);
    write_str(buf);
    write_str("muslforkprobe:ok\n");
    return 0;
}
