#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_str(const char *s) {
    write(1, s, strlen(s));
}

int main(void) {
    struct stat st;
    char buf[64];
    int rc;

    write_str("staterrno:start\n");
    errno = 0;
    rc = stat("/definitely-missing", &st);
    snprintf(buf, sizeof(buf), "staterrno: rc=%d errno=%d\n", rc, (rc < 0) ? errno : 0);
    write_str(buf);
    write_str("staterrno:end\n");

    return (rc == -1 && errno == ENOENT) ? 0 : 1;
}
