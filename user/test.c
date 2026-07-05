// OrthOS Dummy User Program
void _start(void) {
    // 将来的にはここでシステムコールを呼ぶ
    for (;;) {
        __asm__("pause");
    }
}
