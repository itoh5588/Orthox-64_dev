#include <stdio.h>
#include <unistd.h>

extern char **environ;

static char g_env_path[] = "PATH=/usr/bin:/bin:/:/boot";
static char g_env_pwd[] = "PWD=/";
static char g_env_home[] = "HOME=/";
static char *g_envp[] = { g_env_path, g_env_pwd, g_env_home, NULL };

int main(void) {
    char *argv[] = {
        "ash",
        "-c",
        "echo envshow-start; /bin/muslenvshow.elf; echo envshow-done",
        NULL
    };
    printf("muslbusyboxdriver:start\n");
    if (!environ || !environ[0]) {
        environ = g_envp;
    }
    execve("/bin/ash", argv, environ);
    perror("execve /bin/ash");
    return 127;
}
