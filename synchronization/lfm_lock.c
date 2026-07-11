#include "lfm_lock.h"
#include <string.h>

void lfm_mutex_init(lfm_mutex_t *m) {
  memset(m, 0, sizeof(*m));

  compiler_barrier();
  flush_region(m, sizeof(*m));
  compiler_barrier();

  CACHELINE_STORE(&m->magic, LFM_MUTEX_MAGIC);
  store_fence();
}

uint64_t lfm_mutex_lock(lfm_mutex_t *m, int id, int num_hosts) {
  if (id < 0 || id >= num_hosts || num_hosts <= 0 || num_hosts > MAX_HOST_NUM) {
    return UINT64_MAX;
  }

  uint64_t attempts = 0;

retry:
  ++attempts;

  CACHELINE_STORE(&m->b[id], 1);
  CACHELINE_STORE(&m->x, (uint64_t)(id + 1));

  if (CACHELINE_LOAD(&m->y) != 0) {
    CACHELINE_STORE(&m->b[id], 0);
    while (CACHELINE_LOAD(&m->y) != 0) {
      relax_cpu();
    }
    goto retry;
  }

  CACHELINE_STORE(&m->y, (uint64_t)(id + 1));

  if (CACHELINE_LOAD(&m->x) != (uint64_t)(id + 1)) {
    CACHELINE_STORE(&m->b[id], 0);
    for (int j = 0; j < num_hosts; ++j) {
      if (j == id) {
        continue;
      }
      while (CACHELINE_LOAD(&m->b[j]) != 0) {
        relax_cpu();
      }
    }
    if (CACHELINE_LOAD(&m->y) != (uint64_t)(id + 1)) {
      while (CACHELINE_LOAD(&m->y) != 0) {
        relax_cpu();
      }
      goto retry;
    }
  }

  return attempts > 0 ? (attempts - 1) : 0;
}

void lfm_mutex_unlock(lfm_mutex_t *m, int id) {
  CACHELINE_STORE(&m->y, 0);
  CACHELINE_STORE(&m->b[id], 0);
}
