#include <stddef.h>
#include <stdint.h>
#include "sys_internal.h"
#include "syscall.h"
#include "lapic.h"
#include "limine.h"
#include "sound.h"
#include "task.h"
#include "usb.h"
#include "vmm.h"

extern volatile struct limine_framebuffer_request framebuffer_request;
extern int kb_get_event(struct key_event* ev);

int64_t sys_write_serial(const char* buf, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (buf[i] == '\n') {
            __asm__ volatile("outb %b0, %w1" : : "a"((uint8_t)'\r'), "Nd"((uint16_t)0x3f8));
        }
        __asm__ volatile("outb %b0, %w1" : : "a"((uint8_t)buf[i]), "Nd"((uint16_t)0x3f8));
    }
    return (int64_t)count;
}

int sys_get_video_info(struct video_info* info) {
    if (!framebuffer_request.response || framebuffer_request.response->framebuffer_count == 0) {
        return -1;
    }
    struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];
    info->width = fb->width;
    info->height = fb->height;
    info->pitch = fb->pitch;
    info->bpp = fb->bpp;
    return 0;
}

uint64_t sys_map_framebuffer(void) {
    if (!framebuffer_request.response || framebuffer_request.response->framebuffer_count == 0) {
        return 0;
    }
    struct task* current = get_current_task();
    if (!current) return 0;

    struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];
    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(current->ctx.cr3);
    uint64_t vaddr = 0x1000000000ULL;
    uint64_t paddr = (uint64_t)fb->address;
    if (paddr >= g_hhdm_offset) paddr -= g_hhdm_offset;
    uint64_t size = fb->pitch * fb->height;
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    vmm_map_range(pml4, vaddr, paddr, size, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    return vaddr;
}

uint64_t sys_get_ticks_ms(void) {
    return lapic_get_ticks_ms();
}

int sys_get_key_event(struct key_event* ev) {
    if (!ev) return 0;
    return kb_get_event(ev);
}

int sys_sound_on(uint32_t freq_hz) {
    sound_beep_start(freq_hz);
    return 0;
}

int sys_sound_off(void) {
    sound_beep_stop();
    return 0;
}

int sys_sound_pcm_u8(const uint8_t* samples, uint32_t count, uint32_t sample_rate) {
    return sound_pcm_play_u8(samples, count, sample_rate);
}

int sys_usb_info(void) {
    usb_dump_status();
    return usb_is_ready();
}

int sys_usb_read_block(uint32_t lba, void* user_buf, uint32_t count) {
    uint8_t sector_buf[4096];
    uint8_t* dst = (uint8_t*)user_buf;
    uint32_t bytes;
    if (!dst || count == 0) return -1;
    if (count > 8) return -1;
    if (usb_read_block(lba, sector_buf, count) < 0) return -1;
    bytes = count * 512U;
    for (uint32_t i = 0; i < bytes; i++) {
        dst[i] = sector_buf[i];
    }
    return 0;
}

int sys_mount_usb_root(void) {
    return -1;
}

int sys_get_cpu_id(void) {
    struct cpu_local* cpu = get_cpu_local();
    return cpu ? (int)cpu->cpu_id : -1;
}

int sys_set_fork_spread(int enabled) {
    return task_set_fork_spread(enabled);
}

int sys_get_fork_spread(void) {
    return task_get_fork_spread();
}

int sys_get_runq_stats(struct orth_runq_stat* out, uint32_t max_count) {
    return task_get_runq_stats(out, max_count);
}
