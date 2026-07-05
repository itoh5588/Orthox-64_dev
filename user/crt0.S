.section .text
.global _start
.type _start, @function
.extern __libc_start_main
.extern main
.extern _init
.extern _fini

_start:
    xor %rbp, %rbp

    mov (%rsp), %rsi
    lea 8(%rsp), %rdx
    lea main(%rip), %rdi
    lea _init(%rip), %rcx
    lea _fini(%rip), %r8
    xor %r9d, %r9d
    andq $-16, %rsp
    call __libc_start_main

.loop:
    hlt
    jmp .loop
