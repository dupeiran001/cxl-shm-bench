#ifndef COMMON_H
#define COMMON_H

#include <immintrin.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sync_stats.h"

#define CACHE_LINE_SIZE 64

#define MAX_HOST_NUM 4

#define CACHELINE_DEF(type, name)                                              \
  typedef struct {                                                             \
    alignas(CACHE_LINE_SIZE) volatile type value;                              \
    uint8_t padding[CACHE_LINE_SIZE - sizeof(type)];                           \
  } name

CACHELINE_DEF(uint64_t, cacheline_u64);
CACHELINE_DEF(uint32_t, cacheline_u32);
CACHELINE_DEF(size_t, cacheline_size_t);
CACHELINE_DEF(bool, cacheline_bool);

static inline void compiler_barrier(void) {
  __asm__ __volatile__("" ::: "memory");
}

static inline void full_fence(void) { _mm_mfence(); }

static inline void store_fence(void) { _mm_sfence(); }

static inline void flush_line(void *p) {
#if defined(__CLFLUSHOPT__)
  _mm_clflushopt(p);
#else
  _mm_clflush(p);
#endif
}

static inline void flush_region(void *ptr, size_t len) {
  uintptr_t start = (uintptr_t)ptr;
  uintptr_t end = start + len;
  start &= ~(uintptr_t)(CACHE_LINE_SIZE - 1);
  SYNC_STAT_ADD(publish_lines, (end - start + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE);
  for (uintptr_t p = start; p < end; p += CACHE_LINE_SIZE) {
    flush_line((void *)p);
  }
  store_fence();
}

static inline uint64_t align_up_u64(uint64_t x, uint64_t a) {
  return (x + a - 1) & ~(a - 1);
}

#define STORE_CACHELINE(ptr, val)                                              \
  do {                                                                         \
    SYNC_STAT_INC(publish_lines);                                              \
    compiler_barrier();                                                        \
    *(ptr) = (val);                                                            \
    compiler_barrier();                                                        \
    flush_line((void *)(ptr));                                                 \
    store_fence();                                                             \
    compiler_barrier();                                                        \
  } while (0)

#define LOAD_CACHELINE(ptr)                                                    \
  ({                                                                           \
    __typeof__(*(ptr)) _v;                                                     \
    SYNC_STAT_INC(observe_lines);                                              \
    compiler_barrier();                                                        \
    flush_line((void *)(ptr));                                                 \
    full_fence();                                                              \
    compiler_barrier();                                                        \
    _v = *(ptr);                                                               \
    compiler_barrier();                                                        \
    _v;                                                                        \
  })

#define CACHELINE_STORE(cl_ptr, val) STORE_CACHELINE(&(cl_ptr)->value, (val))

#define CACHELINE_LOAD(cl_ptr) LOAD_CACHELINE(&(cl_ptr)->value)

static inline void store_cacheline(volatile uint64_t *slot, uint64_t v) {
  STORE_CACHELINE(slot, v);
}

static inline uint64_t load_cacheline(volatile uint64_t *slot) {
  return LOAD_CACHELINE(slot);
}

static inline void relax_cpu(void) { _mm_pause(); }

#endif
