#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static char g_env_path[] = "PATH=/usr/bin:/bin:/:/boot";
static char g_env_pwd[] = "PWD=/";
static char g_env_home[] = "HOME=/";
static char *g_envp[] = { g_env_path, g_env_pwd, g_env_home, NULL };

int main(void) {
    pid_t pid;
    int status = -1;
    char *argv[] = { "/bin/staterrno.elf", NULL };

    printf("muslstatmissingdriver:start\n");
    if (!environ || !environ[0]) {
        environ = g_envp;
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        printf("muslstatmissingdriver:child\n");
        execve("/bin/staterrno.elf", argv, environ);
        perror("execve /bin/staterrno.elf");
        _exit(127);
    }

    printf("muslstatmissingdriver:parent child=%d\n", (int)pid);
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    printf("muslstatmissingdriver:status=%d\n", status);
    return 0;
}
