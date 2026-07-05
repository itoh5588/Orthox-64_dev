#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int has_entry(const char* path, const char* name) {
    DIR* dir = opendir(path);
    struct dirent* ent;
    if (!dir) return 0;
    while ((ent = readdir(dir)) != 0) {
        if (strcmp(ent->d_name, name) == 0) {
            closedir(dir);
            return 1;
        }
    }
    closedir(dir);
    return 0;
}

int main(void) {
    struct stat st;
    char cwd[128];

    printf("--- mkdir/rmdir Test ---\n");
    if (mkdir("/tmpdir", 0755) < 0) {
        printf("mkdir failed\n");
        return 1;
    }
    if (stat("/tmpdir", &st) < 0) {
        printf("stat failed\n");
        return 2;
    }
    if ((st.st_mode & 0170000) != 0040000) {
        printf("not a directory\n");
        return 3;
    }
    if (!has_entry("/", "tmpdir")) {
        printf("dir not listed in root\n");
        return 4;
    }
    if (chdir("/tmpdir") < 0) {
        printf("chdir failed\n");
        return 5;
    }
    if (!getcwd(cwd, sizeof(cwd))) {
        printf("getcwd failed\n");
        return 6;
    }
    printf("cwd in dir=%s\n", cwd);
    if (strcmp(cwd, "/tmpdir") != 0) {
        printf("unexpected cwd\n");
        return 7;
    }
    if (chdir("/") < 0) {
        printf("chdir root failed\n");
        return 8;
    }
    if (rmdir("/tmpdir") < 0) {
        printf("rmdir failed\n");
        return 9;
    }
    if (stat("/tmpdir", &st) == 0) {
        printf("dir still exists after rmdir\n");
        return 10;
    }
    if (has_entry("/", "tmpdir")) {
        printf("dir still listed after rmdir\n");
        return 11;
    }
    printf("mkdirtest: PASS\n");
    return 0;
}
