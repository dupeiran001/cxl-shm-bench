#ifndef BENCH_PETERSON_LOCK_H
#define BENCH_PETERSON_LOCK_H

#include "../global_allocator/common.h"
#include <stdint.h>

#define PETERSON_MUTEX_MAGIC 0x50455445524e4c4bULL /* "PETERNLK" */

typedef struct {
  cacheline_u64 flag[2];
  cacheline_u64 turn;
} peterson_2p_t;

typedef struct {
  cacheline_u64 magic;
  cacheline_u64 num_hosts;
  peterson_2p_t locks[MAX_HOST_NUM];
} peterson_mutex_t;

void peterson_mutex_init(peterson_mutex_t *m);
uint64_t peterson_mutex_lock(peterson_mutex_t *m, int id, int num_hosts);
void peterson_mutex_unlock(peterson_mutex_t *m, int id);

#define PETERSON_MUTEX_VALID(mptr)                                           \
  (CACHELINE_LOAD(&(mptr)->magic) == PETERSON_MUTEX_MAGIC)

#endif
