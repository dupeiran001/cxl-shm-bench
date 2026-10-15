// barrier.c
#include "barrier.h"
#include <errno.h>
#include <time.h>

static uint64_t barrier_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

int barrier_init(shm_barrier_t *b, uint32_t parties, uint32_t my_id) {
  if (!b || parties == 0 || parties > MAX_HOST_NUM || my_id >= parties) {
    return -EINVAL;
  }

  if (my_id == 0) {
    /* Already built: validate only. Never touch the lock or gen/count of a
     * barrier that may be in use. */
    if (CACHELINE_LOAD(&b->magic) == BARRIER_MAGIC) {
      return (uint32_t)CACHELINE_LOAD(&b->parties) == parties ? 0 : -EINVAL;
    }

    /* Sole initializer: nobody else writes the barrier before magic is
     * published, so the lock can be built without taking it. */
    shm_mutex_init(&b->lock);
    CACHELINE_STORE(&b->parties, parties);
    CACHELINE_STORE(&b->gen, 0);
    CACHELINE_STORE(&b->count, 0);

    /* Publish magic last. */
    CACHELINE_STORE(&b->magic, BARRIER_MAGIC);
    return 0;
  }

  /* Peers only read until participant 0 has published the barrier. */
  uint64_t deadline = barrier_now_ms() + BARRIER_INIT_TIMEOUT_MS;
  while (CACHELINE_LOAD(&b->magic) != BARRIER_MAGIC) {
    if (barrier_now_ms() >= deadline) {
      return -ETIMEDOUT;
    }
    relax_cpu();
  }
  return (uint32_t)CACHELINE_LOAD(&b->parties) == parties ? 0 : -EINVAL;
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
