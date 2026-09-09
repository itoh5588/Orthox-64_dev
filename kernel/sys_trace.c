#include <stdint.h>
#include "pmm.h"
#include "sys_internal.h"
#include "task.h"
#include "vmm.h"

#ifndef ORTHOX_MEM_TRACE
#define ORTHOX_MEM_TRACE 0
#endif

#ifndef ORTHOX_MEM_PROGRESS
#define ORTHOX_MEM_PROGRESS 0
#endif

extern void puts(const char* s);
extern void puthex(uint64_t v);

void syscall_trace_progress_bump(struct task* current, uint64_t* counter) {
#if ORTHOX_MEM_PROGRESS
    if (!current || !current->trace_progress || !counter) return;
    (*counter)++;
#else
    (void)current;
    (void)counter;
#endif
}

void syscall_trace_progress_bump_syscall(struct task* current, uint64_t syscall_no, uint64_t arg2) {
#if ORTHOX_MEM_PROGRESS
    if (!current || !current->trace_progress) return;
    switch (syscall_no) {
        case SYS_READ:
        case SYS_READV:
            current->trace_read_calls++;
            if (syscall_no == SYS_READ) current->trace_read_bytes += arg2;
            break;
        case SYS_WRITE:
        case SYS_WRITEV:
            current->trace_write_calls++;
            if (syscall_no == SYS_WRITE) {
                current->trace_write_bytes += arg2;
                if (arg2 > current->trace_write_max) current->trace_write_max = arg2;
            }
            break;
        case SYS_OPEN:
        case SYS_OPENAT:
            current->trace_open_calls++;
            break;
        case SYS_CLOSE:
            current->trace_close_calls++;
            break;
        case SYS_STAT:
        case SYS_LSTAT:
        case SYS_FSTATAT:
        case SYS_ACCESS:
        case SYS_FACCESSAT:
            current->trace_stat_calls++;
            break;
        case SYS_FSTAT:
            current->trace_fstat_calls++;
            break;
        case SYS_LSEEK:
            current->trace_lseek_calls++;
            break;
        case SYS_IOCTL:
            current->trace_ioctl_calls++;
            break;
        case SYS_CLOCK_GETTIME:
            current->trace_clock_calls++;
            break;
        case SYS_GETTIMEOFDAY:
            current->trace_gettimeofday_calls++;
            break;
        default:
            break;
    }
#else
    (void)current;
    (void)syscall_no;
    (void)arg2;
#endif
}

#if ORTHOX_MEM_TRACE
static uint64_t align_up_page(uint64_t v) {
    return (v + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static int kernel_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int memtrace_current_enabled(void) {
    struct task* current = get_current_task();
    return current && kernel_streq(current->comm, "cc1");
}

static void memtrace_prefix(const char* tag) {
    struct task* current = get_current_task();
    puts("[memtrace] ");
    puts(tag);
    puts(" pid=0x"); puthex(current ? (uint64_t)current->pid : 0);
    puts(" brk=0x"); puthex(current ? current->heap_break : 0);
    puts(" mmap_end=0x"); puthex(current ? current->mmap_end : 0);
    puts(" pmm_alloc=0x"); puthex(pmm_get_allocated_pages());
    puts(" pmm_free=0x"); puthex(pmm_get_free_pages());
}
#endif

void syscall_memtrace_mmap(const char* tag, uint64_t addr, uint64_t length, uint64_t result,
                           int prot, int flags, int fd) {
#if ORTHOX_MEM_TRACE
    uint64_t pages = align_up_page(length) / PAGE_SIZE;
    if (!memtrace_current_enabled()) return;
    memtrace_prefix(tag);
    puts(" addr=0x"); puthex(addr);
    puts(" len=0x"); puthex(length);
    puts(" result=0x"); puthex(result);
    puts(" prot=0x"); puthex((uint64_t)prot);
    puts(" flags=0x"); puthex((uint64_t)flags);
    puts(" fd=0x"); puthex((uint64_t)(int64_t)fd);
    puts(" pages=0x"); puthex(pages);
    puts("\r\n");
#else
    (void)tag;
    (void)addr;
    (void)length;
    (void)result;
    (void)prot;
    (void)flags;
    (void)fd;
#endif
}
