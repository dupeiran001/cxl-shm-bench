// tests/lock_test.c - Comprehensive lock implementation tests
// Tests mutual exclusion correctness for all lock implementations
#define _GNU_SOURCE
#include <criterion/criterion.h>
#include <criterion/logging.h>

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../common.h"
#include "../shm_mutex.h"

#define NUM_THREADS 4
#define ITERATIONS 50000

/*
 * Counter mutual exclusion test.
 * If lock provides correct mutual exclusion, then:
 *   global_counter == sum of all per_thread_counter[i]
 */

typedef struct {
  shm_mutex_t mutex;
  cacheline_u64 global_counter;
  cacheline_u64 per_thread[NUM_THREADS];
} shared_counter_t;

typedef struct {
  shared_counter_t *shared;
  uint32_t tid;
  uint32_t num_threads;
  uint32_t iterations;
  atomic_int *fail_flag;
} thread_arg_t;

static void *counter_worker(void *arg) {
  thread_arg_t *a = (thread_arg_t *)arg;
  shared_counter_t *sh = a->shared;
  uint32_t tid = a->tid;
  uint32_t num_threads = a->num_threads;
  uint32_t iterations = a->iterations;

  for (uint32_t i = 0; i < iterations; i++) {
    if (atomic_load(a->fail_flag) != 0) {
      break; // Early exit on failure
    }

    shm_mutex_lock(&sh->mutex, (int)tid, (int)num_threads);

    // Critical section: increment both counters
    uint64_t global = CACHELINE_LOAD(&sh->global_counter);
    uint64_t local = CACHELINE_LOAD(&sh->per_thread[tid]);
    CACHELINE_STORE(&sh->global_counter, global + 1);
    CACHELINE_STORE(&sh->per_thread[tid], local + 1);

    shm_mutex_unlock(&sh->mutex, (int)tid);
  }

  return NULL;
}

Test(lock, counter_mutual_exclusion, .timeout = 30) {
  shared_counter_t sh;
  memset(&sh, 0, sizeof(sh));
  shm_mutex_init(&sh.mutex);

  cr_assert(SHM_MUTEX_VALID(&sh.mutex), "Lock should be valid after init");

  atomic_int fail_flag = 0;

  pthread_t threads[NUM_THREADS];
  thread_arg_t args[NUM_THREADS];

  for (uint32_t i = 0; i < NUM_THREADS; i++) {
    args[i] = (thread_arg_t){
        .shared = &sh,
        .tid = i,
        .num_threads = NUM_THREADS,
        .iterations = ITERATIONS,
        .fail_flag = &fail_flag,
    };
    int rc = pthread_create(&threads[i], NULL, counter_worker, &args[i]);
    cr_assert_eq(rc, 0, "pthread_create failed");
  }

  for (uint32_t i = 0; i < NUM_THREADS; i++) {
    pthread_join(threads[i], NULL);
  }

  // Verify correctness
  uint64_t global = CACHELINE_LOAD(&sh.global_counter);
  uint64_t sum = 0;
  for (uint32_t i = 0; i < NUM_THREADS; i++) {
    sum += CACHELINE_LOAD(&sh.per_thread[i]);
  }

  cr_log_info("global_counter=%" PRIu64 " sum=%" PRIu64, global, sum);

  cr_assert_eq(global, sum,
               "Mutual exclusion violated: global=%" PRIu64 " != sum=%" PRIu64,
               global, sum);

  uint64_t expected = (uint64_t)NUM_THREADS * ITERATIONS;
  cr_assert_eq(global, expected,
               "Counter mismatch: got %" PRIu64 " expected %" PRIu64, global,
               expected);
}

Test(lock, basic_init_valid, .timeout = 5) {
  shm_mutex_t m;
  memset(&m, 0, sizeof(m));

  cr_assert(!SHM_MUTEX_VALID(&m), "Uninitialized lock should be invalid");

  shm_mutex_init(&m);

  cr_assert(SHM_MUTEX_VALID(&m), "Initialized lock should be valid");
}

Test(lock, single_thread_lock_unlock, .timeout = 5) {
  shm_mutex_t m;
  memset(&m, 0, sizeof(m));
  shm_mutex_init(&m);

  for (int i = 0; i < 1000; i++) {
    uint64_t ret = shm_mutex_lock(&m, 0, 1);
    cr_assert_neq(ret, UINT64_MAX, "Lock should succeed");
    shm_mutex_unlock(&m, 0);
  }
}

Test(lock, two_thread_alternating, .timeout = 10) {
  // Two threads alternate locking
  static struct {
    shm_mutex_t mutex;
    cacheline_u64 counter;
    atomic_int done[2];
  } sh;

  memset(&sh, 0, sizeof(sh));
  shm_mutex_init(&sh.mutex);

  pthread_t t1, t2;

  void *worker(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < 10000; i++) {
      shm_mutex_lock(&sh.mutex, id, 2);
      uint64_t v = CACHELINE_LOAD(&sh.counter);
      CACHELINE_STORE(&sh.counter, v + 1);
      shm_mutex_unlock(&sh.mutex, id);
    }
    atomic_store(&sh.done[id], 1);
    return NULL;
  }

  pthread_create(&t1, NULL, worker, (void *)0);
  pthread_create(&t2, NULL, worker, (void *)1);

  pthread_join(t1, NULL);
  pthread_join(t2, NULL);

  uint64_t final = CACHELINE_LOAD(&sh.counter);
  cr_assert_eq(final, 20000, "Expected 20000, got %" PRIu64, final);
}

Test(lock, invalid_args_return_error, .timeout = 5) {
  shm_mutex_t m;
  shm_mutex_init(&m);

  // Invalid id (negative)
  uint64_t ret = shm_mutex_lock(&m, -1, 2);
  cr_assert_eq(ret, UINT64_MAX, "Negative id should return error");

  // Invalid id (>= num_hosts)
  ret = shm_mutex_lock(&m, 5, 4);
  cr_assert_eq(ret, UINT64_MAX, "id >= num_hosts should return error");

  // Invalid num_hosts
  ret = shm_mutex_lock(&m, 0, 0);
  cr_assert_eq(ret, UINT64_MAX, "num_hosts=0 should return error");

  ret = shm_mutex_lock(&m, 0, MAX_HOST_NUM + 1);
  cr_assert_eq(ret, UINT64_MAX, "num_hosts > MAX should return error");
}
