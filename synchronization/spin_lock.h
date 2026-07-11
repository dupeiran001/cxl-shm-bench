#ifndef BENCH_SPIN_LOCK_H
#define BENCH_SPIN_LOCK_H

#include "../global_allocator/common.h"
#include <pthread.h>
#include <stdint.h>

typedef struct {
  alignas(CACHE_LINE_SIZE) pthread_spinlock_t lock;
} spin_lock_t;

void spin_lock_init(spin_lock_t *lock);
void spin_lock_lock(spin_lock_t *lock);
void spin_lock_unlock(spin_lock_t *lock);

#endif
