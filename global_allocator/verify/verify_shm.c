// shm_host_test.c  (2 hosts, correctness loop using barrier API from barrier.h)
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "barrier.h"
#include "common.h"
#include "shm.h"

/* --- Config --- */

#define HOST_NUM 2
#define MAP_SIZE (64UL * 1024 * 1024) /* 64 MiB; adjust if needed */

#define ID_A 0xA11CEULL
#define ID_BARRIER 0xBA771EULL /* separate shared barrier object */

#define SLICE_CACHELINES 16
#define SLICE_BYTES ((size_t)SLICE_CACHELINES * (size_t)CACHE_LINE_SIZE)

/* --- Layout helpers --- */

/* total bytes for all hosts' slices */
static inline size_t obj_total_data_bytes(uint32_t host_num) {
  return (size_t)host_num * SLICE_BYTES;
}

/* total object size (no embedded flags anymore; barrier is separate) */
static inline size_t obj_size_bytes(uint32_t host_num) {
  return obj_total_data_bytes(host_num);
}

/* pointer to the beginning of a host's slice */
static inline char *slice_base(void *base, uint32_t which_host) {
  char *p = (char *)base;
  size_t off = (size_t)which_host * SLICE_BYTES;
  return p + off;
}

/* --- Test helpers --- */

static void write_slice_pattern(void *base, uint32_t host_id, uint32_t host_num,
                                uint32_t round_idx, uint64_t tag) {
  (void)host_num;
  char *slice = slice_base(base, host_id);

  for (uint32_t cl = 0; cl < SLICE_CACHELINES; ++cl) {
    volatile uint64_t *w =
        (volatile uint64_t *)(slice + (size_t)cl * CACHE_LINE_SIZE);
    uint64_t v = (tag << 56) ^ ((uint64_t)host_id << 32) ^
                 ((uint64_t)round_idx << 8) ^ (uint64_t)cl;
    STORE_CACHELINE(w, v);
  }
}

static void verify_all_slices(void *base, uint32_t host_id, uint32_t host_num,
                              uint32_t round_idx, uint64_t tag) {
  for (uint32_t other = 0; other < host_num; ++other) {
    char *slice = slice_base(base, other);
    for (uint32_t cl = 0; cl < SLICE_CACHELINES; ++cl) {
      volatile uint64_t *w =
          (volatile uint64_t *)(slice + (size_t)cl * CACHE_LINE_SIZE);
      uint64_t got = LOAD_CACHELINE(w);
      uint64_t exp = (tag << 56) ^ ((uint64_t)other << 32) ^
                     ((uint64_t)round_idx << 8) ^ (uint64_t)cl;
      if (got != exp) {
        fprintf(
            stderr,
            "[host %u] verify failed: other=%u cl=%u round=%u tag=0x%02" PRIx64
            " got=0x%016" PRIx64 " exp=0x%016" PRIx64 "\n",
            host_id, other, cl, round_idx, tag, got, exp);
        exit(2);
      }
    }
  }
}

static void *attach_retry(uint32_t host_id, uint64_t id, size_t size,
                          const char *what) {
  for (int i = 0; i < 200000; ++i) {
    void *p = shm_attach_id(id, size);
    if (p)
      return p;
    if (errno != ENOENT) {
      fprintf(stderr, "[host %u] shm_attach_id(%s) failed errno=%d (%s)\n",
              host_id, what, errno, strerror(errno));
      return NULL;
    }
    usleep(1000); /* 1ms */
  }
  fprintf(stderr, "[host %u] shm_attach_id(%s) still ENOENT after retries\n",
          host_id, what);
  return NULL;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr,
            "Usage: %s <host_id (0|1)> [rounds] [dax_dev]\n"
            "  dax_dev defaults to /dev/interleave_dax\n",
            argv[0]);
    return 1;
  }

  uint32_t host_id = (uint32_t)atoi(argv[1]);
  if (host_id >= HOST_NUM) {
    fprintf(stderr, "host_id must be 0..%u\n", HOST_NUM - 1);
    return 1;
  }

  uint32_t rounds = 100;
  if (argc >= 3) {
    rounds = (uint32_t)atoi(argv[2]);
    if (rounds == 0)
      rounds = 1;
  }

  const char *dax_dev =
      (argc >= 4) ? argv[3] : "/dev/interleave_dax";

  struct shm_arg arg = {
      .host_id = host_id,
      .host_num = HOST_NUM,
      .map_size = MAP_SIZE,
  };
  snprintf(arg.dax_dev, DEV_NAME_LENGTH, "%s", dax_dev);

  int32_t rc = shm_enable(arg);
  if (rc != 0) {
    fprintf(stderr, "[host %u] shm_enable failed: %d\n", host_id, rc);
    return 10;
  }

  /* --- Create/attach barrier object --- */
  shm_barrier_t *bar = NULL;
  size_t bar_sz = sizeof(*bar);

  if (host_id == 0) {
    bar = (shm_barrier_t *)shm_malloc_id(ID_BARRIER, bar_sz);
    if (!bar && errno == EEXIST) {
      fprintf(stderr, "[host 0] barrier exists, attaching\n");
      bar = (shm_barrier_t *)shm_attach_id(ID_BARRIER, bar_sz);
    }
    if (!bar) {
      fprintf(stderr,
              "[host 0] barrier shm_malloc/attach failed errno=%d (%s)\n",
              errno, strerror(errno));
      return 11;
    }

    /* Optional: clean slate each run (recommended for correctness tests) */
    memset(bar, 0, bar_sz);
    flush_region(bar, bar_sz);
  } else {
    bar = (shm_barrier_t *)attach_retry(host_id, ID_BARRIER, bar_sz, "BARRIER");
    if (!bar)
      return 12;
  }

  /* Each participant must call barrier_init once */
  rc = barrier_init(bar, HOST_NUM, host_id);
  if (rc != 0) {
    fprintf(stderr, "[host %u] barrier_init failed rc=%d\n", host_id, rc);
    return 13;
  }

  /* --- Create/attach data object A --- */
  size_t sizeA = obj_size_bytes(HOST_NUM);
  void *base = NULL;

  if (host_id == 0) {
    base = shm_malloc_id(ID_A, sizeA);
    if (!base && errno == EEXIST) {
      fprintf(stderr, "[host 0] ID_A exists, attaching\n");
      base = shm_attach_id(ID_A, sizeA);
    }
    if (!base) {
      fprintf(stderr, "[host 0] shm_malloc_id/attach(A) failed errno=%d (%s)\n",
              errno, strerror(errno));
      return 14;
    }

    /* Reset object for this run */
    memset(base, 0, sizeA);
    flush_region(base, sizeA);
  } else {
    base = attach_retry(host_id, ID_A, sizeA, "A");
    if (!base)
      return 15;
  }

  printf("[host %u] base=%p size=%zu rounds=%u\n", host_id, base, sizeA,
         rounds);
  fflush(stdout);

  /* Init barrier: ensure host0's zeroing is visible before starting rounds */
  barrier_wait(bar, HOST_NUM, host_id);

  /* --- Rounds: write -> barrier -> verify -> barrier --- */
  for (uint32_t r = 0; r < rounds; ++r) {
    write_slice_pattern(base, host_id, HOST_NUM, r, 0xA1);

    /* everyone finished writing round r */
    barrier_wait(bar, HOST_NUM, host_id);

    verify_all_slices(base, host_id, HOST_NUM, r, 0xA1);

    /* everyone finished verifying round r */
    barrier_wait(bar, HOST_NUM, host_id);

    if ((r % 10) == 0 || r == rounds - 1) {
      printf("[host %u] round %u/%u OK\n", host_id, r + 1, rounds);
      fflush(stdout);
    }
  }

  printf("[host %u] all %u rounds passed.\n", host_id, rounds);
  fflush(stdout);
  return 0;
}
