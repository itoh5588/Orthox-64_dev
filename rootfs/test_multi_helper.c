#include "test_multi_shared.h"

int shared_answer(void) {
    return 64 + 8;
}

const char *shared_message(void) {
    return "Hello from Orthox-64 Multi File Build!";
}
