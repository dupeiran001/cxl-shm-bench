#include <criterion/criterion.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../common.h"
#include "../cxl_atomic.h"

/*
 * thread_args_t - arguments for worker threads
 */
typedef struct {
  cxl_atomic64_t *atom;
  pthread_barrier_t *bar;
  int id;
  int num_hosts;
  int iters;
} thread_args_t;

static void *worker_inc(void *arg) {
  thread_args_t *args = (thread_args_t *)arg;
  pthread_barrier_wait(args->bar);

  for (int i = 0; i < args->iters; ++i) {
    cxl_atomic64_fetch_add(args->atom, 1, args->id, args->num_hosts);
  }
  return NULL;
}

static void *worker_cas(void *arg) {
  thread_args_t *args = (thread_args_t *)arg;
  pthread_barrier_wait(args->bar);

  for (int i = 0; i < args->iters; ++i) {
    bool success = false;
    while (!success) {
      uint64_t old = cxl_atomic64_load(args->atom);
      uint64_t new_val = old + 1;
      /*
       * cxl_atomic64_cmpxchg logic:
       * if (*atom == old) { *atom = new_val; return true; }
       * else { *old = *atom; return false; }
       */
      uint64_t expected = old;
      success = cxl_atomic64_cmpxchg(args->atom, &expected, new_val, args->id,
                                     args->num_hosts);
    }
  }
  return NULL;
}

Test(cxl_atomic, init_and_basic_ops) {
  cxl_atomic64_t atom;
  cxl_atomic64_init(&atom, 100);

  cr_expect_eq(cxl_atomic64_load(&atom), 100);

  cxl_atomic64_store(&atom, 200);
  cr_expect_eq(cxl_atomic64_load(&atom), 200);
}

Test(cxl_atomic, swap_single_thread) {
  cxl_atomic64_t atom;
  cxl_atomic64_init(&atom, 123);

  /* Host 0 (of 1) swapping */
  uint64_t old = cxl_atomic64_swap(&atom, 456, 0, 1);
  cr_expect_eq(old, 123);
  cr_expect_eq(cxl_atomic64_load(&atom), 456);
}

Test(cxl_atomic, cmpxchg_single_thread) {
  cxl_atomic64_t atom;
  cxl_atomic64_init(&atom, 50);

  uint64_t expected = 50;
  bool ret = cxl_atomic64_cmpxchg(&atom, &expected, 60, 0, 1);
  cr_expect(ret, "cmpxchg should succeed");
  cr_expect_eq(cxl_atomic64_load(&atom), 60);

  /* Fail case */
  expected = 999; /* Wrong expectation */
  ret = cxl_atomic64_cmpxchg(&atom, &expected, 70, 0, 1);
  cr_expect(!ret, "cmpxchg should fail");
  cr_expect_eq(expected, 60, "expected should be updated to current val");
  cr_expect_eq(cxl_atomic64_load(&atom), 60);
}

Test(cxl_atomic, fetch_add_concurrency) {
  const int num_threads = 4;
  const int iters = 1000;
  pthread_t threads[4];
  thread_args_t args[4];
  pthread_barrier_t bar;

  cxl_atomic64_t atom;
  cxl_atomic64_init(&atom, 0);

  pthread_barrier_init(&bar, NULL, num_threads);

  for (int i = 0; i < num_threads; ++i) {
    args[i] = (thread_args_t){
        .atom = &atom,
        .bar = &bar,
        .id = i,
        .num_hosts = num_threads,
        .iters = iters,
    };
    pthread_create(&threads[i], NULL, worker_inc, &args[i]);
  }

  for (int i = 0; i < num_threads; ++i) {
    pthread_join(threads[i], NULL);
  }

  pthread_barrier_destroy(&bar);

  cr_expect_eq(cxl_atomic64_load(&atom), (uint64_t)(num_threads * iters));
}

Test(cxl_atomic, cmpxchg_concurrency) {
  const int num_threads = 4;
  const int iters = 1000;
  pthread_t threads[4];
  thread_args_t args[4];
  pthread_barrier_t bar;

  cxl_atomic64_t atom;
  cxl_atomic64_init(&atom, 0);

  pthread_barrier_init(&bar, NULL, num_threads);

  for (int i = 0; i < num_threads; ++i) {
    args[i] = (thread_args_t){
        .atom = &atom,
        .bar = &bar,
        .id = i,
        .num_hosts = num_threads,
        .iters = iters,
    };
    pthread_create(&threads[i], NULL, worker_cas, &args[i]);
  }

  for (int i = 0; i < num_threads; ++i) {
    pthread_join(threads[i], NULL);
  }

  pthread_barrier_destroy(&bar);

  cr_expect_eq(cxl_atomic64_load(&atom), (uint64_t)(num_threads * iters));
}
