#include "smp.h"
#include "task.h"
#include "pmm.h"
#include "vmm.h"
#include "gdt.h"
#include "idt.h"
#include "lapic.h"
#include "syscall.h"

static struct smp_cpu_info g_smp_cpus[ORTHOX_MAX_CPUS];
static uint32_t g_smp_cpu_count = 1;
static int g_bsp_cpu_index = 0;
static struct limine_mp_response* g_smp_response;
static uint64_t g_ap_stack_tops[ORTHOX_MAX_CPUS];
static volatile uint32_t g_smp_started_cpu_count = 1;

extern void puts(const char* s);
 
static void append_str(char* buf, int* pos, int max, const char* s) {
    while (s && *s && *pos + 1 < max) {
        buf[(*pos)++] = *s++;
    }
}

static void append_dec(char* buf, int* pos, int max, uint64_t v) {
    char tmp[21];
    int n = 0;
    if (v == 0) {
        if (*pos + 1 < max) buf[(*pos)++] = '0';
        return;
    }
    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n-- > 0 && *pos + 1 < max) {
        buf[(*pos)++] = tmp[n];
    }
}

static void append_hex(char* buf, int* pos, int max, uint64_t v) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 60; i >= 0 && *pos + 1 < max; i -= 4) {
        buf[(*pos)++] = hex[(v >> i) & 0xF];
    }
}

static void smp_log_cpu_line(const char* prefix, uint32_t cpu_index,
                             uint64_t lapic_id, uint64_t processor_id,
                             const char* role, const char* suffix) {
    char buf[192];
    int pos = 0;
    append_str(buf, &pos, sizeof(buf), prefix);
    append_dec(buf, &pos, sizeof(buf), cpu_index);
    append_str(buf, &pos, sizeof(buf), " lapic_id=0x");
    append_hex(buf, &pos, sizeof(buf), lapic_id);
    if (processor_id != UINT64_MAX) {
        append_str(buf, &pos, sizeof(buf), " processor_id=0x");
        append_hex(buf, &pos, sizeof(buf), processor_id);
    }
    if (role) {
        append_str(buf, &pos, sizeof(buf), " role=");
        append_str(buf, &pos, sizeof(buf), role);
    }
    if (suffix) append_str(buf, &pos, sizeof(buf), suffix);
    append_str(buf, &pos, sizeof(buf), "\r\n");
    buf[pos] = '\0';
    puts(buf);
}

void smp_init(struct limine_mp_response* response) {
    g_smp_response = response;
    g_smp_cpu_count = 1;
    g_bsp_cpu_index = 0;
    g_smp_started_cpu_count = 1;

    if (!response || response->cpu_count == 0 || !response->cpus) {
        task_set_cpu_count(1);
        g_smp_cpus[0].cpu_index = 0;
        g_smp_cpus[0].processor_id = 0;
        g_smp_cpus[0].lapic_id = 0;
        g_smp_cpus[0].is_bsp = 1;
        g_smp_cpus[0].started = 1;
        return;
    }

    uint32_t cpu_count = (uint32_t)response->cpu_count;
    if (cpu_count > ORTHOX_MAX_CPUS) cpu_count = ORTHOX_MAX_CPUS;

    g_smp_cpu_count = cpu_count;
    task_set_cpu_count(cpu_count);

    for (uint32_t i = 0; i < cpu_count; i++) {
        struct limine_mp_info* cpu = response->cpus[i];
        g_smp_cpus[i].cpu_index = i;
        g_smp_cpus[i].processor_id = cpu ? cpu->processor_id : 0;
        g_smp_cpus[i].lapic_id = cpu ? cpu->lapic_id : 0;
        g_smp_cpus[i].is_bsp = (cpu && cpu->lapic_id == response->bsp_lapic_id) ? 1 : 0;
        g_smp_cpus[i].started = g_smp_cpus[i].is_bsp ? 1 : 0;
        if (g_smp_cpus[i].is_bsp) g_bsp_cpu_index = (int)i;
    }
}

uint32_t smp_get_cpu_count(void) {
    return g_smp_cpu_count;
}

int smp_get_bsp_cpu_index(void) {
    return g_bsp_cpu_index;
}

const struct smp_cpu_info* smp_get_cpu_info(uint32_t cpu_index) {
    if (cpu_index >= g_smp_cpu_count) return 0;
    return &g_smp_cpus[cpu_index];
}

static void smp_enable_sse(void) {
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

static void smp_enable_paging_features(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ULL << 16);
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

static void smp_ap_entry(struct limine_mp_info* info) {
    uint32_t cpu_index = info ? (uint32_t)info->extra_argument : 0;
    uint32_t lapic_id = info ? info->lapic_id : 0;
    uint64_t stack_top = 0;
    struct task* idle = NULL;

    if (cpu_index < g_smp_cpu_count) {
        stack_top = g_ap_stack_tops[cpu_index];
        idle = task_create_idle(cpu_index);
        if (idle) {
            stack_top = idle->kstack_top;
            task_bind_cpu_local(cpu_index, idle, idle, stack_top);
            task_install_cpu_local(cpu_index);
            smp_enable_sse();
            smp_enable_paging_features();
            gdt_init_cpu(cpu_index);
            idt_init_cpu(cpu_index);
            syscall_init_cpu();
            tss_set_stack_for_cpu(cpu_index, stack_top);
            lapic_init_cpu();
        }
        g_smp_cpus[cpu_index].started = 1;
    }
    smp_log_cpu_line("[smp] AP online cpu", cpu_index, lapic_id, UINT64_MAX,
                     NULL, " entering idle-hlt");
    __atomic_add_fetch(&g_smp_started_cpu_count, 1, __ATOMIC_SEQ_CST);
    task_idle_loop(0);
}

void smp_debug_dump(void) {
    char header[96];
    int pos = 0;
    append_str(header, &pos, sizeof(header), "[smp] cpu_count=");
    append_dec(header, &pos, sizeof(header), g_smp_cpu_count);
    append_str(header, &pos, sizeof(header), " bsp_index=");
    append_dec(header, &pos, sizeof(header), (uint64_t)(g_bsp_cpu_index < 0 ? 0 : g_bsp_cpu_index));
    append_str(header, &pos, sizeof(header), "\r\n");
    header[pos] = '\0';
    puts(header);

    for (uint32_t i = 0; i < g_smp_cpu_count; i++) {
        smp_log_cpu_line("[smp] cpu", i, g_smp_cpus[i].lapic_id,
                         g_smp_cpus[i].processor_id,
                         g_smp_cpus[i].is_bsp ? "BSP" : "AP",
                         g_smp_cpus[i].started ? " started=yes" : " started=no");
    }
}

void smp_start_aps(void) {
    if (!g_smp_response || g_smp_cpu_count <= 1 || !g_smp_response->cpus) {
        return;
    }

    for (uint32_t i = 0; i < g_smp_cpu_count; i++) {
        struct limine_mp_info* cpu = g_smp_response->cpus[i];
        if (!cpu || g_smp_cpus[i].is_bsp) continue;
        if (!g_ap_stack_tops[i]) {
            void* stack_phys = pmm_alloc(4);
            if (!stack_phys) {
                char msg[96];
                int pos = 0;
                append_str(msg, &pos, sizeof(msg), "[smp] AP stack alloc failed for cpu");
                append_dec(msg, &pos, sizeof(msg), i);
                append_str(msg, &pos, sizeof(msg), "\r\n");
                msg[pos] = '\0';
                puts(msg);
                continue;
            }
            g_ap_stack_tops[i] = (uint64_t)PHYS_TO_VIRT(stack_phys) + 4 * PAGE_SIZE;
        }
        cpu->extra_argument = i;
        cpu->goto_address = smp_ap_entry;
        smp_log_cpu_line("[smp] AP launch cpu", i, g_smp_cpus[i].lapic_id,
                         UINT64_MAX, NULL, NULL);
    }
}

uint32_t smp_get_started_cpu_count(void) {
    return g_smp_started_cpu_count;
}

int smp_wait_for_aps(uint32_t spin_limit) {
    uint32_t target = g_smp_cpu_count;
    if (target <= 1) return 0;

    for (uint32_t i = 0; i < spin_limit; i++) {
        if (__atomic_load_n(&g_smp_started_cpu_count, __ATOMIC_SEQ_CST) >= target) {
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

void smp_send_resched_ipi(uint32_t cpu_id) {
    const struct smp_cpu_info* cpu = smp_get_cpu_info(cpu_id);
    if (!cpu || !cpu->started) return;
    lapic_send_ipi(cpu->lapic_id, INT_VECTOR_RESCHED);
}

void smp_send_resched_ipi_selftest(void) {
    if (g_smp_cpu_count <= 1) return;
    puts("[smp] sending resched IPI self-test\r\n");
    for (uint32_t i = 0; i < g_smp_cpu_count; i++) {
        if (!g_smp_cpus[i].started || g_smp_cpus[i].is_bsp) continue;
        smp_send_resched_ipi(i);
    }
}
