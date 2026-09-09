#include "bottom_half.h"
#include "spinlock.h"

#define BOTTOM_HALF_QUEUE_CAP 64

struct bottom_half_item {
    bottom_half_fn_t fn;
    void* arg;
};

static spinlock_t g_bottom_half_lock;
static struct bottom_half_item g_bottom_half_queue[BOTTOM_HALF_QUEUE_CAP];
static unsigned int g_bottom_half_head;
static unsigned int g_bottom_half_tail;
static unsigned int g_bottom_half_count;

int bottom_half_enqueue(bottom_half_fn_t fn, void* arg) {
    int ret = 0;
    uint64_t flags;
    if (!fn) return -1;

    flags = spin_lock_irqsave(&g_bottom_half_lock);
    if (g_bottom_half_count >= BOTTOM_HALF_QUEUE_CAP) {
        ret = -1;
    } else {
        g_bottom_half_queue[g_bottom_half_tail].fn = fn;
        g_bottom_half_queue[g_bottom_half_tail].arg = arg;
        g_bottom_half_tail = (g_bottom_half_tail + 1U) % BOTTOM_HALF_QUEUE_CAP;
        g_bottom_half_count++;
    }
    spin_unlock_irqrestore(&g_bottom_half_lock, flags);
    return ret;
}

int bottom_half_run(void) {
    int ran = 0;

    for (;;) {
        struct bottom_half_item item;
        uint64_t flags = spin_lock_irqsave(&g_bottom_half_lock);
        if (g_bottom_half_count == 0) {
            spin_unlock_irqrestore(&g_bottom_half_lock, flags);
            break;
        }
        item = g_bottom_half_queue[g_bottom_half_head];
        g_bottom_half_queue[g_bottom_half_head].fn = 0;
        g_bottom_half_queue[g_bottom_half_head].arg = 0;
        g_bottom_half_head = (g_bottom_half_head + 1U) % BOTTOM_HALF_QUEUE_CAP;
        g_bottom_half_count--;
        spin_unlock_irqrestore(&g_bottom_half_lock, flags);

        item.fn(item.arg);
        ran++;
    }

    return ran;
}
