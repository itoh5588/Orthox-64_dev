#include <stdint.h>
#include <stddef.h>
#include "kassert.h"
#include "task_internal.h"
#include "pmm.h"
#include "vmm.h"
#include "fs.h"

extern struct task* task_list;
extern void fork_child_entry(void);

static void kernel_strcpy(char* dst, const char* src, size_t size) {
    size_t i = 0;
    if (!dst || size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (src[i] && i + 1 < size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

int task_fork(struct syscall_frame* frame) {
    struct task* parent = get_current_task();
    struct task* child;
    uint32_t spawn_cpu;
    void* kstack_phys;
    uint64_t flags;
    KASSERT(parent != 0);
    KASSERT(frame != 0);
    KASSERT(parent->ctx.cr3 != 0);
    // The heavy setup below (address-space copy, kernel stack, fd clone) runs
    // outside g_task_lock: the parent is blocked here and the big kernel lock
    // already serializes it against other syscalls. Holding g_task_lock with
    // IRQs off across vmm_copy_pml4 would stall every other CPU's scheduler
    // tick for the whole copy. Only the final publish needs the lock.
    child = task_alloc_struct();
    if (!child) {
        return -1;
    }
    child->ppid = parent->pid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->sig_pending = 0;
    child->sig_mask = parent->sig_mask;
    for (int i = 0; i < 32; i++) {
        child->sig_handlers[i] = parent->sig_handlers[i];
        child->sig_action_masks[i] = parent->sig_action_masks[i];
        child->sig_action_flags[i] = parent->sig_action_flags[i];
    }
    child->heap_break = parent->heap_break;
    child->mmap_end = parent->mmap_end;
    child->user_entry = parent->user_entry;
    child->user_stack = parent->user_stack;
    child->user_stack_top = parent->user_stack_top;
    child->user_stack_bottom = parent->user_stack_bottom;
    child->user_stack_guard = parent->user_stack_guard;
    child->user_fs_base = parent->user_fs_base;
    child->tls_vaddr = parent->tls_vaddr;
    child->tls_filesz = parent->tls_filesz;
    child->tls_memsz = parent->tls_memsz;
    child->tls_align = parent->tls_align;
    child->timeslice_ticks = TASK_TIMESLICE_TICKS;
    child->ctx.cr3 = vmm_copy_pml4((uint64_t*)PHYS_TO_VIRT(parent->ctx.cr3));
    if (!child->ctx.cr3) {
        task_free_struct(child);
        return -1;
    }
    kernel_strcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    kstack_phys = pmm_alloc(4);
    if (!kstack_phys) {
        vmm_free_user_pml4(child->ctx.cr3);
        task_free_struct(child);
        return -1;
    }
    child->kstack_top = (uint64_t)PHYS_TO_VIRT(kstack_phys) + 4 * PAGE_SIZE;
    child->os_stack_ptr = child->kstack_top;
    struct syscall_frame* child_frame = (struct syscall_frame*)(child->kstack_top - sizeof(struct syscall_frame));
    for (size_t i = 0; i < sizeof(struct syscall_frame); i++) {
        ((uint8_t*)child_frame)[i] = ((uint8_t*)frame)[i];
    }
    child_frame->rax = 0;
    child_frame->rflags &= ~0x400ULL;
    uint64_t* sp = (uint64_t*)child_frame;
    *(--sp) = (uint64_t)fork_child_entry;
    *(--sp) = 0x202;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    child->ctx.rip = (uint64_t)fork_child_entry;
    child->ctx.rsp = (uint64_t)sp;
    child->ctx.rflags = 0x202;
    child->ctx.cs = 0x08;
    child->ctx.ss = 0x10;
    for (int i = 0; i < 512; i++) child->ctx.fxsave_area[i] = parent->ctx.fxsave_area[i];
    for (int i = 0; i < MAX_FDS; i++) {
        if (fs_clone_fd(&child->fds[i], &parent->fds[i]) < 0) {
            for (int j = 0; j < i; j++) {
                fs_release_fd(&child->fds[j]);
            }
            if (child->ctx.cr3 && child->ctx.cr3 != vmm_get_kernel_pml4_phys()) {
                vmm_free_user_pml4(child->ctx.cr3);
            }
            pmm_free((void*)VIRT_TO_PHYS(child->kstack_top - 4 * PAGE_SIZE), 4);
            task_free_struct(child);
            return -1;
        }
    }
    KASSERT(child->kstack_top != 0);
    KASSERT(child->ctx.cr3 != 0);
    flags = task_lock_irqsave();
    child->pid = task_next_pid_locked();
    spawn_cpu = task_choose_fork_cpu_locked((uint32_t)parent->cpu_affinity);
    task_mark_ready_on_cpu_locked_internal(child, spawn_cpu);
    child->next = task_list;
    task_list = child;
    task_rebalance_ready_task_locked_internal(child);
    task_unlock_irqrestore(flags);
    task_request_resched_cpu((uint32_t)child->cpu_affinity);
    return child->pid;
}
