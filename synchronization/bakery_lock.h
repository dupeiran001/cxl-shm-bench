#ifndef BENCH_BAKERY_LOCK_H
#define BENCH_BAKERY_LOCK_H

#include "../global_allocator/common.h"
#include <stdint.h>

#define BAKERY_MUTEX_MAGIC 0xBAAAAAADBAAAAAADULL

typedef struct {
  cacheline_u64 magic;
  cacheline_u64 number[MAX_HOST_NUM];
  cacheline_u64 entering[MAX_HOST_NUM];
} bakery_mutex_t;

void bakery_mutex_init(bakery_mutex_t *m);
uint64_t bakery_mutex_lock(bakery_mutex_t *m, int id, int num_hosts);
void bakery_mutex_unlock(bakery_mutex_t *m, int id);

#define BAKERY_MUTEX_VALID(m) (CACHELINE_LOAD(&(m)->magic) == BAKERY_MUTEX_MAGIC)

#endif
