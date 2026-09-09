#include <stdio.h>
#include <string.h>
#include "../include/syscall.h"

int main(int argc, char** argv) {
    if (argc == 1) {
        printf("forkmode: %s\n", get_fork_spread() ? "spread" : "pinned");
        return 0;
    }

    if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "spread") == 0) {
        if (set_fork_spread(1) < 0) {
            printf("forkmode: set failed\n");
            return 1;
        }
    } else if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "pinned") == 0) {
        if (set_fork_spread(0) < 0) {
            printf("forkmode: set failed\n");
            return 1;
        }
    } else {
        printf("usage: forkmode [on|off]\n");
        return 1;
    }

    printf("forkmode: %s\n", get_fork_spread() ? "spread" : "pinned");
    return 0;
}
