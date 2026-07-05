#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_str(const char *s) {
    write(1, s, strlen(s));
}

int main(void) {
    const char *path = getenv("PATH");
    write_str("muslenvshow: PATH=");
    write_str(path ? path : "(null)");
    write_str("\n");
    _exit(0);
}
