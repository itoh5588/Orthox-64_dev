#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

static void put_line(const char *s) {
    write(1, s, strlen(s));
    write(1, "\n", 1);
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/";
    DIR *dir = opendir(path);
    struct dirent *ent;

    if (!dir) {
        (void)errno;
        put_line("opendir failed");
        return 1;
    }

    while ((ent = readdir(dir)) != 0) {
        put_line(ent->d_name);
    }

    closedir(dir);
    return 0;
}
