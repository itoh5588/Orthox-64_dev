#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "pmm.h"
#include "gdt.h"
#include "vmm.h"
#include "idt.h"
#include "syscall.h"
#include "elf64.h"
#include "task.h"
#include "smp.h"
#include "lapic.h"
#include "sound.h"
#include "spinlock.h"
#include "pci.h"
#include "fs.h"
#include "net.h"
#include "xv6fs.h"
#include "storage.h"
#include "virtio_blk.h"
#include "usb.h"
#include "version.h"
#include "kassert.h"

volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(0);

volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0
};

volatile struct limine_executable_address_request kernel_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0
};

volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

volatile struct limine_mp_request smp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0,
    .flags = 0
};

extern void sys_brk_init(uint64_t initial_break);

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ( "outb %b0, %w1" : : "a"(val), "Nd"(port) );
}

static volatile uint32_t g_serial_lock = 0;

static uint64_t serial_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void serial_irq_restore(uint64_t flags) {
    if (flags & (1ULL << 9)) {
        __asm__ volatile("sti" : : : "memory");
    }
}

/* ---- コンソールの排他 (2026-09-12) --------------------------------------
 *
 * **1 回の puts は元から排他していたが、1 行は複数回の呼び出しで組み立てる。**
 *
 *     puts("[smp] started_cpus="); putdec(n); puts("\r\n");
 *
 * の合間に他の CPU が割り込むと行が混ざる。-smp 2 では BSP が init で
 * この行を出している最中に、idle に落ちた副 CPU が kernel/sched.c から
 * usb_hotplug_poll() を回して別の行を出し、
 *
 *     [usb] hotplug: entered ready=[smp] started_cpus=0 hub_slot=02 ports=
 *
 * になっていた (tests/irq_bottom_half_smp_stress_smoke.sh が
 * `[smp] started_cpus=2` を読めずに落ちる。**揺らぎではなく毎回同じ**)。
 *
 * aarch64 は kernel/aarch64/boot.c に同じ仕掛けを持っている。x86 にも置く。
 *
 * **再入可能でなければならない。**外側で begin した CPU がその中で puts を
 * 呼ぶので、素のロックでは自分を待って止まる。所有 CPU は **CPUID の
 * 初期 APIC ID** で見る —— LAPIC の MMIO を設置する前や、共有層に載る前の
 * 副コアからも呼ばれるため。 */
static volatile int g_console_owner = -1;   /* 初期 APIC ID。-1 = 空き */
static uint32_t g_console_depth;

static inline int console_self(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                             : "a"(1), "c"(0));
    return (int)(ebx >> 24);
}

static void serial_lock(void) {
    while (__atomic_exchange_n(&g_serial_lock, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}

static void serial_unlock(void) {
    __atomic_store_n(&g_serial_lock, 0, __ATOMIC_RELEASE);
}

/* **記帳のあいだだけ割り込みを閉じる。**区間そのものを丸ごと閉じると、
 * usb.c のように長く囲む呼び手で tick を落としかねない (aarch64 と同じ判断) */
void x86_console_begin(void) {
    int me = console_self();
    uint64_t flags = serial_irq_save();
    /* **他の CPU の値と自分の番号は決して一致しない**ので、ロック外で
     * 読んでよい。書くのは所有者だけ */
    if (g_console_owner != me) {
        serial_lock();
        g_console_owner = me;
    }
    g_console_depth++;
    serial_irq_restore(flags);
}

void x86_console_end(void) {
    uint64_t flags = serial_irq_save();
    if (g_console_depth > 0 && --g_console_depth == 0) {
        g_console_owner = -1;
        serial_unlock();
    }
    serial_irq_restore(flags);
}

/* kernel/usb.c の weak な空実装を上書きする (aarch64 は runtime.c:283 で同じこと) */
void usb_arch_console_begin(void) { x86_console_begin(); }
void usb_arch_console_end(void) { x86_console_end(); }

static void init_serial(void) {
    outb(0x3f8 + 1, 0x00); // Disable all interrupts
    outb(0x3f8 + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(0x3f8 + 0, 0x03); // Set divisor to 3 (lo byte) 38400 baud
    outb(0x3f8 + 1, 0x00); //                  (hi byte)
    outb(0x3f8 + 3, 0x03); // 8 bits, no parity, one stop bit (DLAB=0)
    outb(0x3f8 + 1, 0x01); // Enable RX interrupt (DLAB must be 0)
    outb(0x3f8 + 4, 0x0B); // IRQs enabled, RTS/DTR set
}

void puts(const char *s) {
    x86_console_begin();
    for (size_t i = 0; s[i] != '\0'; i++) {
        if (s[i] == '\n') outb(0x3f8, '\r');
        outb(0x3f8, s[i]);
    }
    x86_console_end();
}

void puthex(uint64_t v) {
    const char *hex = "0123456789ABCDEF";
    x86_console_begin();
    for (int i = 60; i >= 0; i -= 4) {
        outb(0x3f8, hex[(v >> i) & 0xF]);
    }
    x86_console_end();
}

static void putdec(uint64_t v) {
    char buf[21];
    int i = 0;
    x86_console_begin();
    if (v == 0) {
        outb(0x3f8, '0');
        x86_console_end();
        return;
    }
    while (v && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i--) outb(0x3f8, buf[i]);
    x86_console_end();
}

static void enable_sse(void) {
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);
    cr0 |= (1ULL << 1);
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);
    cr4 |= (1ULL << 10);
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

static void enable_paging_features(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ULL << 16); // WP (Write Protect)
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

static int path_has_suffix(const char* path, const char* suffix) {
    int len_f = 0;
    int len_s = 0;
    int i;
    if (!path || !suffix) return 0;
    while (path[len_f]) len_f++;
    while (suffix[len_s]) len_s++;
    if (len_f < len_s) return 0;
    for (i = 0; i < len_s; i++) {
        if (path[len_f - len_s + i] != suffix[i]) return 0;
    }
    return 1;
}

static struct limine_file* find_module_by_suffix(const char* suffix) {
    if (!module_request.response) return 0;
    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
        struct limine_file* m = module_request.response->modules[i];
        if (path_has_suffix(m->path, suffix)) return m;
    }
    return 0;
}

static int virtio_blk_storage_read(void* ctx, uint64_t lba, void* buf, size_t count) {
    (void)ctx;
    return virtio_blk_read(lba, buf, (uint32_t)count);
}

static int virtio_blk_storage_write(void* ctx, uint64_t lba, const void* buf, size_t count) {
    (void)ctx;
    return virtio_blk_write(lba, buf, (uint32_t)count);
}

static void register_virtio_blk_image(void) {
    if (virtio_blk_init() == 0) {
        uint64_t capacity = virtio_blk_capacity();
        if (storage_register_device("vblk0", 512, capacity, virtio_blk_storage_read, virtio_blk_storage_write, NULL, 0) == 0) {
            puts("[boot] registered virtio-blk as vblk0\r\n");
            if (xv6fs_mount_storage("vblk0") == 0) {
                puts("[boot] mounted xv6fs root image on vblk0\r\n");
                if (fs_mount_xv6fs_root() == 0) {
                    puts("[boot] switched root source to xv6fs (vblk0)\r\n");
                }
            } else {
                puts("[boot] vblk0 is not xv6fs\r\n");
            }
        }
    }
}

static void register_boot_rootfs_image(void) {
    if (xv6fs_is_mounted()) return; // vblk0 で xv6fs マウント済みならスキップ
    struct limine_file* img = find_module_by_suffix("rootfs.img");
    uint64_t blocks;
    if (!img) return;
    if ((img->size % 512U) != 0) {
        puts("[boot] rootfs.img size not 512-byte aligned\r\n");
        return;
    }
    blocks = img->size / 512U;
    if (storage_register_memory_device("bootimg0", img->address, 512U, blocks, 0) == 0) {
        puts("[boot] registered rootfs.img as storage device bootimg0\r\n");
        if (xv6fs_mount_storage("bootimg0") == 0) {
            puts("[boot] mounted xv6fs root image on bootimg0\r\n");
            if (fs_mount_xv6fs_root() == 0) {
                puts("[boot] switched root source to xv6fs\r\n");
            }
        } else {
            puts("[boot] rootfs.img is not xv6fs\r\n");
        }
    } else {
        puts("[boot] failed to register rootfs.img storage device\r\n");
    }
}

static void* kernel_memset(void* s, int c, size_t n) {
    unsigned char* p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void _start(void) {
    init_serial();
    puts("\r\n--- ");
    puts(ORTHOX_KERNEL_NAME);
    puts(" v");
    puts(ORTHOX_KERNEL_RELEASE);
    puts(" Boot ---\r\n");

#ifdef ORTHOX_KASSERT_SELFTEST
    KASSERT(0 && "ORTHOX_KASSERT_SELFTEST");
#endif

    if (memmap_request.response && hhdm_request.response && kernel_address_request.response) {
        pmm_init();
        gdt_init();
        idt_init();
        enable_sse();
        enable_paging_features();
        uint64_t cr0_val;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0_val));
        puts("CR0 value: 0x"); puthex(cr0_val); puts("\r\n");
        syscall_init();

        vmm_init();
        extern void pic_init(void);
        pic_init();
        lapic_init();
        pci_init();
        sound_init();
        fs_init();
        net_init();
        usb_init();
        register_virtio_blk_image();
        virtio_kout_init();
        register_boot_rootfs_image();
        smp_init(smp_request.response);
        puts("SMP CPUs detected: ");
        putdec(smp_get_cpu_count());
        puts("\r\n");
        smp_debug_dump();

        uint64_t* pml4 = vmm_get_kernel_pml4();
        struct limine_executable_address_response* kaddr = kernel_address_request.response;
        
        vmm_map_range(pml4, kaddr->virtual_base, kaddr->physical_base, 0x2000000, PTE_PRESENT | PTE_WRITABLE);
        vmm_map_range(pml4, hhdm_request.response->offset, 0, 0x100000000ULL, PTE_PRESENT | PTE_WRITABLE);
        
        vmm_activate(pml4);
        task_init();
        smp_start_aps();
        if (smp_wait_for_aps(100000000) == 0) {
            puts("[smp] all APs reported online\r\n");
        } else {
            puts("[smp] AP startup timeout\r\n");
        }
        /* **3 回の呼び出しで 1 行。**囲まないと他の CPU の行と混ざる */
        x86_console_begin();
        puts("[smp] started_cpus=");
        putdec(smp_get_started_cpu_count());
        puts("\r\n");
        x86_console_end();
        smp_send_resched_ipi_selftest();

        if (module_request.response && module_request.response->module_count > 0) {
            struct limine_file* module = NULL;
            module = find_module_by_suffix("sh.elf");

            if (module) {
                // 最初は1つのタスクだけ作成
                struct task* user_task = task_create(0, 0);
                arch_address_space_t user_pml4 = (arch_address_space_t)user_task->ctx.cr3;

                struct elf_info info = elf_load(user_pml4, module->address, 0);
                if (info.entry) {
                    struct elf_info empty_interp;
                    kernel_memset(&empty_interp, 0, sizeof(empty_interp));
                    char* argv[] = { "sh", NULL };
                    char* envp[] = { "PATH=/bin:/bin-musl", "HOME=/", NULL };
                    if (task_prepare_initial_user_stack(user_pml4, user_task, &info, &empty_interp, argv, envp) < 0) {
                        puts("Failed to prepare initial user stack\r\n");
                        while(1);
                    }
                    user_task->heap_break = info.max_vaddr;
                    user_task->user_entry = (uint64_t)info.entry;

                    puts("Starting first user task...\r\n");
                    kernel_yield();
                }
            } else {
                puts("user_test.elf not found!\r\n");
            }
        }
    }

#ifdef X86_VERBOSE_DIAG
    /* **ここから下は暇。**この起動タスクは idle_task にならないまま
     * idle ループへ落ちるので、[pc] に「暇である」と教える
     * (kernel/x86_64/pcstat.c) */
    {
        extern void x86_pc_mark_idle_loop(void);
        x86_pc_mark_idle_loop();
    }
#endif
    task_idle_loop(1);
}
