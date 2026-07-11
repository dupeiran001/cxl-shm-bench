// tests/barrier_test.c
#define _GNU_SOURCE
#include <criterion/criterion.h>
#include <criterion/logging.h>

#include <errno.h>
#include <inttypes.h> // PRIu64
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../barrier.h"
#include "../common.h"

#define ITER 20000

/*
 * Threaded correctness test for barrier_init()/barrier_wait().
 *
 * We test that:
 *  1) No thread passes barrier phase-1 until all have written "arrive[t]=iter".
 *  2) No thread passes barrier phase-2 until all have written "leave[t]=iter".
 *  3) Repeats many times.
 *
 * If barrier is incorrect, we:
 *  - set a shared fail flag,
 *  - capture a message,
 *  - let all threads exit,
 *  - main test asserts fail with that message.
 *
 * If barrier deadlocks, Criterion .timeout kills the test and reports failure.
 */

enum { NTHR = 4 };

typedef struct {
  shm_barrier_t bar;

  /* Per-thread observable state (cacheline-separated). */
  struct {
    cacheline_u64 v;
  } arrive[NTHR];

  struct {
    cacheline_u64 v;
  } leave[NTHR];

  /* Fail-fast reporting */
  cacheline_u64 fail; /* 0 ok, 1 failed */
  char fail_msg[256];
} shared_t;

typedef struct {
  shared_t *sh;
  uint32_t tid;
  uint32_t iters;
} thr_arg_t;

static void fail_now(shared_t *sh, const char *fmt, ...) {
  /* Only first failure wins. */
  if (CACHELINE_LOAD(&sh->fail) != 0)
    return;

  CACHELINE_STORE(&sh->fail, 1);

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(sh->fail_msg, sizeof(sh->fail_msg), fmt, ap);
  va_end(ap);

  /* Make message visible */
  flush_region(sh->fail_msg, sizeof(sh->fail_msg));
}

static inline uint64_t load_u64_slot(cacheline_u64 *cl) {
  return CACHELINE_LOAD(cl);
}
static inline void store_u64_slot(cacheline_u64 *cl, uint64_t v) {
  CACHELINE_STORE(cl, v);
}

static void *worker(void *p) {
  thr_arg_t *a = (thr_arg_t *)p;
  shared_t *sh = a->sh;
  uint32_t tid = a->tid;
  uint32_t iters = a->iters;

  int rc = barrier_init(&sh->bar, NTHR, tid);
  if (rc != 0) {
    fail_now(sh, "thread %u: barrier_init failed rc=%d", tid, rc);
    return NULL;
  }

  for (uint32_t i = 1; i <= iters; i++) {
    if (load_u64_slot(&sh->fail) != 0)
      break;

    /* Phase 1: publish arrive[tid]=i, then barrier */
    store_u64_slot(&sh->arrive[tid].v, (uint64_t)i);

    barrier_wait(&sh->bar, NTHR, tid);

    /* After phase-1 barrier, all arrive[*] must be == i */
    for (uint32_t t = 0; t < NTHR; t++) {
      uint64_t got = load_u64_slot(&sh->arrive[t].v);
      if (got != (uint64_t)i) {
        fail_now(sh,
                 "thread %u: phase1 failed iter=%u: arrive[%u]=%" PRIu64
                 " expected=%u",
                 tid, i, t, got, i);
        break;
      }
    }
    if (load_u64_slot(&sh->fail) != 0)
      break;

    /* Phase 2: publish leave[tid]=i, then barrier */
    store_u64_slot(&sh->leave[tid].v, (uint64_t)i);

    barrier_wait(&sh->bar, NTHR, tid);

    /* After phase-2 barrier, all leave[*] must be == i */
    for (uint32_t t = 0; t < NTHR; t++) {
      uint64_t got = load_u64_slot(&sh->leave[t].v);
      if (got != (uint64_t)i) {
        fail_now(sh,
                 "thread %u: phase2 failed iter=%u: leave[%u]=%" PRIu64
                 " expected=%u",
                 tid, i, t, got, i);
        break;
      }
    }
    if (load_u64_slot(&sh->fail) != 0)
      break;
  }

  return NULL;
}

Test(barrier, threaded_correctness, .timeout = 5) {
  shared_t sh;
  memset(&sh, 0, sizeof(sh));

  pthread_t th[NTHR];
  thr_arg_t args[NTHR];
  for (uint32_t i = 0; i < NTHR; i++) {
    args[i] = (thr_arg_t){
        .sh = &sh,
        .tid = i,
        .iters = ITER,
    };
    pthread_create(&th[i], NULL, worker, &args[i]);
  }

  for (uint32_t i = 0; i < NTHR; i++) {
    pthread_join(th[i], NULL);
  }

  if (CACHELINE_LOAD(&sh.fail) != 0) {
    cr_expect_fail("barrier test failed: %s", sh.fail_msg);
  }
}
