#ifndef WAIT_H
#define WAIT_H

#include <stdint.h>
#include "spinlock.h"
#include "task.h"

struct wait_queue {
    spinlock_t lock;
    struct task* head;
};

struct completion {
    volatile int done;
    int status;
    struct wait_queue wait;
};

typedef int (*wait_condition_t)(void* arg);

void wait_queue_init(struct wait_queue* q);
int wait_queue_empty(struct wait_queue* q);
int wait_event(struct wait_queue* q, wait_condition_t condition, void* arg);
int wait_event_timeout(struct wait_queue* q, wait_condition_t condition, void* arg,
                       uint64_t timeout_ms);
void wake_up_one(struct wait_queue* q);
void wake_up_all(struct wait_queue* q);

void init_completion(struct completion* c);
void reinit_completion(struct completion* c);
void complete(struct completion* c);
void complete_status(struct completion* c, int status);
void complete_all(struct completion* c);
void complete_all_status(struct completion* c, int status);
void wait_for_completion(struct completion* c);
int wait_for_completion_status(struct completion* c);
int wait_for_completion_timeout_status(struct completion* c, uint64_t timeout_ms,
                                       int* status_out);

#endif
