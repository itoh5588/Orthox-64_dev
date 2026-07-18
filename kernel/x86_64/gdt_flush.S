.global gdt_flush
.global jump_to_user

gdt_flush:
    lgdt (%rdi)

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0, %ax
    mov %ax, %fs
    mov %ax, %gs

    # CS を更新 (0x08)
    # lretq を使って、スタックに [RIP, CS] を積んでから戻る
    lea .next(%rip), %rax
    pushq $0x08
    pushq %rax
    lretq

.next:
    ret

# jump_to_user(entry_point, user_stack)
# RDI: entry_point
# RSI: user_stack
jump_to_user:
    # 割り込みを禁止
    cli

    # スタックフレームの構築 (Ring 3 用)
    # 64bit iretq が期待する順序: SS, RSP, RFLAGS, CS, RIP
    
    pushq $0x1B         # User Data Selector (0x18 | RPL 3)
    pushq %rsi          # User Stack Pointer (RSP)
    pushfq              # RFLAGS
    popq %rax
    orq $0x200, %rax    # IF (Interrupt Flag) を有効化
    pushq %rax
    pushq $0x23         # User Code Selector (0x20 | RPL 3)
    pushq %rdi          # Entry Point (RIP)

    # セグメントレジスタをユーザーデータセレクタで更新
    mov $0x1B, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    # ユーザーモードへジャンプ！
    iretq
