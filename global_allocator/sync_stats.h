/*
 * sync_stats.h - optional synchronization-event counters (SYNC_STATS profiling).
 *
 * Build the library (and its consumers) with -DSYNC_STATS to count, per
 * process, how often the software-coherence primitives run:
 *
 *   publish_lines   cachelines written back for cross-host visibility
 *                   (STORE_CACHELINE / flush_region)
 *   observe_lines   cachelines invalidated + refetched (LOAD_CACHELINE)
 *   lock_acquires   shm_mutex (LFM) acquisitions
 *   lock_retries    LFM contention retries (attempts beyond the first)
 *   barrier_waits   cross-host barrier_wait() calls
 *   atomic_rmw      software atomic RMW ops (swap/cmpxchg/fetch_add/add)
 *
 * Counters are per-process, relaxed, in local DRAM — they never touch the
 * shared region and cost one relaxed add when enabled, nothing when disabled.
 * Dump one machine-readable line at end of run with sync_stats_dump(tag).
 * Uses __atomic builtins (not C11 _Atomic) so C++ consumers can include this.
 */
#ifndef SYNC_STATS_H
#define SYNC_STATS_H

#include <stdint.h>

#ifdef SYNC_STATS

typedef struct {
  uint64_t publish_lines;
  uint64_t observe_lines;
  uint64_t lock_acquires;
  uint64_t lock_retries;
  uint64_t barrier_waits;
  uint64_t atomic_rmw;
} sync_stats_t;

#ifdef __cplusplus
extern "C" {
#endif
extern sync_stats_t g_sync_stats;
void sync_stats_reset(void);
void sync_stats_dump(const char *tag);
#ifdef __cplusplus
}
#endif

#define SYNC_STAT_ADD(field, n)                                                \
  __atomic_fetch_add(&g_sync_stats.field, (uint64_t)(n), __ATOMIC_RELAXED)
#define SYNC_STAT_INC(field) SYNC_STAT_ADD(field, 1)

#else /* !SYNC_STATS */

#define SYNC_STAT_ADD(field, n) ((void)0)
#define SYNC_STAT_INC(field) ((void)0)
#ifdef __cplusplus
extern "C" {
#endif
static inline void sync_stats_reset(void) {}
static inline void sync_stats_dump(const char *tag) { (void)tag; }
#ifdef __cplusplus
}
#endif

#endif /* SYNC_STATS */

#endif /* SYNC_STATS_H */
