#include <stdint.h>
#include <stddef.h>
#include "sys_internal.h"
#include "task.h"
#include "lapic.h"
#include "arch_time.h"   /* arch_rtc_seconds */
#include "version.h"
#include "linux_errno.h"
#include "linux_syscall.h"   /* arch_uname_machine */


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

/* 0000-01-01 からの日数 (先発グレゴリオ暦、0 年は閏年)。1970-01-01 が 719528。
 *
 * **閏年は「0 年〜(year-1) 年」を数える (2026-09-13 に直した)。**ここは
 * y/4 - y/100 + y/400 と「1 年〜year 年」を数えていた。
 *
 *   - 平年は 0 年の分が抜けて **1 日足りない**。x86 の date が 2026-09-13 に
 *     Sat Sep 12 を返していた (時分秒は合っていた)
 *   - 閏年は当年の分が 0 年の分と相殺されて、たまたま正しい
 *   - 1970-01-01 は 719527 になり、エポックより前として 0 を返していた
 *
 * 当年の 2 月 29 日は days_before_month が足す。
 * 1970-01-01〜2200-12-31 の毎日をホストの datetime と突き合わせて、旧式は
 * 84,371 日中 63,875 日で -1、この式は 0 日 */
static uint64_t days_since_year_zero(int year, int month, int day) {
    uint64_t y = (uint64_t)year;
    uint64_t days = y * 365ULL;
    if (year > 0) {
        uint64_t yp = y - 1ULL;
        days += yp / 4ULL - yp / 100ULL + yp / 400ULL + 1ULL;   /* +1 は 0 年 */
    }
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

/* include/arch_time.h の arch_rtc_seconds。CMOS の RTC を Unix 秒で読む */
uint64_t arch_rtc_seconds(void) {
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

/* **clock_gettime(CLOCK_REALTIME) から作る (2026-09-13)。**別々に時計を
 * 読むと time() と gettimeofday() がずれる。aarch64 / riscv64 には
 * gettimeofday の口が無い (musl が clock_gettime で代用する) */
int sys_gettimeofday(struct linux_timeval* tv) {
    struct linux_timespec ts;
    if (!tv) return -LINUX_EFAULT;
    (void)sys_clock_gettime(0, &ts);
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

int sys_sleep_ms(uint64_t ms) {
    struct task* current = get_current_task();
    if (!current) return -LINUX_ESRCH;
    task_mark_sleeping(current);
    current->sleep_until_ms = lapic_get_ticks_ms() + ms;
    while (current->state == TASK_SLEEPING) {
        kernel_yield();
    }
    return 0;
}
