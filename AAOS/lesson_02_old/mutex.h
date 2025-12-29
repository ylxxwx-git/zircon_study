#ifndef MUTEX_H
#define MUTEX_H

typedef struct {
    volatile int lock;
} mutex_t;

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);

#endif
