#include <stdint.h>

extern int open(const char* path, int flags);
extern int read(int fd, void* buf, int count);
extern int close(int fd);
extern int write(int fd, const void* buf, int count);
extern void _exit(int status) __attribute__((noreturn));

static void print_str(const char* s) {
    int len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    print_str("\r\n[User] Keyboard input test started. Type something (Press Enter to finish)...\r\n");

    char buf[128];
    int count = 0;
    
    while (1) {
        char c;
        int bytes = read(0, &c, 1);
        if (bytes == 1) {
            // エコーバック
            write(1, &c, 1);
            
            buf[count++] = c;
            if (c == '\n' || count >= 127) {
                break;
            }
        }
    }
    
    buf[count] = '\0';
    print_str("\r\n[User] You typed: ");
    print_str(buf);
    print_str("\r\n");

    _exit(0);
}
