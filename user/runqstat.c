#include <stdio.h>
#include "../include/syscall.h"

static const char* state_name(int state) {
    switch (state) {
        case 0: return "RUN";
        case 1: return "RDY";
        case 2: return "SLP";
        case 3: return "ZOM";
        case 4: return "DED";
        default: return "UNK";
    }
}

int main(void) {
    struct orth_runq_stat stats[16];
    int count = get_runq_stats(stats, 16);
    unsigned total_load = 0;
    unsigned busiest_load = 0;
    unsigned idlest_load = 0;
    unsigned busiest_cpu = 0;
    unsigned idlest_cpu = 0;
    if (count < 0) {
        printf("runqstat: failed\n");
        return 1;
    }

    printf("runqstat: cpus=%d self=%d\n", count, get_cpu_id());
    for (int i = 0; i < count; i++) {
        unsigned load = stats[i].total_load;
        total_load += load;
        if (i == 0 || load > busiest_load) {
            busiest_load = load;
            busiest_cpu = stats[i].cpu_id;
        }
        if (i == 0 || load < idlest_load) {
            idlest_load = load;
            idlest_cpu = stats[i].cpu_id;
        }
        printf("cpu=%u load=%u runq=%u migratable=%u affined=%u ready=%u blocked_ready=%u running=%u blocked_running=%u sleeping=%u blocked_sleeping=%u head=%d tail=%d current_pid=%d current_state=%s idle=%u\n",
               stats[i].cpu_id,
               stats[i].total_load,
               stats[i].runq_count,
               stats[i].migratable_count,
               stats[i].affined_tasks,
               stats[i].affined_ready,
               stats[i].blocked_ready,
               stats[i].affined_running,
               stats[i].blocked_running,
               stats[i].affined_sleeping,
               stats[i].blocked_sleeping,
               stats[i].runq_head_pid,
               stats[i].runq_tail_pid,
               stats[i].current_pid,
               state_name(stats[i].current_state),
               stats[i].current_is_idle);
    }
    printf("runqstat: total_load=%u imbalance=%u busiest_cpu=%u idlest_cpu=%u\n",
           total_load,
           busiest_load - idlest_load,
           busiest_cpu,
           idlest_cpu);
    return 0;
}
