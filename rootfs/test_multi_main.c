#include <stdio.h>
#include "test_multi_shared.h"

int main(void) {
    printf("%s %d\n", shared_message(), shared_answer());
    return 0;
}
