#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../include/syscall.h"

static int run_once(int iter, const char* payload, size_t payload_len) {
    int pipefd[2];
    pid_t writer;
    pid_t reader;
    int writer_status = 0;
    int reader_status = 0;

    if (pipe(pipefd) < 0) {
        perror("pipe");
        return 1;
    }

    writer = fork();
    if (writer < 0) {
        perror("fork(writer)");
        close(pipefd[0]);
        close(pipefd[1]);
        return 1;
    }
    if (writer == 0) {
        close(pipefd[0]);
        if (write(pipefd[1], payload, payload_len) != (ssize_t)payload_len) {
            _exit(11);
        }
        close(pipefd[1]);
        _exit(0);
    }

    reader = fork();
    if (reader < 0) {
        perror("fork(reader)");
        close(pipefd[0]);
        close(pipefd[1]);
        waitpid(writer, &writer_status, 0);
        return 1;
    }
    if (reader == 0) {
        char buf[64];
        int bytes = 0;
        int lines = 0;
        ssize_t n;

        close(pipefd[1]);
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            bytes += (int)n;
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\n') lines++;
            }
        }
        close(pipefd[0]);

        if (n < 0) _exit(12);
        if (bytes != (int)payload_len) _exit(13);
        if (lines != 3) _exit(14);
        _exit(20 + (iter & 0x1f));
    }

    close(pipefd[0]);
    close(pipefd[1]);

    if (waitpid(writer, &writer_status, 0) != writer) {
        perror("waitpid(writer)");
        return 1;
    }
    if (waitpid(reader, &reader_status, 0) != reader) {
        perror("waitpid(reader)");
        return 1;
    }

    if (((writer_status >> 8) & 0xff) != 0) {
        printf("pipestress: writer failed iter=%d status=%d cpu=%d\n",
               iter, (writer_status >> 8) & 0xff, get_cpu_id());
        return 1;
    }
    if (((reader_status >> 8) & 0xff) < 20) {
        printf("pipestress: reader failed iter=%d status=%d cpu=%d\n",
               iter, (reader_status >> 8) & 0xff, get_cpu_id());
        return 1;
    }

    return 0;
}

int main(int argc, char** argv) {
    int loops = 32;
    const char payload[] = "alpha\nbeta\ngamma\n";
    size_t payload_len = strlen(payload);

    if (argc > 1) {
        loops = atoi(argv[1]);
        if (loops <= 0) loops = 32;
        if (loops > 256) loops = 256;
    }

    printf("pipestress: start loops=%d cpu=%d\n", loops, get_cpu_id());

    for (int i = 0; i < loops; i++) {
        if (run_once(i, payload, payload_len) != 0) {
            return 1;
        }
        if ((i + 1) % 8 == 0 || i + 1 == loops) {
            printf("pipestress: progress=%d/%d cpu=%d\n",
                   i + 1, loops, get_cpu_id());
        }
    }

    printf("pipestress: PASS loops=%d cpu=%d\n", loops, get_cpu_id());
    return 0;
}
