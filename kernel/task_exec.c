#include <stdint.h>
#include <stddef.h>
#include "task_internal.h"
#include "pmm.h"
#include "vmm.h"
#include "elf64.h"
#include "fs.h"
#include "lapic.h"

#define EXEC_COPY_PAGES      260
#define EXEC_MAX_PATH_LEN    1024
#define EXEC_MAX_VEC_STRINGS 128
#define EXEC_MAX_VEC_STR_LEN 4096
#define EXEC_ET_DYN_LOAD_BASE 0x400000ULL
#define EXEC_INTERP_LOAD_BASE 0x7fc000000000ULL

#ifndef ORTHOX_MEM_TRACE
#define ORTHOX_MEM_TRACE 0
#endif

#ifndef ORTHOX_MEM_PROGRESS
#define ORTHOX_MEM_PROGRESS 0
#endif

extern void puts(const char* s);
extern void puthex(uint64_t v);
extern int fs_get_file_data(const char* path, void** data, size_t* size);
extern void fs_free_exec_buffer(const char* path, void* data, size_t size);

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = val & 0xFFFFFFFF;
    uint32_t high = val >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static void* kernel_memset(void* s, int c, size_t n) {
    unsigned char* p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

#if ORTHOX_MEM_PROGRESS || ORTHOX_MEM_TRACE
static int task_comm_is_name(const struct task* t, const char* name) {
    int i = 0;
    if (!t || !name) return 0;
    while (t->comm[i] && name[i]) {
        if (t->comm[i] != name[i]) return 0;
        i++;
    }
    return t->comm[i] == '\0' && name[i] == '\0';
}
#endif

struct exec_copy_buf {
    char path[EXEC_MAX_PATH_LEN];
    char argv_storage[EXEC_MAX_VEC_STRINGS][EXEC_MAX_VEC_STR_LEN];
    char envp_storage[EXEC_MAX_VEC_STRINGS][EXEC_MAX_VEC_STR_LEN];
    char* argv[EXEC_MAX_VEC_STRINGS + 1];
    char* envp[EXEC_MAX_VEC_STRINGS + 1];
};

static int copy_user_cstring(const char* src, char* dst, int size) {
    if (!src || !dst || size <= 0) return -1;
    if ((uint64_t)src < 0x1000ULL) return -1;
    int i = 0;
    for (; i + 1 < size; i++) {
        char ch = src[i];
        dst[i] = ch;
        if (!ch) return 0;
    }
    dst[size - 1] = '\0';
    return -1;
}

static int copy_user_string_vector(char* const user_vec[], char** kernel_vec,
                                   char storage[][EXEC_MAX_VEC_STR_LEN]) {
    int count = 0;
    if (!kernel_vec) return -1;
    if (!user_vec) {
        kernel_vec[0] = 0;
        return 0;
    }
    while (count < EXEC_MAX_VEC_STRINGS) {
        const char* src = user_vec[count];
        if (!src) break;
        if (copy_user_cstring(src, storage[count], EXEC_MAX_VEC_STR_LEN) < 0) {
            return -1;
        }
        kernel_vec[count] = storage[count];
        count++;
    }
    kernel_vec[count] = 0;
    return 0;
}

static const char* path_basename(const char* path) {
    const char* base = path;
    if (!path) return "";
    for (const char* p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    return base;
}

static int streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int resolve_interp_file(const char* interp_path, void** file_addr, size_t* file_size) {
    if (fs_get_file_data(interp_path, file_addr, file_size) == 0) {
        return 0;
    }

    const char* base = path_basename(interp_path);
    if (streq(base, "ld-musl-x86_64.so.1")) {
        return fs_get_file_data("/lib/ld-musl-x86_64.so.1", file_addr, file_size);
    }

    return -1;
}

static int alloc_user_stack(uint64_t* pml4_virt, struct task* t, int stack_pages, uint8_t* stack_pages_out[]) {
    if (!pml4_virt || !t || stack_pages <= USER_STACK_GUARD_PAGES) return -1;
    uint64_t stack_bottom_vaddr = USER_STACK_TOP_VADDR - (uint64_t)stack_pages * PAGE_SIZE;
    uint64_t mapped_bottom_vaddr = stack_bottom_vaddr + USER_STACK_GUARD_PAGES * PAGE_SIZE;
    for (int i = 0; i < stack_pages - USER_STACK_GUARD_PAGES; i++) {
        void* stack_phys = pmm_alloc(1);
        if (!stack_phys) return -1;
        uint8_t* stack_mem = (uint8_t*)PHYS_TO_VIRT(stack_phys);
        kernel_memset(stack_mem, 0, PAGE_SIZE);
        vmm_map_page(pml4_virt,
                     mapped_bottom_vaddr + (uint64_t)i * PAGE_SIZE,
                     (uint64_t)stack_phys,
                     PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        if (stack_pages_out) stack_pages_out[i] = stack_mem;
    }
    t->user_stack_top = USER_STACK_TOP_VADDR;
    t->user_stack_bottom = mapped_bottom_vaddr;
    t->user_stack_guard = stack_bottom_vaddr;
    return 0;
}

static int stack_write_bytes(uint8_t* stack_pages[], uint64_t mapped_bottom_vaddr,
                             uint64_t stack_top_vaddr, uint64_t user_addr,
                             const void* src, int len) {
    const uint8_t* in = (const uint8_t*)src;
    if (!stack_pages || !src || len < 0) return -1;
    if (user_addr < mapped_bottom_vaddr || user_addr + (uint64_t)len > stack_top_vaddr) return -1;
    uint64_t rel = user_addr - mapped_bottom_vaddr;
    int remaining = len;
    while (remaining > 0) {
        uint64_t page_index = rel / PAGE_SIZE;
        uint64_t page_off = rel % PAGE_SIZE;
        int chunk = (int)(PAGE_SIZE - page_off);
        if (chunk > remaining) chunk = remaining;
        if (!stack_pages[page_index]) return -1;
        for (int i = 0; i < chunk; i++) {
            stack_pages[page_index][page_off + (uint64_t)i] = in[i];
        }
        in += chunk;
        rel += (uint64_t)chunk;
        remaining -= chunk;
    }
    return 0;
}

static int stack_write_u64(uint8_t* stack_pages[], uint64_t mapped_bottom_vaddr,
                           uint64_t stack_top_vaddr, uint64_t user_addr, uint64_t value) {
    return stack_write_bytes(stack_pages, mapped_bottom_vaddr, stack_top_vaddr,
                             user_addr, &value, (int)sizeof(value));
}

static int copy_user_string_to_stack(uint8_t* stack_pages[], uint64_t mapped_bottom_vaddr,
                                     uint64_t stack_top_vaddr, uint64_t* current_sp,
                                     const char* src, uint64_t* user_addr_out) {
    if ((uint64_t)src < 0x1000ULL) {
        puts("Exec: invalid user string pointer ");
        puthex((uint64_t)src);
        puts("\r\n");
        return -1;
    }
    int len = 0;
    while (src[len]) len++;
    len++;
    if (*current_sp < mapped_bottom_vaddr + (uint64_t)len) return -1;
    *current_sp -= (uint64_t)len;
    if (*current_sp < mapped_bottom_vaddr || *current_sp >= stack_top_vaddr) return -1;
    if (stack_write_bytes(stack_pages, mapped_bottom_vaddr, stack_top_vaddr, *current_sp, src, len) < 0) {
        return -1;
    }
    *user_addr_out = *current_sp;
    return 0;
}

enum {
    AT_NULL = 0,
    AT_PHDR = 3,
    AT_PHENT = 4,
    AT_PHNUM = 5,
    AT_PAGESZ = 6,
    AT_ENTRY = 9,
    AT_UID = 11,
    AT_EUID = 12,
    AT_GID = 13,
    AT_EGID = 14,
    AT_SECURE = 23,
    AT_RANDOM = 25,
};

int task_prepare_initial_user_stack(uint64_t* pml4_virt, struct task* t,
                                    const struct elf_info* info,
                                    const struct elf_info* interp_info,
                                    char* const argv[], char* const envp[]) {
    uint8_t* stack_pages[USER_STACK_PAGES];
    for (int i = 0; i < USER_STACK_PAGES; i++) stack_pages[i] = 0;
    if (alloc_user_stack(pml4_virt, t, USER_STACK_PAGES, stack_pages) < 0) {
        puts("Exec: alloc_user_stack failed\r\n");
        return -1;
    }

    int argc = 0; if (argv) while (argv[argc]) argc++;
    int envc = 0; if (envp) while (envp[envc]) envc++;
    uint64_t env_ptrs[EXEC_MAX_VEC_STRINGS]; uint64_t arg_ptrs[EXEC_MAX_VEC_STRINGS];
    if (argc > EXEC_MAX_VEC_STRINGS) argc = EXEC_MAX_VEC_STRINGS;
    if (envc > EXEC_MAX_VEC_STRINGS) envc = EXEC_MAX_VEC_STRINGS;
    uint64_t current_str_addr = t->user_stack_top;
    current_str_addr -= 16;
    uint64_t random_base = current_str_addr;
    if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top,
                        random_base, 0x0123456789abcdefULL) < 0 ||
        stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top,
                        random_base + 8, 0xfedcba9876543210ULL) < 0) {
        return -1;
    }

    for (int i = envc - 1; i >= 0; i--) {
        if (copy_user_string_to_stack(stack_pages, t->user_stack_bottom, t->user_stack_top,
                                      &current_str_addr, envp[i], &env_ptrs[i]) < 0) {
            return -1;
        }
    }
    for (int i = argc - 1; i >= 0; i--) {
        if (copy_user_string_to_stack(stack_pages, t->user_stack_bottom, t->user_stack_top,
                                      &current_str_addr, argv[i], &arg_ptrs[i]) < 0) {
            return -1;
        }
    }

    current_str_addr &= ~7ULL;
    if (current_str_addr < t->user_stack_bottom + (uint64_t)(envc + argc + 32) * 8ULL) {
        return -1;
    }

    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, 0) < 0) return -1;
    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, AT_NULL) < 0) return -1;

    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, random_base) < 0) return -1;
    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, AT_RANDOM) < 0) return -1;

    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, 0) < 0) return -1;
    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, AT_SECURE) < 0) return -1;

    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, 0) < 0) return -1;
    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, AT_EGID) < 0) return -1;

    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, 0) < 0) return -1;
    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, AT_GID) < 0) return -1;

    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, 0) < 0) return -1;
    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, AT_EUID) < 0) return -1;

    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, 0) < 0) return -1;
    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, AT_UID) < 0) return -1;

    if (info->has_interp) {
        current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, interp_info ? interp_info->load_bias : 0) < 0) return -1;
        current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, 7 /* AT_BASE */) < 0) return -1;
    }

    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, (uint64_t)info->entry) < 0) return -1;
    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, AT_ENTRY) < 0) return -1;

    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, PAGE_SIZE) < 0) return -1;
    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, AT_PAGESZ) < 0) return -1;

    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, info->phnum) < 0) return -1;
    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, AT_PHNUM) < 0) return -1;

    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, info->phent) < 0) return -1;
    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, AT_PHENT) < 0) return -1;

    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, info->phdr_vaddr) < 0) return -1;
    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, AT_PHDR) < 0) return -1;

    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, 0) < 0) return -1;

    for (int i = envc - 1; i >= 0; i--) {
        current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, env_ptrs[i]) < 0) return -1;
    }
    t->user_envp = current_str_addr;

    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, 0) < 0) return -1;
    for (int i = argc - 1; i >= 0; i--) {
        current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, arg_ptrs[i]) < 0) return -1;
    }
    t->user_argv = current_str_addr;

    current_str_addr -= 8; if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, current_str_addr, (uint64_t)argc) < 0) return -1;
    t->user_argc = (uint64_t)argc;

    t->user_stack = current_str_addr;
    return 0;
}

int task_execve(struct syscall_frame* frame, const char* path, char* const argv[], char* const envp[]) {
    void* file_addr = NULL;
    size_t file_size = 0;
    void* exec_copy_phys = 0;
    struct exec_copy_buf* exec_copy = 0;
    uint64_t old_cr3 = 0;
    struct task* t = get_current_task();
    if (t && t->deferred_cr3 && t->deferred_cr3 != t->ctx.cr3 &&
        t->deferred_cr3 != vmm_get_kernel_pml4_phys()) {
        vmm_free_user_pml4(t->deferred_cr3);
        t->deferred_cr3 = 0;
    }
    exec_copy_phys = pmm_alloc(EXEC_COPY_PAGES);
    if (!exec_copy_phys) {
        return -1;
    }
    exec_copy = (struct exec_copy_buf*)PHYS_TO_VIRT(exec_copy_phys);
    kernel_memset(exec_copy, 0, EXEC_COPY_PAGES * PAGE_SIZE);
    if (copy_user_cstring(path, exec_copy->path, EXEC_MAX_PATH_LEN) < 0 ||
        copy_user_string_vector(argv, exec_copy->argv, exec_copy->argv_storage) < 0 ||
        copy_user_string_vector(envp, exec_copy->envp, exec_copy->envp_storage) < 0) {
        pmm_free(exec_copy_phys, EXEC_COPY_PAGES);
        return -1;
    }
    if (fs_get_file_data(exec_copy->path, &file_addr, &file_size) < 0) {
        puts("Exec: File not found: "); puts(exec_copy->path); puts("\r\n");
        pmm_free(exec_copy_phys, EXEC_COPY_PAGES);
        return -1;
    }
    void* pml4_phys = pmm_alloc(1);
    if (!pml4_phys) {
        puts("Exec: pml4 alloc failed\r\n");
        pmm_free(exec_copy_phys, EXEC_COPY_PAGES);
        return -1;
    }
    uint64_t* pml4_virt = (uint64_t*)PHYS_TO_VIRT(pml4_phys);
    uint64_t* kernel_pml4 = vmm_get_kernel_pml4();
    for (int i = 0; i < 512; i++) {
        pml4_virt[i] = (i >= 256) ? kernel_pml4[i] : 0;
    }
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)file_addr;
    uint64_t exec_load_bias = (ehdr->e_type == ET_DYN) ? EXEC_ET_DYN_LOAD_BASE : 0;
    struct elf_info info = elf_load(pml4_virt, file_addr, exec_load_bias);
    fs_free_exec_buffer(exec_copy->path, file_addr, file_size);
    file_addr = NULL;
    if (!info.entry) {
        pmm_free(exec_copy_phys, EXEC_COPY_PAGES);
        return -1;
    }

    struct elf_info interp_info;
    kernel_memset(&interp_info, 0, sizeof(interp_info));
    void* interp_file_addr = NULL;
    size_t interp_file_size = 0;

    if (info.has_interp) {
        if (resolve_interp_file(info.interp_path, &interp_file_addr, &interp_file_size) < 0) {
            puts("Exec: Interpreter not found: "); puts(info.interp_path); puts("\r\n");
            pmm_free(exec_copy_phys, EXEC_COPY_PAGES);
            return -1;
        }
        interp_info = elf_load(pml4_virt, interp_file_addr, EXEC_INTERP_LOAD_BASE);
        fs_free_exec_buffer(info.interp_path, interp_file_addr, interp_file_size);
        interp_file_addr = NULL;
        if (!interp_info.entry) {
            puts("Exec: Failed to load interpreter\r\n");
            pmm_free(exec_copy_phys, EXEC_COPY_PAGES);
            return -1;
        }
    }

    if (task_prepare_initial_user_stack(pml4_virt, t, &info, &interp_info, exec_copy->argv, exec_copy->envp) < 0) {
        pmm_free(exec_copy_phys, EXEC_COPY_PAGES);
        pmm_free(pml4_phys, 1);
        return -1;
    }

    old_cr3 = t->ctx.cr3;
    t->ctx.cr3 = (uint64_t)pml4_phys;
    task_set_comm_from_path(t, exec_copy->path);
#if ORTHOX_MEM_PROGRESS
    t->trace_progress = task_comm_is_name(t, "cc1");
    t->trace_started_ms = lapic_get_ticks_ms();
    t->trace_last_ms = 0;
    t->trace_syscalls = 0;
    t->trace_brk_calls = 0;
    t->trace_mmap_calls = 0;
    t->trace_munmap_calls = 0;
    t->trace_mremap_calls = 0;
    t->trace_read_calls = 0;
    t->trace_write_calls = 0;
    t->trace_read_bytes = 0;
    t->trace_write_bytes = 0;
    t->trace_write_max = 0;
    t->trace_open_calls = 0;
    t->trace_close_calls = 0;
    t->trace_stat_calls = 0;
    t->trace_fstat_calls = 0;
    t->trace_lseek_calls = 0;
    t->trace_ioctl_calls = 0;
    t->trace_clock_calls = 0;
    t->trace_gettimeofday_calls = 0;
    t->trace_cow_faults = 0;
#endif
#if ORTHOX_MEM_TRACE
    if (task_comm_is_name(t, "cc1")) {
        puts("[memtrace] exec cc1 pid=0x"); puthex((uint64_t)t->pid);
        puts(" path="); puts(exec_copy->path);
        puts(" file_size=0x"); puthex((uint64_t)file_size);
        puts("\r\n");
    }
#else
    (void)file_size;
#endif
    t->heap_break = info.max_vaddr;
    t->mmap_end = 0x4000000000ULL;
    t->user_entry = info.has_interp ? (uint64_t)interp_info.entry : (uint64_t)info.entry;
    t->sig_pending = 0;
    t->sig_mask = 0;
    for (int i = 0; i < 32; i++) {
        t->sig_handlers[i] = 0;
        t->sig_action_masks[i] = 0;
        t->sig_action_flags[i] = 0;
    }
    t->user_fs_base = 0;
    t->tls_vaddr = info.tls_vaddr;
    t->tls_filesz = info.tls_filesz;
    t->tls_memsz = info.tls_memsz;
    t->tls_align = info.tls_align;
    for (int i = 0; i < 512; i++) t->ctx.fxsave_area[i] = 0;
    t->ctx.fxsave_area[0] = 0x7f;
    t->ctx.fxsave_area[1] = 0x03;
    t->ctx.fxsave_area[24] = 0x80;
    t->ctx.fxsave_area[25] = 0x1f;
    fs_close_cloexec_descriptors(t);
    frame->r15 = 0;
    frame->r14 = 0;
    frame->r13 = 0;
    frame->r12 = 0;
    frame->rbp = 0;
    frame->rbx = 0;
    frame->r9 = 0;
    frame->r8 = 0;
    frame->r10 = 0;
    frame->rax = 0;
    frame->rip = t->user_entry;
    frame->cs = 0x23;
    frame->ss = 0x1B;
    frame->rsp = t->user_stack;
    frame->rflags = 0x202ULL;
    frame->rdi = t->user_argc;
    frame->rsi = t->user_argv;
    frame->rdx = t->user_envp;
    __asm__ volatile("mov %0, %%cr3" : : "r"(t->ctx.cr3) : "memory");
    wrmsr(MSR_FS_BASE, t->user_fs_base);
    __asm__ volatile("fninit");
    if (old_cr3 && old_cr3 != t->ctx.cr3 && old_cr3 != vmm_get_kernel_pml4_phys()) {
        t->deferred_cr3 = old_cr3;
    }
    pmm_free(exec_copy_phys, EXEC_COPY_PAGES);
    return 0;
}
