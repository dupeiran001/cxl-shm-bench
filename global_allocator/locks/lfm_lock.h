// locks/shm_mutex_lock.h - Lamport's Fast Mutex for CXL shared memory
#ifndef LFM_LOCK_H
#define LFM_LOCK_H

#include "../common.h"
#include <stdalign.h>
#include <stdint.h>

#define SHM_MUTEX_MAGIC 0x4c464d53484d4555ULL /* "LFMSHMU" */

typedef struct {
  cacheline_u64 magic;
  cacheline_u64 x;
  cacheline_u64 y;
  cacheline_u64 b[MAX_HOST_NUM];
  cacheline_u64 ready[MAX_HOST_NUM];
  cacheline_u64 done[MAX_HOST_NUM];
} shm_mutex_t;

void shm_mutex_init(shm_mutex_t *m);
uint64_t shm_mutex_lock(shm_mutex_t *m, int id, int num_hosts);
void shm_mutex_unlock(shm_mutex_t *m, int id);

#define SHM_MUTEX_VALID(mptr)                                                  \
  (CACHELINE_LOAD(&(mptr)->magic) == SHM_MUTEX_MAGIC)

#endif // LFM_LOCK_H
