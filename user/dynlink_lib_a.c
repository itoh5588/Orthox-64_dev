#include <stdio.h>

__thread int dyn_a_tls = 40;

int dyn_a_answer(void) {
    dyn_a_tls += 1;
    return dyn_a_tls;
}

const char* dyn_a_message(void) {
    return "dyn-a";
}
