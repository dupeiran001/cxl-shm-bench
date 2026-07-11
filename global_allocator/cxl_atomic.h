/*
 * cxl_atomic.h - Software atomic primitives for non-coherent CXL shared memory
 *
 * These primitives work across machines sharing CXL memory with no hardware
 * cache coherency. All synchronization is done via clflushopt + fences.
 *
 * Implementation:
 * Protects the atomic value with an embedded shm_mutex_t (shm_mutex.h), whose
 * lock backend is selected at compile time via LOCK_IMPL (default Lamport's
 * Fast Mutex; also Peterson, Bakery). cxl_atomic therefore uses whichever
 * lock the library was built with, not LFM unconditionally. This guarantees
 * correctness across processes/machines.
 */

#ifndef CXL_ATOMIC_H
#define CXL_ATOMIC_H

#include "common.h"
#include "shm_mutex.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * cxl_atomic64_t - An atomic 64-bit value for cross-machine operations
 *
 * Uses an embedded shm_mutex_t to protect the value.
 */
typedef struct {
  cacheline_u64 value;
  shm_mutex_t lock; /* Protects 'value' */
} cxl_atomic64_t;

/* Initialize an atomic variable */
static inline void cxl_atomic64_init(cxl_atomic64_t *a, uint64_t val) {
  CACHELINE_STORE(&a->value, val);
  shm_mutex_init(&a->lock);
}

/* Load the current value (with flush for visibility) - atomic read */
static inline uint64_t cxl_atomic64_load(cxl_atomic64_t *a) {
  /*
   * We use CACHELINE_LOAD to flush any local stale copies and ensure we see
   * the latest value written to memory.
   */
  return CACHELINE_LOAD(&a->value);
}

/* Store a value (atomic write) - UNSAFE if concurrent with other RMW ops
 * without lock */
static inline void cxl_atomic64_store(cxl_atomic64_t *a, uint64_t val) {
  CACHELINE_STORE(&a->value, val);
}

/*
 * cxl_atomic64_swap - Atomically swap value, return old value
 */
static inline uint64_t cxl_atomic64_swap(cxl_atomic64_t *a, uint64_t new_val,
                                         uint32_t my_id, uint32_t num_hosts) {
  SYNC_STAT_INC(atomic_rmw);
  shm_mutex_lock(&a->lock, (int)my_id, (int)num_hosts);

  uint64_t old_val = CACHELINE_LOAD(&a->value);
  CACHELINE_STORE(&a->value, new_val);

  shm_mutex_unlock(&a->lock, (int)my_id);
  return old_val;
}

/*
 * cxl_atomic64_cmpxchg - Compare-and-swap
 */
static inline bool cxl_atomic64_cmpxchg(cxl_atomic64_t *a, uint64_t *expected,
                                        uint64_t new_val, uint32_t my_id,
                                        uint32_t num_hosts) {
  SYNC_STAT_INC(atomic_rmw);
  shm_mutex_lock(&a->lock, (int)my_id, (int)num_hosts);

  uint64_t cur_val = CACHELINE_LOAD(&a->value);
  if (cur_val == *expected) {
    CACHELINE_STORE(&a->value, new_val);
    shm_mutex_unlock(&a->lock, (int)my_id);
    return true;
  } else {
    *expected = cur_val;
    shm_mutex_unlock(&a->lock, (int)my_id);
    return false;
  }
}

/*
 * cxl_atomic64_fetch_add - Atomically add and return old value
 */
static inline uint64_t cxl_atomic64_fetch_add(cxl_atomic64_t *a, uint64_t delta,
                                              uint32_t my_id,
                                              uint32_t num_hosts) {
  SYNC_STAT_INC(atomic_rmw);
  shm_mutex_lock(&a->lock, (int)my_id, (int)num_hosts);

  uint64_t old_val = CACHELINE_LOAD(&a->value);
  CACHELINE_STORE(&a->value, old_val + delta);

  shm_mutex_unlock(&a->lock, (int)my_id);
  return old_val;
}

/*
 * cxl_atomic64_add - Atomically add (don't need old value)
 */
static inline void cxl_atomic64_add(cxl_atomic64_t *a, uint64_t delta,
                                    uint32_t my_id, uint32_t num_hosts) {
  SYNC_STAT_INC(atomic_rmw);
  shm_mutex_lock(&a->lock, (int)my_id, (int)num_hosts);

  uint64_t old_val = CACHELINE_LOAD(&a->value);
  CACHELINE_STORE(&a->value, old_val + delta);

  shm_mutex_unlock(&a->lock, (int)my_id);
}

#endif /* CXL_ATOMIC_H */
