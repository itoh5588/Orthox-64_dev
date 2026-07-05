#include <stdint.h>
#include <stddef.h>
#include "sys_internal.h"
#include "task.h"
#include "lapic.h"
#include "limine.h"
#include "version.h"

extern volatile struct limine_memmap_request memmap_request;

static inline void outb_u8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb_u8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static int is_leap_year(int year) {
    if ((year % 4) != 0) return 0;
    if ((year % 100) != 0) return 1;
    return (year % 400) == 0;
}

static uint32_t days_before_month(int year, int month) {
    static const uint16_t base[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    uint32_t days = 0;
    if (month < 1) month = 1;
    if (month > 12) month = 12;
    days = base[month - 1];
    if (month > 2 && is_leap_year(year)) days++;
    return days;
}

static uint64_t days_since_year_zero(int year, int month, int day) {
    uint64_t y = (uint64_t)year;
    uint64_t days = y * 365ULL + y / 4ULL - y / 100ULL + y / 400ULL;
    days += days_before_month(year, month);
    if (day > 0) days += (uint64_t)(day - 1);
    return days;
}

static uint8_t cmos_read(uint8_t reg) {
    outb_u8(0x70, reg);
    return inb_u8(0x71);
}

static int cmos_bcd_to_bin(int value) {
    return ((value >> 4) * 10) + (value & 0x0F);
}

static uint64_t sys_realtime_seconds(void) {
    int second;
    int minute;
    int hour;
    int day;
    int month;
    int year;
    int regb;
    uint64_t days;

    while (cmos_read(0x0A) & 0x80) {
    }

    second = cmos_read(0x00);
    minute = cmos_read(0x02);
    hour = cmos_read(0x04);
    day = cmos_read(0x07);
    month = cmos_read(0x08);
    year = cmos_read(0x09);
    regb = cmos_read(0x0B);

    if ((regb & 0x04) == 0) {
        second = cmos_bcd_to_bin(second);
        minute = cmos_bcd_to_bin(minute);
        hour = cmos_bcd_to_bin(hour & 0x7F) | (hour & 0x80);
        day = cmos_bcd_to_bin(day);
        month = cmos_bcd_to_bin(month);
        year = cmos_bcd_to_bin(year);
    }

    if ((regb & 0x02) == 0 && (hour & 0x80)) {
        hour = ((hour & 0x7F) + 12) % 24;
    }

    year += (year >= 70) ? 1900 : 2000;
    days = days_since_year_zero(year, month, day);
    if (days < 719528ULL) return 0;
    return (days - 719528ULL) * 86400ULL
        + (uint64_t)hour * 3600ULL
        + (uint64_t)minute * 60ULL
        + (uint64_t)second;
}

int sys_gettimeofday(struct orth_timeval_k* tv) {
    if (!tv) return -1;
    tv->tv_sec = (int64_t)sys_realtime_seconds();
    tv->tv_usec = 0;
    return 0;
}

int sys_clock_gettime(int clock_id, struct orth_timespec_k* ts) {
    uint64_t ms;
    if (!ts) return -1;
    if (clock_id == 0) {
        ts->tv_sec = (int64_t)sys_realtime_seconds();
        ts->tv_nsec = 0;
        return 0;
    }
    if (clock_id != 1) return -1;
    ms = lapic_get_ticks_ms();
    ts->tv_sec = (int64_t)(ms / 1000ULL);
    ts->tv_nsec = (int64_t)((ms % 1000ULL) * 1000000ULL);
    return 0;
}

static void copy_cstr_fixed(char* dst, size_t dst_size, const char* src) {
    size_t i = 0;
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (i + 1 < dst_size && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

int sys_sched_yield(void) {
    kernel_yield();
    return 0;
}

int sys_nanosleep(const struct orth_timespec_k* req, struct orth_timespec_k* rem) {
    uint64_t ms;
    if (!req) return -1;
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000LL) return -1;
    ms = (uint64_t)req->tv_sec * 1000ULL;
    if (req->tv_nsec != 0) {
        ms += (uint64_t)((req->tv_nsec + 999999LL) / 1000000LL);
    }
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return sys_sleep_ms(ms);
}

int sys_uname(struct linux_utsname_k* buf) {
    if (!buf) return -1;
    copy_cstr_fixed(buf->sysname, sizeof(buf->sysname), ORTHOX_UNAME_SYSNAME);
    copy_cstr_fixed(buf->nodename, sizeof(buf->nodename), ORTHOX_UNAME_NODENAME);
    copy_cstr_fixed(buf->release, sizeof(buf->release), ORTHOX_KERNEL_RELEASE);
    copy_cstr_fixed(buf->version, sizeof(buf->version), ORTHOX_KERNEL_VERSION);
    copy_cstr_fixed(buf->machine, sizeof(buf->machine), ORTHOX_UNAME_MACHINE);
    copy_cstr_fixed(buf->domainname, sizeof(buf->domainname), ORTHOX_UNAME_DOMAINNAME);
    return 0;
}

int sys_getrlimit(unsigned resource, struct linux_rlimit_k* rlim) {
    (void)resource;
    if (!rlim) return -1;
    rlim->rlim_cur = ~0ULL;
    rlim->rlim_max = ~0ULL;
    return 0;
}

int sys_prlimit64(int pid, unsigned resource, const struct linux_rlimit_k* new_limit,
                  struct linux_rlimit_k* old_limit) {
    struct task* current = get_current_task();
    if (pid != 0 && (!current || pid != current->pid)) return -1;
    (void)new_limit;
    return sys_getrlimit(resource, old_limit);
}

int sys_sysinfo(struct linux_sysinfo_k* info) {
    struct limine_memmap_response* memmap;
    uint64_t totalram = 0;

    if (!info) return -1;

    memmap = memmap_request.response;
    if (memmap) {
        for (uint64_t i = 0; i < memmap->entry_count; i++) {
            struct limine_memmap_entry* entry = memmap->entries[i];
            if (entry->type == LIMINE_MEMMAP_USABLE) {
                totalram += entry->length;
            }
        }
    }

    *info = (struct linux_sysinfo_k){0};
    info->uptime = lapic_get_ticks_ms() / 1000ULL;
    info->totalram = (unsigned long)totalram;
    info->freeram = (unsigned long)totalram;
    info->procs = 1;
    info->mem_unit = 1;
    return 0;
}

int sys_sleep_ms(uint64_t ms) {
    struct task* current = get_current_task();
    if (!current) return -1;
    task_mark_sleeping(current);
    current->sleep_until_ms = lapic_get_ticks_ms() + ms;
    while (current->state == TASK_SLEEPING) {
        kernel_yield();
    }
    return 0;
}
