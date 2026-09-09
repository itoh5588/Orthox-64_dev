.section .text
.global _start
_start:
    # write(1, msg, 20)
    mov $1, %rax
    mov $1, %rdi
    lea msg(%rip), %rsi
    mov $20, %rdx
    syscall

    # exit(0)
    mov $60, %rax
    xor %rdi, %rdi
    syscall

.section .data
msg:
    .ascii "Hello from OrthOS!\n"
