#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void write_str(const char *s) {
    write(1, s, strlen(s));
}

int main(void) {
    char *const argv[] = {
        (char *)"/bin/printenv",
        (char *)"PATH",
        0,
    };
    char *const envp[] = {
        (char *)"PATH=/usr/bin:/bin:/:/boot",
        0,
    };

    write_str("muslexecprobe:start\n");
    execve("/bin/printenv", argv, envp);
    perror("execve /bin/printenv");
    return 1;
}
