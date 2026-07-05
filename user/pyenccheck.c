#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern char **environ;
int setenv(const char *name, const char *value, int overwrite);

int main(void) {
    char *argv[] = {
        "/bin/python3",
        "-c",
        "import encodings; print('ok')",
        0,
    };
    execve("/bin/python3", argv, environ);
    perror("execve");
    return 1;
}
