#include "config.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct atomicstats_thread {
  struct atomicstats_thread *next;
  uint64_t count;
  unsigned int generation;
};

static _Atomic(struct atomicstats_thread *) atomicstats_threads = NULL;
static __thread struct atomicstats_thread *atomicstats_tls = NULL;
static __thread int atomicstats_inside = 0;

static _Atomic unsigned int atomicstats_generation = 1;
static _Atomic int atomicstats_roi_active = 0;
static _Atomic int atomicstats_printed = 0;

static struct atomicstats_thread *atomicstats_get_tls(void) {
  if (atomicstats_tls) {
    return atomicstats_tls;
  }

  atomicstats_inside = 1;
  struct atomicstats_thread *state = calloc(1, sizeof(*state));
  atomicstats_inside = 0;
  if (!state) {
    return NULL;
  }

  state->generation = atomic_load(&atomicstats_generation);

  struct atomicstats_thread *head;
  do {
    head = atomic_load(&atomicstats_threads);
    state->next = head;
  } while (!atomic_compare_exchange_weak(&atomicstats_threads, &head, state));

  atomicstats_tls = state;
  return state;
}

static void atomicstats_reset_thread(struct atomicstats_thread *state) {
  if (!state) {
    return;
  }
  state->generation = atomic_load(&atomicstats_generation);
  state->count = 0;
}

static void atomicstats_maybe_reset(struct atomicstats_thread *state) {
  unsigned int gen = atomic_load(&atomicstats_generation);
  if (state && state->generation != gen) {
    state->generation = gen;
    state->count = 0;
  }
}

void __parsec_atomicstats_op(void) {
  if (atomicstats_inside) {
    return;
  }
  if (!atomic_load(&atomicstats_roi_active)) {
    return;
  }

  struct atomicstats_thread *state = atomicstats_get_tls();
  if (!state) {
    return;
  }

  atomicstats_inside = 1;
  atomicstats_maybe_reset(state);
  state->count++;
  atomicstats_inside = 0;
}

void __parsec_atomicstats_bench_begin(void) {
  atomic_fetch_add(&atomicstats_generation, 1);
  atomicstats_reset_thread(atomicstats_tls);
  atomic_store(&atomicstats_roi_active, 0);
}

void __parsec_atomicstats_roi_begin(void) {
  atomic_fetch_add(&atomicstats_generation, 1);
  atomicstats_reset_thread(atomicstats_tls);
  atomic_store(&atomicstats_roi_active, 1);
}

void __parsec_atomicstats_roi_end(void) {
  atomic_store(&atomicstats_roi_active, 0);
}

static void atomicstats_print(void) {
  if (atomic_exchange(&atomicstats_printed, 1)) {
    return;
  }

  uint64_t total = 0;
  int threads = 0;

  struct atomicstats_thread *state = atomic_load(&atomicstats_threads);
  while (state) {
    total += state->count;
    threads++;
    state = state->next;
  }

  fprintf(stderr, HOOKS_PREFIX " Atomic ops in ROI: total=%" PRIu64 ", threads=%d\n", total, threads);
}

void __parsec_atomicstats_bench_end(void) {
  atomicstats_print();
}

__attribute__((constructor)) static void atomicstats_ctor(void) {
  atomic_store(&atomicstats_roi_active, 1);
}

__attribute__((destructor)) static void atomicstats_dtor(void) {
  atomicstats_print();
}
