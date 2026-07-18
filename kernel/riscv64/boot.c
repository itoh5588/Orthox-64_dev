#include <stdint.h>
#include "riscv64/boot.h"
#include "riscv64/csr.h"
#include "riscv64/dtb.h"
#include "riscv64/elf.h"
#include "riscv64/entry.h"
#include "riscv64/syscall.h"
#include "riscv64/task.h"
#include "riscv64/trap.h"
#include "riscv64/vm.h"
#include "elf64.h"
#include "pmm.h"
#include "syscall.h"
#include "task.h"

extern int task_execve(arch_task_exec_frame_t* frame, const char* path,
                       char* const argv[], char* const envp[]);
extern int task_fork(arch_task_exec_frame_t* frame);
extern void task_main(void);

static volatile riscv64_boot_info_t g_riscv64_boot_info;
static volatile int g_riscv64_user_handoff_started;

#ifndef RISCV64_BOOTSTRAP_ARG0
#define RISCV64_BOOTSTRAP_ARG0 "pwd"
#endif
#ifdef RISCV64_BOOTSTRAP_ARG1
static char g_riscv64_bootstrap_argv1[] = RISCV64_BOOTSTRAP_ARG1;
#endif
#ifdef RISCV64_BOOTSTRAP_ARG2
static char g_riscv64_bootstrap_argv2[] = RISCV64_BOOTSTRAP_ARG2;
#endif
#ifdef RISCV64_BOOTSTRAP_ARG3
static char g_riscv64_bootstrap_argv3[] = RISCV64_BOOTSTRAP_ARG3;
#endif

static char g_riscv64_bootstrap_path[] = "/bootstrap-user";
static char g_riscv64_bootstrap_argv0[] = RISCV64_BOOTSTRAP_ARG0;
static char* g_riscv64_bootstrap_argv[] = {
    g_riscv64_bootstrap_argv0,
#ifdef RISCV64_BOOTSTRAP_ARG1
    g_riscv64_bootstrap_argv1,
#endif
#ifdef RISCV64_BOOTSTRAP_ARG2
    g_riscv64_bootstrap_argv2,
#endif
#ifdef RISCV64_BOOTSTRAP_ARG3
    g_riscv64_bootstrap_argv3,
#endif
    0
};
static char g_riscv64_bootstrap_env0[] = "PWD=/";
static char g_riscv64_bootstrap_env1[] = "PATH=/bin";
static char* g_riscv64_bootstrap_envp[] = { g_riscv64_bootstrap_env0, g_riscv64_bootstrap_env1, 0 };

extern struct task* task_list;

static uint64_t riscv64_boot_stack_top(void) {
    uint64_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    return (sp & ~(4096ULL - 1ULL)) + 4096ULL;
}

#define RISCV64_UART_RHR 0
#define RISCV64_UART_THR 0
#define RISCV64_UART_IER 1
#define RISCV64_UART_FCR 2
#define RISCV64_UART_LCR 3
#define RISCV64_UART_LSR 5

#define RISCV64_UART_LCR_DLAB 0x80U
#define RISCV64_UART_LCR_8N1  0x03U
#define RISCV64_UART_FCR_FIFO_ENABLE 0x01U
#define RISCV64_UART_FCR_FIFO_CLEAR  0x06U
#define RISCV64_UART_LSR_RX_READY    0x01U
#define RISCV64_UART_LSR_TX_IDLE     0x20U

static inline volatile uint8_t* riscv64_uart0_regs(void) {
    return (volatile uint8_t*)(uintptr_t)RISCV64_QEMU_VIRT_UART0_BASE;
}

static uint32_t riscv64_align_up4(uint32_t value) {
    return (value + 3U) & ~3U;
}

static int riscv64_str_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int riscv64_dtb_compatible_has(const char* data, uint32_t len, const char* needle) {
    uint32_t i = 0;
    if (!data || !needle) return 0;
    while (i < len) {
        const char* entry = data + i;
        uint32_t entry_len = 0;
        while (i + entry_len < len && entry[entry_len] != '\0') entry_len++;
        if (i + entry_len >= len) break;
        if (riscv64_str_eq(entry, needle)) return 1;
        i += entry_len + 1U;
    }
    return 0;
}

static uint64_t riscv64_dtb_reg_base(const uint8_t* data, uint32_t len) {
    if (!data) return 0;
    if (len >= 16U) return riscv64_dtb_read_be64(data);
    if (len >= 8U) return (uint64_t)riscv64_dtb_read_be32(data);
    return 0;
}

static uint64_t riscv64_dtb_reg_size(const uint8_t* data, uint32_t len) {
    if (!data) return 0;
    if (len >= 16U) return riscv64_dtb_read_be64(data + 8U);
    if (len >= 8U) return (uint64_t)riscv64_dtb_read_be32(data + 4U);
    return 0;
}

static void riscv64_dtb_scan(uint64_t dtb_pa) {
    const riscv64_fdt_header_t* header = (const riscv64_fdt_header_t*)(uintptr_t)dtb_pa;
    uint32_t struct_off;
    uint32_t struct_size;
    uint32_t strings_off;
    const uint8_t* struct_base;
    const char* strings_base;
    uint32_t off = 0;
    int depth = 0;
    int candidate_uart = 0;
    int candidate_virtio = 0;
    int candidate_memory = 0;
    uint64_t candidate_reg = 0;
    uint64_t candidate_size = 0;

    if (!riscv64_dtb_valid(dtb_pa)) return;

    struct_off = riscv64_dtb_read_be32(&header->off_dt_struct);
    struct_size = riscv64_dtb_read_be32(&header->size_dt_struct);
    strings_off = riscv64_dtb_read_be32(&header->off_dt_strings);
    struct_base = (const uint8_t*)(uintptr_t)(dtb_pa + struct_off);
    strings_base = (const char*)(uintptr_t)(dtb_pa + strings_off);

    while (off + 4U <= struct_size) {
        uint32_t token = riscv64_dtb_read_be32(struct_base + off);
        off += 4U;

        if (token == RISCV64_FDT_BEGIN_NODE) {
            const char* node_name = (const char*)(struct_base + off);
            while (off < struct_size && struct_base[off] != 0) off++;
            off++;
            off = riscv64_align_up4(off);
            depth++;
            candidate_uart = 0;
            candidate_virtio = 0;
            candidate_memory = 0;
            candidate_reg = 0;
            candidate_size = 0;
            (void)node_name;
            continue;
        }

        if (token == RISCV64_FDT_END_NODE) {
            if (candidate_uart && candidate_reg != 0) {
                g_riscv64_boot_info.uart_base = candidate_reg;
                g_riscv64_boot_info.flags |= RISCV64_BOOT_FLAG_UART_FROM_DTB;
            }
            if (candidate_virtio && candidate_reg != 0) {
                if (g_riscv64_boot_info.first_virtio_mmio_base == 0) {
                    g_riscv64_boot_info.first_virtio_mmio_base = candidate_reg;
                }
                g_riscv64_boot_info.virtio_mmio_count++;
                g_riscv64_boot_info.flags |= RISCV64_BOOT_FLAG_VIRTIO_MMIO_FOUND;
            }
            if (candidate_memory && candidate_reg != 0 && candidate_size != 0 &&
                g_riscv64_boot_info.memory_size == 0) {
                g_riscv64_boot_info.memory_base = candidate_reg;
                g_riscv64_boot_info.memory_size = candidate_size;
            }
            if (depth > 0) depth--;
            candidate_uart = 0;
            candidate_virtio = 0;
            candidate_memory = 0;
            candidate_reg = 0;
            candidate_size = 0;
            continue;
        }

        if (token == RISCV64_FDT_PROP) {
            uint32_t len;
            uint32_t nameoff;
            const uint8_t* data;
            const char* prop_name;
            if (off + 8U > struct_size) break;
            len = riscv64_dtb_read_be32(struct_base + off);
            nameoff = riscv64_dtb_read_be32(struct_base + off + 4U);
            off += 8U;
            if (off + riscv64_align_up4(len) > struct_size) break;
            data = struct_base + off;
            prop_name = strings_base + nameoff;

            if (depth >= 1 && riscv64_str_eq(prop_name, "compatible")) {
                if (riscv64_dtb_compatible_has((const char*)data, len, "ns16550a") ||
                    riscv64_dtb_compatible_has((const char*)data, len, "ns16550")) {
                    candidate_uart = 1;
                }
                if (riscv64_dtb_compatible_has((const char*)data, len, "virtio,mmio")) {
                    candidate_virtio = 1;
                }
            } else if (depth >= 1 && riscv64_str_eq(prop_name, "device_type")) {
                if (len >= 7U && riscv64_str_eq((const char*)data, "memory")) {
                    candidate_memory = 1;
                }
            } else if (depth >= 1 && riscv64_str_eq(prop_name, "reg")) {
                candidate_reg = riscv64_dtb_reg_base(data, len);
                candidate_size = riscv64_dtb_reg_size(data, len);
            }

            off += riscv64_align_up4(len);
            continue;
        }

        if (token == RISCV64_FDT_NOP) continue;
        if (token == RISCV64_FDT_END) break;
        break;
    }
}

uint32_t riscv64_dtb_read_be32(const void* addr) {
    const uint8_t* p = (const uint8_t*)addr;
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

uint64_t riscv64_dtb_read_be64(const void* addr) {
    const uint8_t* p = (const uint8_t*)addr;
    return ((uint64_t)p[0] << 56) |
           ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) |
           ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) |
           ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) |
           (uint64_t)p[7];
}

int riscv64_dtb_valid(uint64_t dtb_pa) {
    const riscv64_fdt_header_t* header = (const riscv64_fdt_header_t*)(uintptr_t)dtb_pa;
    if (!dtb_pa) return 0;
    return riscv64_dtb_read_be32(&header->magic) == RISCV64_FDT_MAGIC;
}

uint32_t riscv64_dtb_total_size(uint64_t dtb_pa) {
    const riscv64_fdt_header_t* header = (const riscv64_fdt_header_t*)(uintptr_t)dtb_pa;
    if (!riscv64_dtb_valid(dtb_pa)) return 0;
    return riscv64_dtb_read_be32(&header->totalsize);
}

void riscv64_boot_capture(uint64_t hart_id, uint64_t dtb_pa) {
    g_riscv64_boot_info.hart_id = hart_id;
    g_riscv64_boot_info.dtb_pa = dtb_pa;
    g_riscv64_boot_info.memory_base = 0;
    g_riscv64_boot_info.memory_size = 0;
    g_riscv64_boot_info.uart_base = RISCV64_QEMU_VIRT_UART0_BASE;
    g_riscv64_boot_info.first_virtio_mmio_base = 0;
    g_riscv64_boot_info.dtb_size = riscv64_dtb_total_size(dtb_pa);
    g_riscv64_boot_info.virtio_mmio_count = 0;
    g_riscv64_boot_info.flags = 0;
    if (g_riscv64_boot_info.dtb_size != 0) {
        g_riscv64_boot_info.flags |= RISCV64_BOOT_FLAG_DTB_VALID;
        riscv64_dtb_scan(dtb_pa);
    }
}

const riscv64_boot_info_t* riscv64_boot_info(void) {
    return (const riscv64_boot_info_t*)&g_riscv64_boot_info;
}

void riscv64_mark_user_handoff_started(void) {
    g_riscv64_user_handoff_started = 1;
}

int riscv64_user_handoff_started(void) {
    return g_riscv64_user_handoff_started;
}

void riscv64_uart_init(void) {
    volatile uint8_t* uart = riscv64_uart0_regs();

    uart[RISCV64_UART_IER] = 0x00U;
    uart[RISCV64_UART_LCR] = RISCV64_UART_LCR_DLAB;
    uart[RISCV64_UART_RHR] = 0x03U;
    uart[RISCV64_UART_IER] = 0x00U;
    uart[RISCV64_UART_LCR] = RISCV64_UART_LCR_8N1;
    uart[RISCV64_UART_FCR] = RISCV64_UART_FCR_FIFO_ENABLE | RISCV64_UART_FCR_FIFO_CLEAR;

    g_riscv64_boot_info.flags |= RISCV64_BOOT_FLAG_UART_READY;
}

void riscv64_uart_putchar(char ch) {
    volatile uint8_t* uart = riscv64_uart0_regs();
    while ((uart[RISCV64_UART_LSR] & RISCV64_UART_LSR_TX_IDLE) == 0) {
    }
    uart[RISCV64_UART_THR] = (uint8_t)ch;
}

int riscv64_uart_getchar_nonblock(void) {
    volatile uint8_t* uart = riscv64_uart0_regs();
    if ((uart[RISCV64_UART_LSR] & RISCV64_UART_LSR_RX_READY) == 0) return -1;
    return (int)uart[RISCV64_UART_RHR];
}

void riscv64_uart_puts(const char* s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') riscv64_uart_putchar('\r');
        riscv64_uart_putchar(*s++);
    }
}

void riscv64_uart_puthex64(uint64_t value) {
    static const char hex[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        riscv64_uart_putchar(hex[(value >> shift) & 0xfU]);
    }
}

void riscv64_wait_forever(void) {
    for (;;) {
        __asm__ volatile("wfi");
    }
}

static void riscv64_vm_selftest(void) {
    uint64_t aspace = riscv64_vm_create_address_space();
    uint64_t virt = 0x40000000ULL;
    uint64_t phys;
    uint64_t mapped;
    static const uint8_t payload[] = { 'R', 'V', 'E', 'L', 'F', '!', 0 };

    if (!aspace) {
        riscv64_uart_puts("  vm selftest: create failed\n");
        return;
    }

    if (riscv64_elf_load_segment_bootstrap(aspace, virt, payload, sizeof(payload), 128,
                                           RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_W) < 0) {
        riscv64_uart_puts("  vm selftest: bootstrap load failed\n");
        riscv64_vm_destroy_address_space(aspace);
        return;
    }
    mapped = riscv64_vm_get_phys(aspace, virt);
    if (!mapped) {
        riscv64_uart_puts("  vm selftest: map/get_phys failed\n");
        riscv64_vm_destroy_address_space(aspace);
        return;
    }

    phys = mapped & ~(4096ULL - 1ULL);
    if (*(volatile uint8_t*)(uintptr_t)mapped != 'R' ||
        *(volatile uint8_t*)(uintptr_t)(mapped + 5) != '!') {
        riscv64_uart_puts("  vm selftest: payload copy failed\n");
        riscv64_vm_bootstrap_free_page(phys);
        riscv64_vm_destroy_address_space(aspace);
        return;
    }

    riscv64_vm_update_page_flags(aspace, virt, RISCV64_VM_PAGE_R);
    mapped = riscv64_vm_get_phys(aspace, virt);
    if ((mapped & ~(4096ULL - 1ULL)) != phys) {
        riscv64_uart_puts("  vm selftest: update flags failed\n");
        riscv64_vm_bootstrap_free_page(phys);
        riscv64_vm_destroy_address_space(aspace);
        return;
    }

    riscv64_vm_unmap_page(aspace, virt);
    if (riscv64_vm_get_phys(aspace, virt) != 0) {
        riscv64_uart_puts("  vm selftest: unmap failed\n");
        riscv64_vm_bootstrap_free_page(phys);
        riscv64_vm_destroy_address_space(aspace);
        return;
    }

    riscv64_vm_bootstrap_free_page(phys);
    riscv64_vm_destroy_address_space(aspace);
    riscv64_uart_puts("  vm selftest passed\n");
}

static void riscv64_vm_clone_selftest(void) {
    uint64_t parent = riscv64_vm_create_address_space();
    uint64_t child = 0;
    uint64_t virt = 0x0000000041000000ULL;
    uint64_t parent_phys;
    uint64_t child_phys;

    if (!parent) {
        riscv64_uart_puts("  vm clone selftest: create failed\n");
        return;
    }
    parent_phys = (uint64_t)(uintptr_t)pmm_alloc(1);
    if (!parent_phys) {
        riscv64_uart_puts("  vm clone selftest: page alloc failed\n");
        riscv64_vm_destroy_address_space(parent);
        return;
    }
    ((volatile uint8_t*)(uintptr_t)parent_phys)[0] = 'P';
    ((volatile uint8_t*)(uintptr_t)parent_phys)[1] = '0';
    riscv64_vm_map_page(parent, virt, parent_phys, RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_W | RISCV64_VM_PAGE_U);

    child = (uint64_t)arch_vm_clone_address_space((arch_address_space_t)parent);
    if (!child) {
        riscv64_uart_puts("  vm clone selftest: clone failed\n");
        riscv64_vm_destroy_address_space(parent);
        return;
    }

    child_phys = riscv64_vm_get_phys(child, virt) & ~(PAGE_SIZE - 1ULL);
    parent_phys = riscv64_vm_get_phys(parent, virt) & ~(PAGE_SIZE - 1ULL);
    if (!child_phys || !parent_phys || child_phys == parent_phys) {
        riscv64_uart_puts("  vm clone selftest: isolated page missing\n");
        riscv64_vm_destroy_address_space(child);
        riscv64_vm_destroy_address_space(parent);
        return;
    }
    if (((volatile uint8_t*)(uintptr_t)child_phys)[0] != 'P' ||
        ((volatile uint8_t*)(uintptr_t)child_phys)[1] != '0') {
        riscv64_uart_puts("  vm clone selftest: clone copy failed\n");
        riscv64_vm_destroy_address_space(child);
        riscv64_vm_destroy_address_space(parent);
        return;
    }

    ((volatile uint8_t*)(uintptr_t)child_phys)[1] = '1';
    if (((volatile uint8_t*)(uintptr_t)parent_phys)[1] != '0') {
        riscv64_uart_puts("  vm clone selftest: parent mutated\n");
        riscv64_vm_destroy_address_space(child);
        riscv64_vm_destroy_address_space(parent);
        return;
    }

    riscv64_vm_destroy_address_space(child);
    riscv64_vm_destroy_address_space(parent);
    riscv64_uart_puts("  vm clone selftest passed\n");
}

static void riscv64_user_stack_selftest(void) {
    struct task temp_task;
    struct elf_info info;
    char arg0[] = "bootstrap";
    char* argv[] = { arg0, 0 };
    char env0[] = "PWD=/";
    char* envp[] = { env0, 0 };
    uint64_t aspace = riscv64_vm_create_address_space();
    uint64_t sp_phys;
    uint64_t argc_at_sp;

    if (!aspace) {
        riscv64_uart_puts("  user stack selftest: create failed\n");
        return;
    }
    for (uint64_t i = 0; i < sizeof(temp_task); i++) ((uint8_t*)&temp_task)[i] = 0;
    info.entry = (void*)(uintptr_t)0x0000000000400000ULL;
    info.max_vaddr = 0x0000000000401000ULL;
    info.phdr_vaddr = 0x0000000000400040ULL;
    info.phent = sizeof(Elf64_Phdr);
    info.phnum = 1;

    if (task_prepare_initial_user_stack((arch_address_space_t)aspace, &temp_task, &info, argv, envp) < 0) {
        riscv64_uart_puts("  user stack selftest: prepare failed\n");
        riscv64_vm_destroy_address_space(aspace);
        return;
    }

    sp_phys = arch_vm_get_phys((arch_address_space_t)aspace, temp_task.user_stack);
    if (!sp_phys) {
        riscv64_uart_puts("  user stack selftest: sp unmapped\n");
        riscv64_vm_destroy_address_space(aspace);
        return;
    }
    argc_at_sp = *(volatile uint64_t*)(uintptr_t)sp_phys;
    if (argc_at_sp != 1 || temp_task.user_stack_guard == 0 || temp_task.user_stack_bottom == 0) {
        riscv64_uart_puts("  user stack selftest: bad layout\n");
        riscv64_vm_destroy_address_space(aspace);
        return;
    }

    riscv64_vm_destroy_address_space(aspace);
    riscv64_uart_puts("  user stack selftest passed\n");
}

static void riscv64_user_frame_selftest(void) {
    struct arch_task_context ctx;
    struct arch_task_user_state state;
    riscv64_trap_frame_t trap_frame;
    const uint64_t entry = 0x0000000040001000ULL;
    const uint64_t sp = 0x000000007ffff000ULL;
    const uint64_t arg0 = 3;
    const uint64_t arg1 = 0x000000007fffef00ULL;
    const uint64_t arg2 = 0x000000007fffef80ULL;

    for (uint64_t i = 0; i < sizeof(ctx); i++) {
        ((uint8_t*)&ctx)[i] = 0;
    }

    state.entry_pc = entry;
    state.user_sp = sp;
    state.arg0 = arg0;
    state.arg1 = arg1;
    state.arg2 = arg2;

    riscv64_task_prepare_initial_user_frame(&ctx.user_frame, &state);

    if (ctx.user_frame.sepc != entry ||
        ctx.user_frame.sp != sp ||
        ctx.user_frame.a0 != arg0 ||
        ctx.user_frame.a1 != arg1 ||
        ctx.user_frame.a2 != arg2) {
        riscv64_uart_puts("  user frame selftest failed\n");
        return;
    }

    if ((ctx.user_frame.sstatus & RISCV64_SSTATUS_SPP) != 0 ||
        (ctx.user_frame.sstatus & RISCV64_SSTATUS_SPIE) == 0) {
        riscv64_uart_puts("  user frame selftest flags failed\n");
        return;
    }

    riscv64_uart_puts("  user frame selftest passed\n");

    for (uint64_t i = 0; i < sizeof(trap_frame); i++) {
        ((uint8_t*)&trap_frame)[i] = 0;
    }
    trap_frame.sepc = entry + 4;
    trap_frame.sp = sp - 16;
    trap_frame.a0 = 42;
    trap_frame.a1 = arg1 + 8;
    trap_frame.a2 = arg2 + 8;
    trap_frame.sstatus = ctx.user_frame.sstatus;

    riscv64_syscall_set_current_context(&ctx);
    riscv64_syscall_sync_current_user_frame(&trap_frame);
    riscv64_syscall_set_current_context(0);

    if (ctx.user_frame.sepc != trap_frame.sepc ||
        ctx.user_frame.sp != trap_frame.sp ||
        ctx.user_frame.a0 != trap_frame.a0 ||
        ctx.user_frame.a1 != trap_frame.a1 ||
        ctx.user_frame.a2 != trap_frame.a2) {
        riscv64_uart_puts("  user frame sync selftest failed\n");
        return;
    }

    riscv64_uart_puts("  user frame sync selftest passed\n");
}

static void riscv64_syscall_dispatch_selftest(void) {
    task_context_t* ctx = task_current_context();
    riscv64_trap_frame_t frame;
    char cwd_buf[8];
    static const char write_buf[] = "SOK\n";

    if (!ctx) {
        riscv64_uart_puts("  syscall selftest: no current context\n");
        return;
    }

    for (uint64_t i = 0; i < sizeof(frame); i++) {
        ((uint8_t*)&frame)[i] = 0;
    }
    for (uint64_t i = 0; i < sizeof(cwd_buf); i++) {
        cwd_buf[i] = 0;
    }

    frame.sepc = 0x0000000040002000ULL;
    frame.sp = 0x000000007ffff000ULL;
    frame.sstatus = ctx->user_frame.sstatus;
    frame.a0 = (uint64_t)(uintptr_t)cwd_buf;
    frame.a1 = sizeof(cwd_buf);
    frame.a7 = SYS_GETCWD;

    riscv64_syscall_dispatch(&frame);

    if (frame.sepc != 0x0000000040002004ULL ||
        frame.a0 != (uint64_t)(uintptr_t)cwd_buf ||
        cwd_buf[0] != '/' ||
        cwd_buf[1] != '\0' ||
        ctx->user_frame.sepc != frame.sepc) {
        riscv64_uart_puts("  syscall selftest: getcwd failed\n");
        return;
    }

    for (uint64_t i = 0; i < sizeof(frame); i++) {
        ((uint8_t*)&frame)[i] = 0;
    }
    frame.sepc = 0x0000000040003000ULL;
    frame.sp = 0x000000007ffff000ULL;
    frame.sstatus = ctx->user_frame.sstatus;
    frame.a7 = SYS_GETPID;

    riscv64_syscall_dispatch(&frame);

    if (frame.sepc != 0x0000000040003004ULL || frame.a0 == 0) {
        riscv64_uart_puts("  syscall selftest: getpid failed\n");
        return;
    }

    for (uint64_t i = 0; i < sizeof(frame); i++) {
        ((uint8_t*)&frame)[i] = 0;
    }
    frame.sepc = 0x0000000040004000ULL;
    frame.sp = 0x000000007ffff000ULL;
    frame.sstatus = ctx->user_frame.sstatus;
    frame.a0 = 1;
    frame.a1 = (uint64_t)(uintptr_t)write_buf;
    frame.a2 = 4;
    frame.a7 = SYS_WRITE;

    riscv64_syscall_dispatch(&frame);

    if (frame.sepc != 0x0000000040004004ULL || frame.a0 != 4) {
        riscv64_uart_puts("  syscall selftest: write failed\n");
        return;
    }

    riscv64_uart_puts("  syscall dispatch selftest passed\n");
}

static void riscv64_fs_syscall_selftest(void) {
    task_context_t* ctx = task_current_context();
    riscv64_trap_frame_t frame;
    struct kstat st;
    struct kstat st2;
    char read_buf[8];
    static const char root_path[] = "/";
    static const char bootstrap_path[] = "/bootstrap-user";
    static const char relative_bootstrap[] = "bootstrap-user";
    int fd;
    int dirfd;

    if (!ctx) {
        riscv64_uart_puts("  fs syscall selftest: no current context\n");
        return;
    }

    for (uint64_t i = 0; i < sizeof(st); i++) ((uint8_t*)&st)[i] = 0;
    for (uint64_t i = 0; i < sizeof(st2); i++) ((uint8_t*)&st2)[i] = 0;
    for (uint64_t i = 0; i < sizeof(read_buf); i++) read_buf[i] = 0;
    for (uint64_t i = 0; i < sizeof(frame); i++) ((uint8_t*)&frame)[i] = 0;

    frame.sepc = 0x0000000040005000ULL;
    frame.sp = 0x000000007ffff000ULL;
    frame.sstatus = ctx->user_frame.sstatus;
    frame.a0 = (uint64_t)(uintptr_t)root_path;
    frame.a7 = SYS_CHDIR;
    riscv64_syscall_dispatch(&frame);
    if ((int64_t)frame.a0 != 0) {
        riscv64_uart_puts("  fs syscall selftest: chdir failed\n");
        return;
    }

    for (uint64_t i = 0; i < sizeof(frame); i++) ((uint8_t*)&frame)[i] = 0;
    frame.sepc = 0x0000000040005010ULL;
    frame.sp = 0x000000007ffff000ULL;
    frame.sstatus = ctx->user_frame.sstatus;
    frame.a0 = (uint64_t)(uintptr_t)bootstrap_path;
    frame.a1 = (uint64_t)(uintptr_t)&st;
    frame.a7 = SYS_STAT;
    riscv64_syscall_dispatch(&frame);
    if ((int64_t)frame.a0 != 0 || (st.mode & 0170000U) != KSTAT_MODE_FILE || st.size < 4) {
        riscv64_uart_puts("  fs syscall selftest: stat failed\n");
        return;
    }

    for (uint64_t i = 0; i < sizeof(frame); i++) ((uint8_t*)&frame)[i] = 0;
    frame.sepc = 0x0000000040005018ULL;
    frame.sp = 0x000000007ffff000ULL;
    frame.sstatus = ctx->user_frame.sstatus;
    frame.a0 = (uint64_t)(uintptr_t)root_path;
    frame.a1 = O_DIRECTORY;
    frame.a2 = 0;
    frame.a7 = SYS_OPEN;
    riscv64_syscall_dispatch(&frame);
    dirfd = (int)frame.a0;
    if (dirfd < 3) {
        riscv64_uart_puts("  fs syscall selftest: open dir failed\n");
        return;
    }

    for (uint64_t i = 0; i < sizeof(frame); i++) ((uint8_t*)&frame)[i] = 0;
    frame.sepc = 0x000000004000501cULL;
    frame.sp = 0x000000007ffff000ULL;
    frame.sstatus = ctx->user_frame.sstatus;
    frame.a0 = (uint64_t)dirfd;
    frame.a1 = (uint64_t)(uintptr_t)relative_bootstrap;
    frame.a2 = 0;
    frame.a3 = 0;
    frame.a7 = SYS_OPENAT;
    riscv64_syscall_dispatch(&frame);
    fd = (int)frame.a0;
    if (fd < 3) {
        riscv64_uart_puts("  fs syscall selftest: openat failed\n");
        return;
    }

    for (uint64_t i = 0; i < sizeof(frame); i++) ((uint8_t*)&frame)[i] = 0;
    frame.sepc = 0x000000004000501eULL;
    frame.sp = 0x000000007ffff000ULL;
    frame.sstatus = ctx->user_frame.sstatus;
    frame.a0 = (uint64_t)fd;
    frame.a1 = (uint64_t)(uintptr_t)&st2;
    frame.a7 = SYS_FSTAT;
    riscv64_syscall_dispatch(&frame);
    if ((int64_t)frame.a0 != 0 || st2.size != st.size || st2.mode != st.mode) {
        riscv64_uart_puts("  fs syscall selftest: fstat failed\n");
        return;
    }

    for (uint64_t i = 0; i < sizeof(frame); i++) ((uint8_t*)&frame)[i] = 0;
    frame.sepc = 0x000000004000501fULL;
    frame.sp = 0x000000007ffff000ULL;
    frame.sstatus = ctx->user_frame.sstatus;
    frame.a0 = (uint64_t)dirfd;
    frame.a1 = (uint64_t)(uintptr_t)relative_bootstrap;
    frame.a2 = (uint64_t)(uintptr_t)&st2;
    frame.a3 = 0;
    frame.a7 = SYS_FSTATAT;
    riscv64_syscall_dispatch(&frame);
    if ((int64_t)frame.a0 != 0 || st2.size != st.size || st2.mode != st.mode) {
        riscv64_uart_puts("  fs syscall selftest: fstatat failed\n");
        return;
    }

    for (uint64_t i = 0; i < sizeof(frame); i++) ((uint8_t*)&frame)[i] = 0;
    frame.sepc = 0x000000004000501fULL + 4;
    frame.sp = 0x000000007ffff000ULL;
    frame.sstatus = ctx->user_frame.sstatus;
    frame.a0 = (uint64_t)dirfd;
    frame.a7 = SYS_FCHDIR;
    riscv64_syscall_dispatch(&frame);
    if ((int64_t)frame.a0 != 0) {
        riscv64_uart_puts("  fs syscall selftest: fchdir failed\n");
        return;
    }

    for (uint64_t i = 0; i < sizeof(frame); i++) ((uint8_t*)&frame)[i] = 0;
    frame.sepc = 0x0000000040005020ULL;
    frame.sp = 0x000000007ffff000ULL;
    frame.sstatus = ctx->user_frame.sstatus;
    frame.a0 = (uint64_t)(uintptr_t)bootstrap_path;
    frame.a1 = 0;
    frame.a2 = 0;
    frame.a7 = SYS_OPEN;
    riscv64_syscall_dispatch(&frame);
    fd = (int)frame.a0;
    if (fd < 3) {
        riscv64_uart_puts("  fs syscall selftest: open failed\n");
        return;
    }

    for (uint64_t i = 0; i < sizeof(frame); i++) ((uint8_t*)&frame)[i] = 0;
    frame.sepc = 0x0000000040005030ULL;
    frame.sp = 0x000000007ffff000ULL;
    frame.sstatus = ctx->user_frame.sstatus;
    frame.a0 = (uint64_t)fd;
    frame.a1 = (uint64_t)(uintptr_t)read_buf;
    frame.a2 = 4;
    frame.a7 = SYS_READ;
    riscv64_syscall_dispatch(&frame);
    if ((int64_t)frame.a0 != 4 || read_buf[0] != 0x7f || read_buf[1] != 'E' ||
        read_buf[2] != 'L' || read_buf[3] != 'F') {
        riscv64_uart_puts("  fs syscall selftest: read failed\n");
        return;
    }

    for (uint64_t i = 0; i < sizeof(frame); i++) ((uint8_t*)&frame)[i] = 0;
    frame.sepc = 0x0000000040005040ULL;
    frame.sp = 0x000000007ffff000ULL;
    frame.sstatus = ctx->user_frame.sstatus;
    frame.a0 = (uint64_t)fd;
    frame.a7 = SYS_CLOSE;
    riscv64_syscall_dispatch(&frame);
    if ((int64_t)frame.a0 != 0) {
        riscv64_uart_puts("  fs syscall selftest: close failed\n");
        return;
    }

    for (uint64_t i = 0; i < sizeof(frame); i++) ((uint8_t*)&frame)[i] = 0;
    frame.sepc = 0x0000000040005044ULL;
    frame.sp = 0x000000007ffff000ULL;
    frame.sstatus = ctx->user_frame.sstatus;
    frame.a0 = (uint64_t)dirfd;
    frame.a7 = SYS_CLOSE;
    riscv64_syscall_dispatch(&frame);
    if ((int64_t)frame.a0 != 0) {
        riscv64_uart_puts("  fs syscall selftest: close dir failed\n");
        return;
    }

    riscv64_uart_puts("  fs syscall selftest passed\n");
}

static struct task* riscv64_find_task_by_pid(int pid) {
    struct task* task = task_list;
    while (task) {
        if (task->pid == pid) return task;
        task = task->next;
    }
    return 0;
}

#define RISCV64_USER_MMAP_BASE_VADDR 0x0000002000000000ULL

static uint64_t riscv64_align_up_page(uint64_t value) {
    return (value + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
}

static uint64_t riscv64_test_user_map_page(const char* fail_prefix) {
    struct task* current = get_current_task();
    uint64_t base;
    uint64_t phys;
    uint64_t limit = 0x00007F0000000000ULL;

    if (!current) {
        riscv64_uart_puts(fail_prefix);
        riscv64_uart_puts(": no current task\n");
        return 0;
    }

    base = current->mmap_end;
    if (base < RISCV64_USER_MMAP_BASE_VADDR) {
        base = RISCV64_USER_MMAP_BASE_VADDR;
    }
    base = riscv64_align_up_page(base);
    while (base < limit && arch_vm_get_phys(arch_task_context_get_address_space(&current->ctx), base) != 0) {
        base += PAGE_SIZE;
    }
    if (base >= limit) {
        riscv64_uart_puts(fail_prefix);
        riscv64_uart_puts(": no free user va\n");
        return 0;
    }

    phys = (uint64_t)(uintptr_t)pmm_alloc(1);
    if (!phys) {
        riscv64_uart_puts(fail_prefix);
        riscv64_uart_puts(": page alloc failed\n");
        return 0;
    }
    for (uint64_t i = 0; i < PAGE_SIZE; i++) {
        ((uint8_t*)(uintptr_t)phys)[i] = 0;
    }
    arch_vm_map_page(arch_task_context_get_address_space(&current->ctx),
                     base,
                     phys,
                     arch_vm_user_page_flags(1, 0));
    current->mmap_end = base + PAGE_SIZE;

    phys = arch_vm_get_phys(arch_task_context_get_address_space(&current->ctx), base) & ~(PAGE_SIZE - 1ULL);
    if (!phys) {
        riscv64_uart_puts(fail_prefix);
        riscv64_uart_puts(": map lookup failed\n");
        return 0;
    }
    return base;
}

static int riscv64_test_user_unmap_page(uint64_t base, const char* fail_prefix) {
    struct task* current = get_current_task();
    uint64_t phys;

    if (!current || !base) return -1;

    phys = arch_vm_get_phys(arch_task_context_get_address_space(&current->ctx), base) & ~(PAGE_SIZE - 1ULL);
    if (!phys) {
        riscv64_uart_puts(fail_prefix);
        riscv64_uart_puts(": unmap source missing\n");
        return -1;
    }
    arch_vm_unmap_page(arch_task_context_get_address_space(&current->ctx), base);
    if (arch_vm_get_phys(arch_task_context_get_address_space(&current->ctx), base) != 0) {
        riscv64_uart_puts(fail_prefix);
        riscv64_uart_puts(": unmap kept mapping\n");
        return -1;
    }
    pmm_free((void*)phys, 1);
    return 0;
}

static void riscv64_user_map_selftest(void) {
    struct task* current = get_current_task();
    uint64_t base;
    uint64_t phys;
    volatile uint8_t* page;

    base = riscv64_test_user_map_page("  user map selftest");
    if (!base || !current) return;

    phys = arch_vm_get_phys(arch_task_context_get_address_space(&current->ctx), base);
    page = (volatile uint8_t*)(uintptr_t)(phys & ~(PAGE_SIZE - 1ULL));
    if (page[0] != 0 || page[1] != 0 || page[2] != 0 || page[3] != 0) {
        riscv64_uart_puts("  user map selftest: page not zeroed\n");
        (void)riscv64_test_user_unmap_page(base, "  user map selftest");
        return;
    }

    page[0] = 'M';
    page[1] = '0';
    if (page[0] != 'M' || page[1] != '0') {
        riscv64_uart_puts("  user map selftest: page write failed\n");
        (void)riscv64_test_user_unmap_page(base, "  user map selftest");
        return;
    }

    if (riscv64_test_user_unmap_page(base, "  user map selftest") < 0) return;
    riscv64_uart_puts("  user map selftest passed\n");
}

static void riscv64_fork_selftest(void) {
    struct task* current = get_current_task();
    struct task* child = 0;
    uint64_t parent_phys;
    uint64_t child_phys;
    uint64_t base = 0;
    int child_pid;

    if (!current) {
        riscv64_uart_puts("  fork selftest: no current task\n");
        return;
    }

    base = riscv64_test_user_map_page("  fork selftest");
    if (!base) return;

    parent_phys = arch_vm_get_phys(arch_task_context_get_address_space(&current->ctx), base) &
                  ~(PAGE_SIZE - 1ULL);
    ((volatile uint8_t*)(uintptr_t)parent_phys)[0] = 'F';
    ((volatile uint8_t*)(uintptr_t)parent_phys)[1] = '0';

    child_pid = task_fork(&current->ctx.user_frame);
    if (child_pid <= 0) {
        riscv64_uart_puts("  fork selftest: fork failed\n");
        (void)riscv64_test_user_unmap_page(base, "  fork selftest");
        return;
    }

    child = riscv64_find_task_by_pid(child_pid);
    if (!child) {
        riscv64_uart_puts("  fork selftest: child missing\n");
        (void)riscv64_test_user_unmap_page(base, "  fork selftest");
        return;
    }

    child_phys = arch_vm_get_phys(arch_task_context_get_address_space(&child->ctx), base) &
                 ~(PAGE_SIZE - 1ULL);
    if (!child_phys || child_phys == parent_phys) {
        riscv64_uart_puts("  fork selftest: address space not cloned\n");
        goto cleanup_child;
    }
    if (((volatile uint8_t*)(uintptr_t)child_phys)[0] != 'F' ||
        ((volatile uint8_t*)(uintptr_t)child_phys)[1] != '0') {
        riscv64_uart_puts("  fork selftest: child page contents bad\n");
        goto cleanup_child;
    }

    ((volatile uint8_t*)(uintptr_t)child_phys)[1] = '1';
    if (((volatile uint8_t*)(uintptr_t)parent_phys)[1] != '0') {
        riscv64_uart_puts("  fork selftest: parent page mutated\n");
        goto cleanup_child;
    }
    if (child->ppid != current->pid || child->pid != child_pid || child->kstack_top == 0) {
        riscv64_uart_puts("  fork selftest: child task state bad\n");
        goto cleanup_child;
    }

    if (task_mark_zombie(child, 0) < 0 || task_reap(child) < 0 || riscv64_find_task_by_pid(child_pid)) {
        riscv64_uart_puts("  fork selftest: child cleanup failed\n");
        (void)riscv64_test_user_unmap_page(base, "  fork selftest");
        return;
    }
    if (riscv64_test_user_unmap_page(base, "  fork selftest") < 0) return;
    riscv64_uart_puts("  fork selftest passed\n");
    return;

cleanup_child:
    if (child) {
        (void)task_mark_zombie(child, 0);
        (void)task_reap(child);
    }
    (void)riscv64_test_user_unmap_page(base, "  fork selftest");
}

static void riscv64_first_user_task_bootstrap_continue(void) {
    arch_task_exec_frame_t frame;

    if (!get_current_task()) {
        riscv64_uart_puts("  first user task bootstrap failed: no current task\n");
        riscv64_wait_forever();
    }
    riscv64_syscall_dispatch_selftest();
    riscv64_fs_syscall_selftest();
    if (task_execve(&frame, g_riscv64_bootstrap_path, g_riscv64_bootstrap_argv, g_riscv64_bootstrap_envp) < 0) {
        riscv64_uart_puts("  first user task bootstrap failed: execve\n");
        riscv64_wait_forever();
    }
    riscv64_user_map_selftest();
    riscv64_fork_selftest();
    riscv64_mark_user_handoff_started();
    task_main();
}

static void riscv64_first_user_task_bootstrap(void) {
    struct task* current;

    task_init();
    current = get_current_task();
    if (!current) {
        riscv64_uart_puts("  first user task bootstrap failed: no current task\n");
        return;
    }
    riscv64_run_on_stack(current->kstack_top, riscv64_first_user_task_bootstrap_continue);
}

void riscv64_early_main(uint64_t hart_id, uint64_t dtb_pa) {
    riscv64_boot_capture(hart_id, dtb_pa);
    riscv64_uart_init();

    riscv64_uart_puts("\nOrthox riscv64 early boot\n");
    riscv64_uart_puts("  hart: 0x");
    riscv64_uart_puthex64(hart_id);
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  dtb : 0x");
    riscv64_uart_puthex64(dtb_pa);
    riscv64_uart_puts("\n");

    if (g_riscv64_boot_info.flags & RISCV64_BOOT_FLAG_DTB_VALID) {
        riscv64_uart_puts("  dtb size: 0x");
        riscv64_uart_puthex64(g_riscv64_boot_info.dtb_size);
        riscv64_uart_puts("\n");
        riscv64_uart_puts("  uart   : 0x");
        riscv64_uart_puthex64(g_riscv64_boot_info.uart_base);
        riscv64_uart_puts("\n");
        if (g_riscv64_boot_info.flags & RISCV64_BOOT_FLAG_VIRTIO_MMIO_FOUND) {
            riscv64_uart_puts("  virtio0: 0x");
            riscv64_uart_puthex64(g_riscv64_boot_info.first_virtio_mmio_base);
            riscv64_uart_puts("\n");
        }
        if (g_riscv64_boot_info.memory_size != 0) {
            riscv64_uart_puts("  memory : 0x");
            riscv64_uart_puthex64(g_riscv64_boot_info.memory_base);
            riscv64_uart_puts("..0x");
            riscv64_uart_puthex64(g_riscv64_boot_info.memory_base + g_riscv64_boot_info.memory_size);
            riscv64_uart_puts("\n");
        }
    } else {
        riscv64_uart_puts("  dtb invalid\n");
    }

    pmm_init();
    riscv64_vm_init();
    riscv64_vm_selftest();
    riscv64_vm_clone_selftest();
    riscv64_user_stack_selftest();
    riscv64_user_frame_selftest();
    riscv64_trap_init();
    riscv64_trap_set_kernel_stack(riscv64_boot_stack_top());
    riscv64_timer_init();
    riscv64_first_user_task_bootstrap();
    riscv64_wait_forever();
}
