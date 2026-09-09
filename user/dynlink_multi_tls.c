#include <stdio.h>

extern const char* dyn_a_message(void);
extern int dyn_b_total(void);

int main(void) {
    int first = dyn_b_total();
    int second = dyn_b_total();
    printf("dynlink-multi-tls: %s %d %d\n", dyn_a_message(), first, second);
    if (first != 43 || second != 45) {
        printf("dynlink-multi-tls: FAIL\n");
        return 1;
    }
    printf("dynlink-multi-tls: PASS\n");
    return 0;
}
