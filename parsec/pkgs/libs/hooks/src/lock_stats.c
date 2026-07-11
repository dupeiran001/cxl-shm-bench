#include "config.h"

#include <pthread.h>
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/syscall.h>
#endif

#ifndef LOCKSTATS_MAP_SIZE
#define LOCKSTATS_MAP_SIZE 1024
#endif

struct lockstats_entry {
  void *lock;
  uint64_t start_ns;
  uint32_t recursion;
  uint8_t in_roi;
};

struct lockstats_thread {
  struct lockstats_thread *next;
  int thread_index;
  uintptr_t tid;
  uint64_t total_ns;
  uint64_t max_ns;
  uint64_t count;
  uint64_t barrier_total_ns;
  uint64_t barrier_max_ns;
  uint64_t barrier_count;
  uint64_t barrier_wait_start_ns;
  struct lockstats_entry map[LOCKSTATS_MAP_SIZE];
  uint8_t state[LOCKSTATS_MAP_SIZE];
};

static _Atomic(struct lockstats_thread *) lockstats_threads = NULL;
static _Atomic int lockstats_next_thread = 0;

static __thread struct lockstats_thread *lockstats_tls = NULL;
static __thread int lockstats_inside = 0;

static _Atomic int lockstats_roi_active = 0;
static _Atomic uint64_t lockstats_roi_begin_ns = 0;
static _Atomic uint64_t lockstats_roi_end_ns = 0;

static int (*real_pthread_mutex_lock)(pthread_mutex_t *);
static int (*real_pthread_mutex_trylock)(pthread_mutex_t *);
static int (*real_pthread_mutex_timedlock)(pthread_mutex_t *, const struct timespec *);
static int (*real_pthread_mutex_unlock)(pthread_mutex_t *);
static int (*real_pthread_rwlock_rdlock)(pthread_rwlock_t *);
static int (*real_pthread_rwlock_tryrdlock)(pthread_rwlock_t *);
static int (*real_pthread_rwlock_wrlock)(pthread_rwlock_t *);
static int (*real_pthread_rwlock_trywrlock)(pthread_rwlock_t *);
static int (*real_pthread_rwlock_unlock)(pthread_rwlock_t *);
static int (*real_pthread_spin_lock)(pthread_spinlock_t *);
static int (*real_pthread_spin_trylock)(pthread_spinlock_t *);
static int (*real_pthread_spin_unlock)(pthread_spinlock_t *);
static int (*real_pthread_cond_wait)(pthread_cond_t *, pthread_mutex_t *);
static int (*real_pthread_cond_timedwait)(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
static int (*real_pthread_barrier_wait)(pthread_barrier_t *);

static uint64_t lockstats_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void lockstats_init_real(void) {
  if (real_pthread_mutex_lock) {
    return;
  }

  lockstats_inside = 1;
  real_pthread_mutex_lock = dlsym(RTLD_NEXT, "pthread_mutex_lock");
  real_pthread_mutex_trylock = dlsym(RTLD_NEXT, "pthread_mutex_trylock");
  real_pthread_mutex_timedlock = dlsym(RTLD_NEXT, "pthread_mutex_timedlock");
  real_pthread_mutex_unlock = dlsym(RTLD_NEXT, "pthread_mutex_unlock");
  real_pthread_rwlock_rdlock = dlsym(RTLD_NEXT, "pthread_rwlock_rdlock");
  real_pthread_rwlock_tryrdlock = dlsym(RTLD_NEXT, "pthread_rwlock_tryrdlock");
  real_pthread_rwlock_wrlock = dlsym(RTLD_NEXT, "pthread_rwlock_wrlock");
  real_pthread_rwlock_trywrlock = dlsym(RTLD_NEXT, "pthread_rwlock_trywrlock");
  real_pthread_rwlock_unlock = dlsym(RTLD_NEXT, "pthread_rwlock_unlock");
  real_pthread_spin_lock = dlsym(RTLD_NEXT, "pthread_spin_lock");
  real_pthread_spin_trylock = dlsym(RTLD_NEXT, "pthread_spin_trylock");
  real_pthread_spin_unlock = dlsym(RTLD_NEXT, "pthread_spin_unlock");
  real_pthread_cond_wait = dlsym(RTLD_NEXT, "pthread_cond_wait");
  real_pthread_cond_timedwait = dlsym(RTLD_NEXT, "pthread_cond_timedwait");
  real_pthread_barrier_wait = dlsym(RTLD_NEXT, "pthread_barrier_wait");
  lockstats_inside = 0;
}

__attribute__((constructor))
static void lockstats_ctor(void) {
  lockstats_init_real();
}

static uintptr_t lockstats_thread_id(void) {
#ifdef __linux__
  return (uintptr_t)syscall(SYS_gettid);
#else
  return (uintptr_t)pthread_self();
#endif
}

static struct lockstats_thread *lockstats_get_tls(void) {
  if (lockstats_tls) {
    return lockstats_tls;
  }

  lockstats_inside = 1;
  struct lockstats_thread *state = calloc(1, sizeof(*state));
  lockstats_inside = 0;
  if (!state) {
    return NULL;
  }

  state->thread_index = atomic_fetch_add(&lockstats_next_thread, 1);
  state->tid = lockstats_thread_id();

  struct lockstats_thread *head;
  do {
    head = atomic_load(&lockstats_threads);
    state->next = head;
  } while (!atomic_compare_exchange_weak(&lockstats_threads, &head, state));

  lockstats_tls = state;
  return state;
}

static size_t lockstats_hash(void *lock) {
  uintptr_t v = (uintptr_t)lock;
  v >>= 3;
  v ^= v >> 16;
  return (size_t)(v & (LOCKSTATS_MAP_SIZE - 1));
}

static struct lockstats_entry *lockstats_map_lookup(struct lockstats_thread *state,
                                                    void *lock,
                                                    int create) {
  size_t idx = lockstats_hash(lock);
  size_t first_del = (size_t)-1;

  for (size_t step = 0; step < LOCKSTATS_MAP_SIZE; step++) {
    size_t pos = (idx + step) & (LOCKSTATS_MAP_SIZE - 1);
    uint8_t st = state->state[pos];
    if (st == 0) {
      if (!create) {
        return NULL;
      }
      if (first_del != (size_t)-1) {
        pos = first_del;
      }
      state->state[pos] = 1;
      state->map[pos].lock = lock;
      state->map[pos].recursion = 0;
      state->map[pos].in_roi = 0;
      return &state->map[pos];
    }
    if (st == 2) {
      if (first_del == (size_t)-1) {
        first_del = pos;
      }
      continue;
    }
    if (state->map[pos].lock == lock) {
      return &state->map[pos];
    }
  }

  return NULL;
}

static void lockstats_map_delete(struct lockstats_thread *state,
                                 struct lockstats_entry *entry) {
  size_t pos = (size_t)(entry - state->map);
  state->state[pos] = 2;
  state->map[pos].lock = NULL;
  state->map[pos].recursion = 0;
  state->map[pos].in_roi = 0;
  state->map[pos].start_ns = 0;
}

static void lockstats_on_lock(void *lock) {
  if (!atomic_load(&lockstats_roi_active)) {
    return;
  }

  struct lockstats_thread *state = lockstats_get_tls();
  if (!state) {
    return;
  }

  struct lockstats_entry *entry = lockstats_map_lookup(state, lock, 1);
  if (!entry) {
    return;
  }

  if (entry->recursion == 0) {
    entry->start_ns = lockstats_now_ns();
    entry->in_roi = 1;
  }
  entry->recursion++;
}

static void lockstats_on_unlock(void *lock) {
  struct lockstats_thread *state = lockstats_get_tls();
  if (!state) {
    return;
  }

  struct lockstats_entry *entry = lockstats_map_lookup(state, lock, 0);
  if (!entry) {
    return;
  }

  if (entry->recursion > 1) {
    entry->recursion--;
    return;
  }

  if (entry->in_roi) {
    uint64_t end_ns = lockstats_now_ns();
    uint64_t roi_end = atomic_load(&lockstats_roi_end_ns);
    if (roi_end && end_ns > roi_end) {
      end_ns = roi_end;
    }
    if (end_ns > entry->start_ns) {
      uint64_t delta = end_ns - entry->start_ns;
      state->total_ns += delta;
      if (delta > state->max_ns) {
        state->max_ns = delta;
      }
      state->count += 1;
    }
  }

  lockstats_map_delete(state, entry);
}

static void barrierstats_on_wait_begin(void) {
  if (!atomic_load(&lockstats_roi_active)) {
    return;
  }

  struct lockstats_thread *state = lockstats_get_tls();
  if (!state) {
    return;
  }

  state->barrier_wait_start_ns = lockstats_now_ns();
}

static void barrierstats_on_wait_end(void) {
  if (!atomic_load(&lockstats_roi_active)) {
    return;
  }

  struct lockstats_thread *state = lockstats_get_tls();
  if (!state) {
    return;
  }

  uint64_t start_ns = state->barrier_wait_start_ns;
  if (!start_ns) {
    return;
  }

  uint64_t delta = lockstats_now_ns() - start_ns;
  state->barrier_wait_start_ns = 0;
  state->barrier_total_ns += delta;
  state->barrier_count += 1;
  if (delta > state->barrier_max_ns) {
    state->barrier_max_ns = delta;
  }
}

void __parsec_barrierstats_wait_begin(void) {
  barrierstats_on_wait_begin();
}

void __parsec_barrierstats_wait_end(void) {
  barrierstats_on_wait_end();
}

void __parsec_lockstats_bench_begin(void) {
  atomic_store(&lockstats_roi_active, 0);
  atomic_store(&lockstats_roi_begin_ns, 0);
  atomic_store(&lockstats_roi_end_ns, 0);
}

void __parsec_lockstats_roi_begin(void) {
  atomic_store(&lockstats_roi_begin_ns, lockstats_now_ns());
  atomic_store(&lockstats_roi_end_ns, 0);
  atomic_store(&lockstats_roi_active, 1);

  struct lockstats_thread *state = atomic_load(&lockstats_threads);
  while (state) {
    state->barrier_total_ns = 0;
    state->barrier_max_ns = 0;
    state->barrier_count = 0;
    state->barrier_wait_start_ns = 0;
    state = state->next;
  }
}

void __parsec_lockstats_roi_end(void) {
  atomic_store(&lockstats_roi_end_ns, lockstats_now_ns());
  atomic_store(&lockstats_roi_active, 0);
}

void __parsec_lockstats_bench_end(void) {
  uint64_t roi_begin = atomic_load(&lockstats_roi_begin_ns);
  uint64_t roi_end = atomic_load(&lockstats_roi_end_ns);
  if (roi_begin && !roi_end) {
    roi_end = lockstats_now_ns();
  }
  uint64_t roi_ns = (roi_end > roi_begin) ? (roi_end - roi_begin) : 0;

  uint64_t total_ns = 0;
  uint64_t total_count = 0;
  uint64_t max_ns = 0;
  int thread_count = 0;

  struct lockstats_thread *state = atomic_load(&lockstats_threads);
  while (state) {
    if (state->count > 0) {
      thread_count++;
    }
    total_ns += state->total_ns;
    total_count += state->count;
    if (state->max_ns > max_ns) {
      max_ns = state->max_ns;
    }
    state = state->next;
  }

  double roi_s = (double)roi_ns / 1e9;
  double total_s = (double)total_ns / 1e9;
  double avg_us = total_count ? ((double)total_ns / (double)total_count) / 1000.0 : 0.0;
  double max_us = (double)max_ns / 1000.0;
  double ratio = (roi_ns && thread_count) ? (double)total_ns / (double)roi_ns / (double)thread_count : 0.0;
  int have_locks = total_count > 0;

  fprintf(stderr, HOOKS_PREFIX " Lockstats: ROI duration %.6fs\n", roi_s);
  if (!have_locks) {
    fprintf(stderr, HOOKS_PREFIX " Lockstats: lock-based sync in ROI: no (no lock acquisitions)\n");
  } else {
    fprintf(stderr, HOOKS_PREFIX " Lockstats: lock-based sync in ROI: yes (acquisitions=%" PRIu64 ", threads=%d)\n",
           total_count, thread_count);
    fprintf(stderr, HOOKS_PREFIX " Lockstats: critical-section time sum %.6fs, avg %.3fus, max %.3fus\n",
           total_s, avg_us, max_us);
    if (roi_ns && thread_count) {
      fprintf(stderr, HOOKS_PREFIX " Lockstats: critical-section ratio (sum/(ROI*threads)) %.2f%%\n",
             ratio * 100.0);
    } else {
      fprintf(stderr, HOOKS_PREFIX " Lockstats: critical-section ratio (sum/(ROI*threads)) n/a\n");
    }

    state = atomic_load(&lockstats_threads);
    while (state) {
      if (state->count > 0) {
        double thread_s = (double)state->total_ns / 1e9;
        double thread_avg_us = ((double)state->total_ns / (double)state->count) / 1000.0;
        double thread_max_us = (double)state->max_ns / 1000.0;
        double thread_ratio = roi_ns ? (double)state->total_ns / (double)roi_ns : 0.0;
        fprintf(stderr, HOOKS_PREFIX " Lockstats: thread %d tid=0x%" PRIxPTR " locks=%" PRIu64
                           " total=%.6fs avg=%.3fus max=%.3fus ratio=%.2f%%\n",
               state->thread_index, state->tid, state->count,
               thread_s, thread_avg_us, thread_max_us, thread_ratio * 100.0);
      }
      state = state->next;
    }
  }

  uint64_t barrier_total_ns = 0;
  uint64_t barrier_max_ns = 0;
  uint64_t barrier_count = 0;
  int barrier_threads = 0;

  state = atomic_load(&lockstats_threads);
  while (state) {
    if (state->barrier_count > 0) {
      barrier_threads++;
    }
    barrier_total_ns += state->barrier_total_ns;
    barrier_count += state->barrier_count;
    if (state->barrier_max_ns > barrier_max_ns) {
      barrier_max_ns = state->barrier_max_ns;
    }
    state = state->next;
  }

  double barrier_total_s = (double)barrier_total_ns / 1e9;
  double barrier_avg_us = barrier_count ? ((double)barrier_total_ns / (double)barrier_count) / 1000.0 : 0.0;
  double barrier_max_us = (double)barrier_max_ns / 1000.0;
  double barrier_ratio = (roi_ns && barrier_threads) ? (double)barrier_total_ns / (double)roi_ns / (double)barrier_threads : 0.0;

  fprintf(stderr, HOOKS_PREFIX " Barrierstats: ROI duration %.6fs\n", roi_s);
  if (barrier_count == 0) {
    fprintf(stderr, HOOKS_PREFIX " Barrierstats: barrier waits in ROI: no (count=0)\n");
    return;
  }

  fprintf(stderr, HOOKS_PREFIX " Barrierstats: barrier waits in ROI: yes (waits=%" PRIu64 ", threads=%d)\n",
         barrier_count, barrier_threads);
  fprintf(stderr, HOOKS_PREFIX " Barrierstats: wait time sum %.6fs, avg %.3fus, max %.3fus\n",
         barrier_total_s, barrier_avg_us, barrier_max_us);
  if (roi_ns && barrier_threads) {
    fprintf(stderr, HOOKS_PREFIX " Barrierstats: wait ratio (sum/(ROI*threads)) %.2f%%\n",
           barrier_ratio * 100.0);
  } else {
    fprintf(stderr, HOOKS_PREFIX " Barrierstats: wait ratio (sum/(ROI*threads)) n/a\n");
  }

  state = atomic_load(&lockstats_threads);
  while (state) {
    if (state->barrier_count > 0) {
      double thread_s = (double)state->barrier_total_ns / 1e9;
      double thread_avg_us = ((double)state->barrier_total_ns / (double)state->barrier_count) / 1000.0;
      double thread_max_us = (double)state->barrier_max_ns / 1000.0;
      double thread_ratio = roi_ns ? (double)state->barrier_total_ns / (double)roi_ns : 0.0;
      fprintf(stderr, HOOKS_PREFIX " Barrierstats: thread %d tid=0x%" PRIxPTR " waits=%" PRIu64
                         " total=%.6fs avg=%.3fus max=%.3fus ratio=%.2f%%\n",
             state->thread_index, state->tid, state->barrier_count,
             thread_s, thread_avg_us, thread_max_us, thread_ratio * 100.0);
    }
    state = state->next;
  }
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
  lockstats_init_real();
  if (lockstats_inside) {
    return real_pthread_mutex_lock(mutex);
  }
  int rv = real_pthread_mutex_lock(mutex);
  if (rv == 0) {
    lockstats_on_lock((void *)mutex);
  }
  return rv;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
  lockstats_init_real();
  if (lockstats_inside) {
    return real_pthread_mutex_trylock(mutex);
  }
  int rv = real_pthread_mutex_trylock(mutex);
  if (rv == 0) {
    lockstats_on_lock((void *)mutex);
  }
  return rv;
}

int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *ts) {
  lockstats_init_real();
  if (lockstats_inside) {
    if (!real_pthread_mutex_timedlock) {
      return ENOSYS;
    }
    return real_pthread_mutex_timedlock(mutex, ts);
  }
  if (!real_pthread_mutex_timedlock) {
    return ENOSYS;
  }
  int rv = real_pthread_mutex_timedlock(mutex, ts);
  if (rv == 0) {
    lockstats_on_lock((void *)mutex);
  }
  return rv;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  lockstats_init_real();
  if (lockstats_inside) {
    return real_pthread_mutex_unlock(mutex);
  }
  int rv = real_pthread_mutex_unlock(mutex);
  if (rv == 0) {
    lockstats_on_unlock((void *)mutex);
  }
  return rv;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *lock) {
  lockstats_init_real();
  if (lockstats_inside) {
    return real_pthread_rwlock_rdlock(lock);
  }
  int rv = real_pthread_rwlock_rdlock(lock);
  if (rv == 0) {
    lockstats_on_lock((void *)lock);
  }
  return rv;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *lock) {
  lockstats_init_real();
  if (lockstats_inside) {
    return real_pthread_rwlock_tryrdlock(lock);
  }
  int rv = real_pthread_rwlock_tryrdlock(lock);
  if (rv == 0) {
    lockstats_on_lock((void *)lock);
  }
  return rv;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *lock) {
  lockstats_init_real();
  if (lockstats_inside) {
    return real_pthread_rwlock_wrlock(lock);
  }
  int rv = real_pthread_rwlock_wrlock(lock);
  if (rv == 0) {
    lockstats_on_lock((void *)lock);
  }
  return rv;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *lock) {
  lockstats_init_real();
  if (lockstats_inside) {
    return real_pthread_rwlock_trywrlock(lock);
  }
  int rv = real_pthread_rwlock_trywrlock(lock);
  if (rv == 0) {
    lockstats_on_lock((void *)lock);
  }
  return rv;
}

int pthread_rwlock_unlock(pthread_rwlock_t *lock) {
  lockstats_init_real();
  if (lockstats_inside) {
    return real_pthread_rwlock_unlock(lock);
  }
  int rv = real_pthread_rwlock_unlock(lock);
  if (rv == 0) {
    lockstats_on_unlock((void *)lock);
  }
  return rv;
}

int pthread_spin_lock(pthread_spinlock_t *lock) {
  lockstats_init_real();
  if (lockstats_inside) {
    if (!real_pthread_spin_lock) {
      return ENOSYS;
    }
    return real_pthread_spin_lock(lock);
  }
  if (!real_pthread_spin_lock) {
    return ENOSYS;
  }
  int rv = real_pthread_spin_lock(lock);
  if (rv == 0) {
    lockstats_on_lock((void *)lock);
  }
  return rv;
}

int pthread_spin_trylock(pthread_spinlock_t *lock) {
  lockstats_init_real();
  if (lockstats_inside) {
    if (!real_pthread_spin_trylock) {
      return ENOSYS;
    }
    return real_pthread_spin_trylock(lock);
  }
  if (!real_pthread_spin_trylock) {
    return ENOSYS;
  }
  int rv = real_pthread_spin_trylock(lock);
  if (rv == 0) {
    lockstats_on_lock((void *)lock);
  }
  return rv;
}

int pthread_spin_unlock(pthread_spinlock_t *lock) {
  lockstats_init_real();
  if (lockstats_inside) {
    if (!real_pthread_spin_unlock) {
      return ENOSYS;
    }
    return real_pthread_spin_unlock(lock);
  }
  if (!real_pthread_spin_unlock) {
    return ENOSYS;
  }
  int rv = real_pthread_spin_unlock(lock);
  if (rv == 0) {
    lockstats_on_unlock((void *)lock);
  }
  return rv;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  lockstats_init_real();
  if (lockstats_inside) {
    return real_pthread_cond_wait(cond, mutex);
  }
  lockstats_on_unlock((void *)mutex);
  int rv = real_pthread_cond_wait(cond, mutex);
  if (rv == 0) {
    lockstats_on_lock((void *)mutex);
  }
  return rv;
}

int pthread_cond_timedwait(pthread_cond_t *cond,
                           pthread_mutex_t *mutex,
                           const struct timespec *ts) {
  lockstats_init_real();
  if (lockstats_inside) {
    return real_pthread_cond_timedwait(cond, mutex, ts);
  }
  lockstats_on_unlock((void *)mutex);
  int rv = real_pthread_cond_timedwait(cond, mutex, ts);
  if (rv == 0) {
    lockstats_on_lock((void *)mutex);
  }
  return rv;
}

int pthread_barrier_wait(pthread_barrier_t *barrier) {
  lockstats_init_real();
  if (lockstats_inside) {
    if (!real_pthread_barrier_wait) {
      return ENOSYS;
    }
    return real_pthread_barrier_wait(barrier);
  }
  if (!real_pthread_barrier_wait) {
    return ENOSYS;
  }
  barrierstats_on_wait_begin();
  int rv = real_pthread_barrier_wait(barrier);
  barrierstats_on_wait_end();
  return rv;
}
