#include "peterson_lock.h"
#include <immintrin.h>
#include <string.h>

static inline uint32_t next_pow2(uint32_t v) {
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  return v + 1;
}

static inline uint32_t log2_ceil(uint32_t v) {
  uint32_t r = 0;
  v--;
  while (v > 0) {
    v >>= 1;
    r++;
  }
  return r;
}

void peterson_mutex_init(peterson_mutex_t *m) {
  memset(m, 0, sizeof(*m));

  compiler_barrier();
  flush_region(m, sizeof(*m));
  compiler_barrier();

  CACHELINE_STORE(&m->magic, PETERSON_MUTEX_MAGIC);
  store_fence();
}

static inline void peterson_2p_lock(peterson_2p_t *lock, int pos) {
  int other = 1 - pos;

  CACHELINE_STORE(&lock->flag[pos], 1);
  CACHELINE_STORE(&lock->turn, (uint64_t)other);
  _mm_mfence();

  while (CACHELINE_LOAD(&lock->flag[other]) != 0 &&
         CACHELINE_LOAD(&lock->turn) == (uint64_t)other) {
    relax_cpu();
  }
}

static inline void peterson_2p_unlock(peterson_2p_t *lock, int pos) {
  CACHELINE_STORE(&lock->flag[pos], 0);
}

uint64_t peterson_mutex_lock(peterson_mutex_t *m, int id, int num_hosts) {
  if (id < 0 || id >= num_hosts || num_hosts <= 0 || num_hosts > MAX_HOST_NUM) {
    return UINT64_MAX;
  }

  if (CACHELINE_LOAD(&m->num_hosts) == 0) {
    CACHELINE_STORE(&m->num_hosts, (uint64_t)num_hosts);
  }

  if (num_hosts == 1) {
    return 0;
  }

  uint32_t n = next_pow2((uint32_t)num_hosts);
  uint32_t levels = log2_ceil(n);

  uint32_t node = (uint32_t)id;
  for (uint32_t level = 0; level < levels; level++) {
    uint32_t lock_base = (n >> (level + 1)) - 1;
    uint32_t lock_offset = node >> 1;
    uint32_t lock_idx = lock_base + lock_offset;
    int pos = node & 1;

    peterson_2p_lock(&m->locks[lock_idx], pos);
    node >>= 1;
  }

  return 0;
}

void peterson_mutex_unlock(peterson_mutex_t *m, int id) {
  uint32_t num_hosts = (uint32_t)CACHELINE_LOAD(&m->num_hosts);
  if (num_hosts == 0) {
    num_hosts = MAX_HOST_NUM;
  }
  if (num_hosts == 1) {
    return;
  }

  uint32_t n = next_pow2(num_hosts);
  uint32_t levels = log2_ceil(n);

  uint32_t node = (uint32_t)id;
  uint32_t lock_indices[32];
  int positions[32];

  for (uint32_t level = 0; level < levels; level++) {
    uint32_t lock_base = (n >> (level + 1)) - 1;
    uint32_t lock_offset = node >> 1;
    lock_indices[level] = lock_base + lock_offset;
    positions[level] = node & 1;
    node >>= 1;
  }

  for (int level = (int)levels - 1; level >= 0; level--) {
    peterson_2p_unlock(&m->locks[lock_indices[level]], positions[level]);
  }
}
