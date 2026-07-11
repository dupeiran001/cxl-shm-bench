#include "peterson_lock.h"
#include <immintrin.h>
#include <string.h>

/*
 * Tournament tree for N hosts:
 * - Host i starts at leaf position i
 * - At each level, hosts compete pairwise using a 2-process Peterson lock
 * - Winner advances to next level until reaching root
 *
 * Lock index calculation:
 * - Level 0 (leaves): hosts 0,1 compete at lock (N/2 + 0), hosts 2,3 at (N/2 +
 * 1), etc.
 * - We use a heap-like indexing: lock[0] is root, lock[1..2] are children, etc.
 * - Actually simpler: use array index = (num_hosts/2 + host_id/2) at first
 * level, then propagate upward.
 *
 * For simplicity, we use: at level L, host competes against sibling.
 * Lock index at level L for host H: ((num_hosts >> 1) + (H >> 1)) >> L
 * Position within lock (0 or 1): (H >> L) & 1
 */

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

void shm_mutex_init(shm_mutex_t *m) {
  memset(m, 0, sizeof(*m));

  compiler_barrier();
  flush_region(m, sizeof(*m));
  compiler_barrier();

  CACHELINE_STORE(&m->magic, SHM_MUTEX_MAGIC);
  store_fence();
}

// Acquire a single 2-process Peterson lock
// pos: 0 or 1 (which of the two competitors we are)
static inline void peterson_2p_lock(peterson_2p_t *lock, int pos) {
  int other = 1 - pos;

  CACHELINE_STORE(&lock->flag[pos], 1);
  CACHELINE_STORE(&lock->turn, (uint64_t)other);
  _mm_mfence(); /* Required for Peterson on x86 (Store-Load barrier) */

  // Wait while other wants in AND it's other's turn
  while (CACHELINE_LOAD(&lock->flag[other]) != 0 &&
         CACHELINE_LOAD(&lock->turn) == (uint64_t)other) {
    relax_cpu();
  }
}

// Release a single 2-process Peterson lock
static inline void peterson_2p_unlock(peterson_2p_t *lock, int pos) {
  CACHELINE_STORE(&lock->flag[pos], 0);
}

uint64_t shm_mutex_lock(shm_mutex_t *m, int id, int num_hosts) {
  if (id < 0 || id >= num_hosts || num_hosts <= 0 || num_hosts > MAX_HOST_NUM) {
    return UINT64_MAX;
  }

  // Ensure num_hosts is recorded for unlock (which strictly needs it)
  // We use the first observed num_hosts value.
  if (CACHELINE_LOAD(&m->num_hosts) == 0) {
    CACHELINE_STORE(&m->num_hosts, (uint64_t)num_hosts);
  }

  // Special case: only 1 host, no lock needed
  if (num_hosts == 1) {
    return 0;
  }

  // Pad to power of 2 for balanced tree
  uint32_t n = next_pow2((uint32_t)num_hosts);
  uint32_t levels = log2_ceil(n);

  // Track our path through the tree for unlock
  // At each level, we acquire the lock at a specific index with a specific
  // position Lock index at level L: (n >> (L+1)) + (id >> (L+1)) - 1 (adjusted
  // for 0-indexing)

  // Simpler scheme: linear array of locks
  // Lock[i] at level L for pair starting at host 2^(L+1) * k
  // Position in lock: (id >> L) & 1

  uint32_t node = (uint32_t)id;
  for (uint32_t level = 0; level < levels; level++) {
    // Which lock at this level?
    uint32_t lock_base =
        (n >> (level + 1)) - 1;       // First lock index at this level
    uint32_t lock_offset = node >> 1; // Which pair
    uint32_t lock_idx = lock_base + lock_offset;

    // Our position in the 2-process lock
    int pos = node & 1;

    peterson_2p_lock(&m->locks[lock_idx], pos);

    // Move to parent node for next level
    node >>= 1;
  }

  return 0; // Peterson doesn't track contention attempts
}

void shm_mutex_unlock(shm_mutex_t *m, int id) {
  // We need to unlock in reverse order (from root back to leaves)
  // To do this, we need to know num_hosts - read from struct
  uint32_t num_hosts = (uint32_t)CACHELINE_LOAD(&m->num_hosts);

  if (num_hosts == 0) {
    // Fallback: assume MAX_HOST_NUM if not set
    num_hosts = MAX_HOST_NUM;
  }
  if (num_hosts == 1) {
    return; // No lock to release
  }

  uint32_t n = next_pow2(num_hosts);
  uint32_t levels = log2_ceil(n);

  // Unlock from root (highest level) down to leaves
  // Calculate our path first
  uint32_t node = (uint32_t)id;

  // Store the path: lock indices and positions
  uint32_t lock_indices[32];
  int positions[32];

  for (uint32_t level = 0; level < levels; level++) {
    uint32_t lock_base = (n >> (level + 1)) - 1;
    uint32_t lock_offset = node >> 1;
    lock_indices[level] = lock_base + lock_offset;
    positions[level] = node & 1;
    node >>= 1;
  }

  // Release in reverse order
  for (int level = (int)levels - 1; level >= 0; level--) {
    peterson_2p_unlock(&m->locks[lock_indices[level]], positions[level]);
  }
}
