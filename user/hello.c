#include <stdint.h>

extern int write(int fd, const void* buf, int count);
extern void _exit(int status);

static void print_str(const char* s) {
    int len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

void _start() {
    print_str("\r\n[Hello App] Successfully executed via execve()!\r\n");
    print_str("[Hello App] I am a completely different program.\r\n");
    print_str("[Hello App] Now exiting...\r\n");
    _exit(0);
}
