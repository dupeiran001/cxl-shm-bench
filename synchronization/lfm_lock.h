#ifndef BENCH_LFM_LOCK_H
#define BENCH_LFM_LOCK_H

#include "../global_allocator/common.h"
#include <stdint.h>

#define LFM_MUTEX_MAGIC 0x4c464d53484d4555ULL /* "LFMSHMU" */

typedef struct {
  cacheline_u64 magic;
  cacheline_u64 x;
  cacheline_u64 y;
  cacheline_u64 b[MAX_HOST_NUM];
  cacheline_u64 ready[MAX_HOST_NUM];
  cacheline_u64 done[MAX_HOST_NUM];
} lfm_mutex_t;

void lfm_mutex_init(lfm_mutex_t *m);
uint64_t lfm_mutex_lock(lfm_mutex_t *m, int id, int num_hosts);
void lfm_mutex_unlock(lfm_mutex_t *m, int id);

#define LFM_MUTEX_VALID(mptr) (CACHELINE_LOAD(&(mptr)->magic) == LFM_MUTEX_MAGIC)

#endif
