.section .text
.global switch_context
.global restore_context
.global call_app

# void switch_context(struct task_context* next_ctx, struct task_context* prev_ctx)
switch_context:
    pushfq
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, 112(%rsi) # ctx.rsp offset
    
    # SSE 保存・復元 (ctx が 16バイト境界なら 192 も 16バイト境界)
    fxsave 192(%rsi)
    fxrstor 192(%rdi)

    # CR3 を切り替える
    mov (%rdi), %rax    # next_ctx->cr3
    mov %rax, %cr3

    mov 112(%rdi), %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    andq $~0x100, (%rsp)
    popfq
    ret

# int call_app(int argc, char** argv, uint16_t ss, uint64_t rip, uint64_t rsp, uint64_t* os_stack_ptr)
call_app:
    cli
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, (%r9)

    mov %rdx, %r10 # ss
    mov %r8, %r11  # rsp
    
    pushq %r10     # SS
    pushq %r11     # RSP
    pushfq         # RFLAGS
    orq $0x200, (%rsp)
    lea 8(%r10), %rax
    pushq %rax     # CS
    pushq %rcx     # RIP

    mov %r10, %ds
    mov %r10, %es

    xor %rax, %rax
    xor %rbx, %rbx
    xor %rcx, %rcx
    xor %r8, %r8
    xor %r9, %r9
    xor %r10, %r10
    xor %r11, %r11
    xor %r12, %r12
    xor %r13, %r13
    xor %r14, %r14
    xor %r15, %r15

    # Entering user mode directly from kernel context; restore user GS base
    # so the next syscall swapgs lands on the per-CPU area.
    swapgs
    iretq

.global fork_child_entry
fork_child_entry:
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    pop %r9
    pop %r8
    pop %r10
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rax

    # スタック上の SS を利用して DS, ES を設定
    mov 32(%rsp), %r11
    mov %r11, %ds
    mov %r11, %es

    # fork child returns to user without passing through syscall exit, so we
    # must restore the user GS base here as well.
    swapgs
    iretq
