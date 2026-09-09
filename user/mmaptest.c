#include <stdio.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int main() {
    printf("--- OrthOS mmap Test ---\n");
    size_t length = 4096 * 2;
    void* addr = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == (void*)-1) {
        printf("mmap failed!\n");
        return 1;
    }
    printf("mmap successful! Allocated at: %p\n", addr);
    const char* test_str1 = "Hello, OrthOS mmap (Page 1)!";
    strcpy((char*)addr, test_str1);
    char* addr_page2 = (char*)addr + 4096;
    const char* test_str2 = "Checking Page 2 alignment...";
    strcpy(addr_page2, test_str2);
    if (strcmp((char*)addr, test_str1) == 0 && strcmp(addr_page2, test_str2) == 0) {
        printf("Read/Write check: SUCCESS\n");
        printf("Page 1: '%s'\n", (char*)addr);
        printf("Page 2: '%s'\n", addr_page2);
    } else {
        printf("Read/Write check: FAILED!\n");
        return 1;
    }
    printf("mmap test COMPLETED SUCCESSFULLY.\n");
    return 0;
}
