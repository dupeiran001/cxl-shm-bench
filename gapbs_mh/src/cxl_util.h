#ifndef CXL_UTIL_H
#define CXL_UTIL_H

#include <stdint.h>
#include <x86intrin.h>

extern "C" {
#include "../../global_allocator/common.h"
#include "../../global_allocator/shm_mutex.h"
}

/*
 * CXL Safe Access Helpers
 *
 * For non-coherent CXL memory:
 * - Load: Flush -> Fence -> Load (ensures we see latest remote writes)
 * - Store: Store -> Flush -> Fence (ensures our writes are visible)
 */

template <typename T> static inline T CxlLoad(const T *ptr) {
  // Manual implementation for C++ compatibility (avoid const issues with
  // LOAD_CACHELINE)
  // 1. Flush local cache line (invalidate/writeback) to clear stale data
  // 2. Fence to order the subsequent load
  // 3. Load
  asm volatile("" ::: "memory"); // compiler_barrier
  _mm_clflushopt(const_cast<T *>(ptr));
  _mm_mfence();
  asm volatile("" ::: "memory");
  T result = *ptr;
  asm volatile("" ::: "memory");
  return result;
}

template <typename T> static inline void CxlStore(T *ptr, T val) {
  // 1. Store
  // 2. Flush
  // 3. Fence
  STORE_CACHELINE(ptr, val);
}

/*
 * Hashed LFM Locks
 *
 * Used for fine-grained synchronization where allocating a full mutex per item
 * is too expensive (e.g., Parent Array in BFS).
 *
 * Protocol:
 * 1. Hash address/index to a lock index.
 * 2. Acquire that lock.
 * 3. Perform operation.
 * 4. Release lock.
 */

// Simple hash for lock distribution
static inline uint32_t GetLockIdx(uint64_t idx, uint32_t num_locks) {
  // Simple modulo for now, assuming uniform distribution of accesses
  return idx % num_locks;
}

// Compare-and-Swap on a shared array element, protected by a hashed lock
template <typename T>
static inline bool CxlHashedCAS(T *array, uint64_t idx, T old_val, T new_val,
                                shm_mutex_t *locks, uint32_t num_locks,
                                int my_id, int num_hosts) {
  uint32_t lock_idx = GetLockIdx(idx, num_locks);
  shm_mutex_t *m = &locks[lock_idx];

  shm_mutex_lock(m, my_id, num_hosts);

  // Critical Section
  T cur = CxlLoad(&array[idx]);
  bool success = false;
  if (cur == old_val) {
    CxlStore(&array[idx], new_val);
    success = true;
  }

  shm_mutex_unlock(m, my_id);
  return success;
}

#endif // CXL_UTIL_H
