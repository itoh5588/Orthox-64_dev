#ifndef BOTTOM_HALF_H
#define BOTTOM_HALF_H

typedef void (*bottom_half_fn_t)(void* arg);

int bottom_half_enqueue(bottom_half_fn_t fn, void* arg);
int bottom_half_run(void);

#endif
