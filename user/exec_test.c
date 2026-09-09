#include "syscall.h"
#include <stdint.h>

extern int write(int fd, const void* buf, int count);
extern int getpid(void);
extern void _exit(int status);

void puts(const char* s) {
    write(1, s, 0); // write implementation handles null-terminated strings if count=0?
    // Actually our write takes count. Let's write a quick strlen
}

static int strlen(const char* s) {
    int len = 0;
    while(s[len]) len++;
    return len;
}

void print(const char* s) {
    write(1, s, strlen(s));
}

int main(int argc, char** argv) {
    print("\r\n[Exec] Successfully executed exec_test program!\r\n");
    print("[Exec] My PID is: ");
    
    int pid = getpid();
    char pid_str[2] = { '0' + (char)pid, '\0' };
    print(pid_str);
    print("\r\n");
    
    print("[Exec] argc = ");
    char argc_str[2] = { '0' + (char)argc, '\0' };
    print(argc_str);
    print("\r\n");

    for (int i = 0; i < argc; i++) {
        print("[Exec] argv[");
        char i_str[2] = { '0' + (char)i, '\0' };
        print(i_str);
        print("] = ");
        if (argv[i]) {
            print(argv[i]);
        } else {
            print("(null)");
        }
        print("\r\n");
    }

    _exit(0);
    return 0;
}
