.global _syscall_wrap

# int64_t _syscall_wrap(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6)
# System V AMD64 ABI:
# rdi = num (syscall number)
# rsi = arg1
# rdx = arg2
# rcx = arg3
# r8  = arg4
# r9  = arg5
# stack+8 = arg6

.text
_syscall_wrap:
    # rax = syscall number (from rdi)
    # rdi = arg1 (from rsi)
    # rsi = arg2 (from rdx)
    # rdx = arg3 (from rcx)
    # r10 = arg4 (from r8)
    # r8  = arg5 (from r9)
    # r9  = arg6 (from stack)

    mov %rdi, %rax   # syscall number
    mov %rsi, %rdi   # arg1
    mov %rdx, %rsi   # arg2
    mov %rcx, %rdx   # arg3
    mov %r8, %r10    # arg4
    mov %r9, %r8     # arg5
    mov 8(%rsp), %r9 # arg6 (stack)

    syscall

    ret
