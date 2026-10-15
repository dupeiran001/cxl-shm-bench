// barrier.h
#ifndef BARRIER_H
#define BARRIER_H

#include "common.h"
#include "shm_mutex.h"
#include <stdint.h>

#define BARRIER_MAGIC 0x4241525249455231ULL /* "BARRIER1" */

/*
 * Place this struct in shared memory (your shm_malloc_id result),
 * and all participants must point to the same instance.
 */
typedef struct {
  cacheline_u64 magic;

  shm_mutex_t lock; // protects count/gen init + updates

  cacheline_u32 parties; // total participants
  cacheline_u32 _pad0;

  cacheline_u64 gen;   // generation number
  cacheline_u64 count; // arrivals in current gen
} shm_barrier_t;

/* How long a non-zero participant waits in barrier_init() for participant 0
 * to publish the barrier. */
#ifndef BARRIER_INIT_TIMEOUT_MS
#define BARRIER_INIT_TIMEOUT_MS 60000ULL
#endif

/*
 * barrier_init(): must be called by EACH participant once.
 *  - b: pointer to shared barrier memory
 *  - parties: total participant count
 *  - my_id: [0, parties)
 *
 * Participant 0 is the only writer during initialization and publishes magic
 * last; the others only read, waiting for magic. The memory must be zeroed by
 * its creator before any participant can reach barrier_init(); a barrier that
 * already has magic is validated, never rebuilt.
 *
 * Returns 0, -EINVAL (bad arguments or parties mismatch) or -ETIMEDOUT.
 */
int barrier_init(shm_barrier_t *b, uint32_t parties, uint32_t my_id);

/*
 * barrier_wait(): waits until all parties reach the barrier.
 *  - uses my_id/parties passed in
 */
void barrier_wait(shm_barrier_t *b, uint32_t parties, uint32_t my_id);

#endif
