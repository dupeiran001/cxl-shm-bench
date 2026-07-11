// shm_mutex.h - Compile-time lock implementation selection
// Include this header for the pluggable mutex API.
//
// To switch implementations, set LOCK_IMPL in the Makefile or -D flag:
//   make LOCK_IMPL=lfm       # Lamport's Fast Mutex (default)
//   make LOCK_IMPL=peterson  # Peterson's tournament tree
//
#ifndef SHM_MUTEX_H
#define SHM_MUTEX_H

#if defined(LOCK_IMPL_PETERSON)
#include "locks/peterson_lock.h"
#elif defined(LOCK_IMPL_BAKERY)
#include "locks/bakery_lock.h"
#else
// Default: Lamport's Fast Mutex (LFM)
#include "locks/lfm_lock.h"
#endif

/*
 * Unified API (provided by all implementations):
 *
 *   typedef struct { ... } shm_mutex_t;
 *
 *   void shm_mutex_init(shm_mutex_t *m);
 *     Initialize mutex. Must be called before use. Safe for multiple callers.
 *
 *   uint64_t shm_mutex_lock(shm_mutex_t *m, int id, int num_hosts);
 *     Acquire lock. id = caller's host ID [0, num_hosts).
 *     Returns contention count or 0 on success, UINT64_MAX on error.
 *
 *   void shm_mutex_unlock(shm_mutex_t *m, int id);
 *     Release lock. Must be called by the lock holder.
 *
 *   SHM_MUTEX_VALID(m)
 *     Returns true if mutex is properly initialized.
 */

#endif // SHM_MUTEX_H
