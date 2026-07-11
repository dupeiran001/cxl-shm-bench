// barrier.c
#include "barrier.h"
#include <errno.h>

int barrier_init(shm_barrier_t *b, uint32_t parties, uint32_t my_id) {
  if (!b || parties == 0 || parties > MAX_HOST_NUM || my_id >= parties) {
    return -EINVAL;
  }

  /* Best-effort init of the lock itself (same pattern as your shm_enable). */
  if (!SHM_MUTEX_VALID(&b->lock)) {
    shm_mutex_init(&b->lock);
  }

  /* If already initialized, just validate. */
  if (CACHELINE_LOAD(&b->magic) == BARRIER_MAGIC) {
    uint32_t p = (uint32_t)CACHELINE_LOAD(&b->parties);
    if (p != parties)
      return -EINVAL;
    return 0;
  }

  /* Initialize under lock so multiple participants racing here are fine. */
  shm_mutex_lock(&b->lock, (int)my_id, (int)parties);

  if (CACHELINE_LOAD(&b->magic) != BARRIER_MAGIC) {
    CACHELINE_STORE(&b->parties, parties);
    CACHELINE_STORE(&b->gen, 0);
    CACHELINE_STORE(&b->count, 0);

    /* Publish magic last. */
    CACHELINE_STORE(&b->magic, BARRIER_MAGIC);
  } else {
    uint32_t p = (uint32_t)CACHELINE_LOAD(&b->parties);
    if (p != parties) {
      shm_mutex_unlock(&b->lock, (int)my_id);
      return -EINVAL;
    }
  }

  shm_mutex_unlock(&b->lock, (int)my_id);
  return 0;
}

void barrier_wait(shm_barrier_t *b, uint32_t parties, uint32_t my_id) {
  SYNC_STAT_INC(barrier_waits);

  /* Arrive: increment count under lock; last thread advances gen & resets. */
  shm_mutex_lock(&b->lock, (int)my_id, (int)parties);

  /* Read current generation first. */
  uint64_t my_gen = CACHELINE_LOAD(&b->gen);

  uint64_t c = CACHELINE_LOAD(&b->count);
  c++;
  CACHELINE_STORE(&b->count, c);

  if (c == parties) {
    /* Last to arrive: reset count then advance generation. */
    CACHELINE_STORE(&b->count, 0);
    CACHELINE_STORE(&b->gen, my_gen + 1);
    shm_mutex_unlock(&b->lock, (int)my_id);
    return;
  }

  shm_mutex_unlock(&b->lock, (int)my_id);

  /* Wait until generation advances. */
  while (CACHELINE_LOAD(&b->gen) == my_gen) {
    relax_cpu();
  }
}
