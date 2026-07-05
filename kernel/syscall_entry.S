.section .text
.global syscall_entry
.extern syscall_dispatch

syscall_entry:
    # 1. GS_BASE を入れ替えて Per-CPU データにアクセス可能にする
    swapgs

    # 2. ユーザー RSP を cpu_data->user_stack (offset 8) に退避
    mov %rsp, %gs:8
    
    # 3. カーネルスタック cpu_data->kernel_stack (offset 0) をロード
    mov %gs:0, %rsp

    # 4. カーネルスタック上に iretq 用のスタックフレームを構築
    pushq $0x1B    # SS (User Data)
    pushq %gs:8    # RSP (User RSP)
    pushq %r11     # RFLAGS
    pushq $0x23    # CS (User Code)
    pushq %rcx     # RIP (Return Address)
    
    # 5. 汎用レジスタ保存
    push %rax
    push %rdi
    push %rsi
    push %rdx
    push %r10
    push %r8
    push %r9
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15

    # 6. 第1引数にフレームポインタを渡し、ディスパッチ
    mov %rsp, %rdi
    
    mov %rsp, %rbx
    andq $-16, %rsp
    call syscall_dispatch
    mov %rbx, %rsp

    # 7. レジスタ復元
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

    # DS, ES をユーザーデータセグメント (0x1B) に設定
    mov $0x1B, %r11
    mov %r11, %ds
    mov %r11, %es

    # 8. ユーザー GS_BASE に戻す
    swapgs

    # 9. iretq でユーザーモードへ復帰
    iretq
