#include "bakery_lock.h"
#include <string.h>

void bakery_mutex_init(bakery_mutex_t *m) {
  memset(m, 0, sizeof(*m));

  compiler_barrier();
  flush_region(m, sizeof(*m));
  compiler_barrier();

  CACHELINE_STORE(&m->magic, BAKERY_MUTEX_MAGIC);
  store_fence();
}

static inline uint64_t max_ticket(bakery_mutex_t *m, int num_hosts) {
  uint64_t max_val = 0;
  for (int i = 0; i < num_hosts; i++) {
    uint64_t v = CACHELINE_LOAD(&m->number[i]);
    if (v > max_val) {
      max_val = v;
    }
  }
  return max_val;
}

uint64_t bakery_mutex_lock(bakery_mutex_t *m, int id, int num_hosts) {
  if (id < 0 || id >= num_hosts || num_hosts <= 0 || num_hosts > MAX_HOST_NUM) {
    return UINT64_MAX;
  }

  CACHELINE_STORE(&m->entering[id], 1);
  store_fence();

  uint64_t ticket = max_ticket(m, num_hosts) + 1;
  CACHELINE_STORE(&m->number[id], ticket);
  store_fence();

  CACHELINE_STORE(&m->entering[id], 0);
  store_fence();

  for (int j = 0; j < num_hosts; j++) {
    if (j == id) {
      continue;
    }

    while (CACHELINE_LOAD(&m->entering[j])) {
      relax_cpu();
    }

    while (1) {
      uint64_t other_ticket = CACHELINE_LOAD(&m->number[j]);
      if (other_ticket == 0) {
        break;
      }

      if (other_ticket < ticket || (other_ticket == ticket && j < id)) {
        relax_cpu();
      } else {
        break;
      }
    }
  }

  return 0;
}

void bakery_mutex_unlock(bakery_mutex_t *m, int id) {
  CACHELINE_STORE(&m->number[id], 0);
  store_fence();
}
