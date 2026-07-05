#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include "limine.h"

struct smp_cpu_info {
    uint32_t cpu_index;
    uint32_t processor_id;
    uint32_t lapic_id;
    uint8_t is_bsp;
    uint8_t started;
};

void smp_init(struct limine_mp_response* response);
uint32_t smp_get_cpu_count(void);
int smp_get_bsp_cpu_index(void);
const struct smp_cpu_info* smp_get_cpu_info(uint32_t cpu_index);
void smp_debug_dump(void);
void smp_start_aps(void);
uint32_t smp_get_started_cpu_count(void);
int smp_wait_for_aps(uint32_t spin_limit);
void smp_send_resched_ipi(uint32_t cpu_id);
void smp_send_resched_ipi_selftest(void);

#endif
