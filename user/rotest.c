#include <stdio.h>
#include <stdint.h>

const char* const ro_data = "This is read-only data";

int main() {
    uint16_t cs, ds;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    __asm__ volatile("mov %%ds, %0" : "=r"(ds));

    printf("Read-only test start. CS: 0x%x, DS: 0x%x\n", cs, ds);
    printf("Data address: %p, Content: %s\n", ro_data, ro_data);

    printf("Attempting to write to read-only data (forced by volatile)...\n");
    
    // volatile により最適化を禁止し、物理的な書き込み命令を生成させる
    volatile char* p = (volatile char*)ro_data;
    *p = 'X'; 

    printf("Successfully wrote to read-only data!? (This should NOT happen)\n");
    return 0;
}
