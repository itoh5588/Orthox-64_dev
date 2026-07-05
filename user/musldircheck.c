#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int main(void) {
    DIR *dir;
    struct dirent *ent;
    struct stat st;
    char buf[64];
    int fd;
    int n;
    int count = 0;
    int found_encodings = 0;

    if (stat("/lib/python3.12", &st) < 0) {
        perror("stat /lib/python3.12");
        return 1;
    }
    printf("stat /lib/python3.12 ok mode=%o size=%ld mtime=%ld\n",
           st.st_mode,
           (long)st.st_size,
           (long)st.st_mtime);

    dir = opendir("/lib/python3.12");
    if (!dir) {
        perror("opendir /lib/python3.12");
        return 2;
    }

    while ((ent = readdir(dir)) != 0) {
        count++;
        if (strcmp(ent->d_name, "encodings") == 0) {
            found_encodings = 1;
        }
    }
    printf("readdir count=%d found_encodings=%d\n", count, found_encodings);

    if (stat("/lib/python3.12/encodings/__init__.py", &st) < 0) {
        perror("stat encodings/__init__.py");
        return 3;
    }
    printf("stat encodings/__init__.py ok size=%ld mtime=%ld\n",
           (long)st.st_size,
           (long)st.st_mtime);
    fd = open("/lib/python3.12/encodings/__init__.py", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open encodings/__init__.py");
        return 5;
    }
    n = (int)read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        perror("read encodings/__init__.py");
        close(fd);
        return 6;
    }
    buf[(n > 0) ? n : 0] = '\0';
    close(fd);
    printf("read encodings/__init__.py n=%d prefix=%.32s\n", n, buf);
    return found_encodings ? 0 : 4;
}
