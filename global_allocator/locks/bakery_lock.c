#include "bakery_lock.h"
#include <string.h>

void shm_mutex_init(shm_mutex_t *m) {
  memset(m, 0, sizeof(*m));

  compiler_barrier();
  flush_region(m, sizeof(*m));
  compiler_barrier();

  CACHELINE_STORE(&m->magic, SHM_MUTEX_MAGIC);
  store_fence();
}

static inline uint64_t max_ticket(shm_mutex_t *m, int num_hosts) {
  uint64_t max_val = 0;
  for (int i = 0; i < num_hosts; i++) {
    uint64_t v = CACHELINE_LOAD(&m->number[i]);
    if (v > max_val) {
      max_val = v;
    }
  }
  return max_val;
}

uint64_t shm_mutex_lock(shm_mutex_t *m, int id, int num_hosts) {
  if (id < 0 || id >= num_hosts || num_hosts <= 0 || num_hosts > MAX_HOST_NUM) {
    return UINT64_MAX;
  }

  // 1. Choosing ticket
  CACHELINE_STORE(&m->entering[id], 1);
  store_fence(); // Ensure entering=1 is visible

  uint64_t ticket = max_ticket(m, num_hosts) + 1;
  CACHELINE_STORE(&m->number[id], ticket);
  store_fence(); // Ensure number is visible

  CACHELINE_STORE(&m->entering[id], 0);
  store_fence(); // Ensure entering=0 is visible

  // 2. Wait for others
  for (int j = 0; j < num_hosts; j++) {
    if (j == id)
      continue;

    // Wait if process j is choosing
    while (CACHELINE_LOAD(&m->entering[j])) {
      relax_cpu();
    }

    // Wait if process j has a valid ticket AND (ticket[j] < ticket[id] OR pair
    // is smaller)
    while (1) {
      uint64_t other_ticket = CACHELINE_LOAD(&m->number[j]);
      if (other_ticket == 0) {
        break; // Not interested
      }

      // Is j higher priority? (smaller ticket, or same ticket & smaller id)
      if (other_ticket < ticket || (other_ticket == ticket && j < id)) {
        relax_cpu(); // Wait
      } else {
        break; // We have priority
      }
    }
  }

  return 0; // Bakery logic doesn't easily track retries, returning 0
}

void shm_mutex_unlock(shm_mutex_t *m, int id) {
  CACHELINE_STORE(&m->number[id], 0);
  store_fence();
}
