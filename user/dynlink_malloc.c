#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    void* p = malloc(752);
    if (!p) {
        printf("dynlink-malloc: FAIL malloc\n");
        return 1;
    }
    memset(p, 0x5a, 752);
    printf("dynlink-malloc: PASS %p\n", p);
    free(p);
    return 0;
}
