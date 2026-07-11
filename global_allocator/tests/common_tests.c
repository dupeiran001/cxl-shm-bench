#include <criterion/criterion.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../common.h"
#include "../shm_mutex.h"

typedef struct {
  shm_mutex_t *mutex;
  cacheline_u64 *counter;
  pthread_barrier_t *bar;
  int id;
  int num_hosts;
  int iters;
} worker_args_t;

static void *worker_fn(void *arg) {
  worker_args_t *w = (worker_args_t *)arg;
  pthread_barrier_wait(w->bar);
  for (int i = 0; i < w->iters; ++i) {
    shm_mutex_lock(w->mutex, w->id, w->num_hosts);
    uint64_t v = CACHELINE_LOAD(w->counter);
    CACHELINE_STORE(w->counter, v + 1);
    shm_mutex_unlock(w->mutex, w->id);
  }
  return NULL;
}

Test(common, cacheline_types_aligned) {
  cr_expect_eq(sizeof(cacheline_u64), CACHE_LINE_SIZE);
  cr_expect_eq(sizeof(cacheline_u32), CACHE_LINE_SIZE);
  cr_expect_eq(sizeof(cacheline_size_t), CACHE_LINE_SIZE);
  cr_expect_eq(sizeof(cacheline_bool), CACHE_LINE_SIZE);

  cr_expect_eq(_Alignof(cacheline_u64), CACHE_LINE_SIZE);
  cr_expect_eq(_Alignof(cacheline_u32), CACHE_LINE_SIZE);
  cr_expect_eq(_Alignof(cacheline_size_t), CACHE_LINE_SIZE);
  cr_expect_eq(_Alignof(cacheline_bool), CACHE_LINE_SIZE);
}

Test(common, cacheline_store_load) {
  cacheline_u64 c64 = {0};
  CACHELINE_STORE(&c64, 42);
  cr_expect_eq(CACHELINE_LOAD(&c64), 42);

  cacheline_u32 c32 = {0};
  CACHELINE_STORE(&c32, 7U);
  cr_expect_eq(CACHELINE_LOAD(&c32), 7U);

  cacheline_size_t cs = {0};
  CACHELINE_STORE(&cs, (size_t)1234);
  cr_expect_eq(CACHELINE_LOAD(&cs), (size_t)1234);

  cacheline_bool cb = {0};
  CACHELINE_STORE(&cb, true);
  cr_expect_eq(CACHELINE_LOAD(&cb), true);
}

#ifdef LOCK_IMPL_LFM
Test(lfm, init_sets_magic_and_zeros) {
  shm_mutex_t m;
  memset(&m, 0xAB, sizeof(m));

  shm_mutex_init(&m);

  cr_expect(SHM_MUTEX_VALID(&m), "magic not published after init");
  cr_expect_eq(m.x.value, 0, "x not zeroed");
  cr_expect_eq(m.y.value, 0, "y not zeroed");

  for (int i = 0; i < MAX_HOST_NUM; ++i) {
    cr_expect_eq(m.b[i].value, 0, "b[%d] not zero", i);
    cr_expect_eq(m.ready[i].value, 0, "ready[%d] not zero", i);
    cr_expect_eq(m.done[i].value, 0, "done[%d] not zero", i);
  }
}

Test(lfm, lock_and_unlock_single_thread) {
  shm_mutex_t m;
  shm_mutex_init(&m);

  uint64_t retries = shm_mutex_lock(&m, 0, 2);
  cr_expect_eq(retries, 0, "unexpected retries acquiring lock");
  cr_expect_eq(m.y.value, 1);
  cr_expect_eq(m.b[0].value, 1);

  shm_mutex_unlock(&m, 0);
  cr_expect_eq(m.y.value, 0);
  cr_expect_eq(m.b[0].value, 0);
}
#endif

Test(lfm, two_thread_correctness) {
  const int threads = 2;
  const int iters = 500000;

  struct {
    shm_mutex_t m;
    cacheline_u64 counter;
  } shared;
  memset(&shared, 0, sizeof(shared));
  shm_mutex_init(&shared.m);
  CACHELINE_STORE(&shared.counter, 0);

  pthread_barrier_t bar;
  pthread_barrier_init(&bar, NULL, threads);

  pthread_t tids[threads];
  worker_args_t args[threads];
  for (int i = 0; i < threads; ++i) {
    args[i] = (worker_args_t){
        .mutex = &shared.m,
        .counter = &shared.counter,
        .bar = &bar,
        .id = i,
        .num_hosts = threads,
        .iters = iters,
    };
    pthread_create(&tids[i], NULL, worker_fn, &args[i]);
  }

  for (int i = 0; i < threads; ++i) {
    pthread_join(tids[i], NULL);
  }

  pthread_barrier_destroy(&bar);

  cr_expect_eq(CACHELINE_LOAD(&shared.counter), (uint64_t)(threads * iters));
}
