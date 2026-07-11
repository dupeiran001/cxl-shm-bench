#include <criterion/criterion.h>
#include <criterion/logging.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "shm.h"

/* Two IDs, allocated by different "hosts" to force interleaving. */
#define ID_A 0xA11CEULL
#define ID_B 0xB22CEULL

/* Each host gets a slice of this many cachelines within each object. */
#define SLICE_CACHELINES 16
#define SLICE_BYTES ((size_t)SLICE_CACHELINES * (size_t)CACHE_LINE_SIZE)

/* Ready flags live after all slices; one cacheline per host. */
static inline size_t obj_total_data_bytes(uint32_t host_num) {
  return (size_t)host_num * SLICE_BYTES;
}
static inline size_t obj_flags_bytes(uint32_t host_num) {
  return (size_t)host_num * (size_t)CACHE_LINE_SIZE;
}
static inline size_t obj_size_bytes(uint32_t host_num) {
  /* data region + flags region */
  return obj_total_data_bytes(host_num) + obj_flags_bytes(host_num);
}

static inline volatile uint64_t *flag_ptr(void *base, uint32_t host_num,
                                          uint32_t which_host) {
  char *p = (char *)base;
  size_t off =
      obj_total_data_bytes(host_num) + (size_t)which_host * CACHE_LINE_SIZE;
  return (volatile uint64_t *)(p + off);
}

static inline volatile uint64_t *
slice_word_ptr(void *base, uint32_t which_host,
               uint32_t word_idx /* in u64 */) {
  /* word_idx chooses which u64 inside the slice (still cacheline-aligned if
   * word_idx==0) */
  char *p = (char *)base;
  size_t off =
      (size_t)which_host * SLICE_BYTES + (size_t)word_idx * sizeof(uint64_t);
  return (volatile uint64_t *)(p + off);
}

static void write_slice_pattern(void *base, uint32_t host_id, uint32_t host_num,
                                uint32_t round, uint64_t tag) {
  (void)host_num;
  /*
   * Write one u64 at the beginning of each cacheline in the slice.
   * That ensures we touch multiple cachelines and avoid false sharing.
   */
  for (uint32_t cl = 0; cl < SLICE_CACHELINES; ++cl) {
    volatile uint64_t *w =
        (volatile uint64_t *)((char *)base + (size_t)host_id * SLICE_BYTES +
                              (size_t)cl * CACHE_LINE_SIZE);
    uint64_t v = (tag << 56) ^ ((uint64_t)host_id << 32) ^ (uint64_t)round ^
                 (uint64_t)cl;
    STORE_CACHELINE(w, v);
  }
}

static void verify_all_slices(void *base, uint32_t host_id, uint32_t host_num,
                              uint32_t round, uint64_t tag) {
  for (uint32_t other = 0; other < host_num; ++other) {
    for (uint32_t cl = 0; cl < SLICE_CACHELINES; ++cl) {
      volatile uint64_t *w =
          (volatile uint64_t *)((char *)base + (size_t)other * SLICE_BYTES +
                                (size_t)cl * CACHE_LINE_SIZE);
      uint64_t got = LOAD_CACHELINE(w);
      uint64_t exp = (tag << 56) ^ ((uint64_t)other << 32) ^ (uint64_t)round ^
                     (uint64_t)cl;
      if (got != exp) {
        fprintf(
            stderr,
            "[host %u] verify failed: other=%u cl=%u round=%u tag=0x%02" PRIx64
            " got=0x%016" PRIx64 " exp=0x%016" PRIx64 "\n",
            host_id, other, cl, round, tag, got, exp);
        _exit(2);
      }
    }
  }
}

static void busy_wait_all_ready(void *base, uint32_t host_num, uint64_t epoch) {
  for (;;) {
    bool all = true;
    for (uint32_t h = 0; h < host_num; ++h) {
      uint64_t v = LOAD_CACHELINE(flag_ptr(base, host_num, h));
      if (v != epoch) {
        all = false;
        break;
      }
    }
    if (all)
      return;
    relax_cpu();
  }
}

static void set_my_ready(void *base, uint32_t host_id, uint32_t host_num,
                         uint64_t epoch) {
  STORE_CACHELINE(flag_ptr(base, host_num, host_id), epoch);
}

/* Each child process executes this as a "host". */
static void run_host(uint32_t host_id, uint32_t host_num, const char *path,
                     size_t map_size, uint32_t rounds) {
  struct shm_arg arg = {
      .host_id = host_id,
      .host_num = host_num,
      .map_size = map_size,
  };
  snprintf(arg.dax_dev, DEV_NAME_LENGTH, "%s", path);

  int32_t rc = shm_enable(arg);
  if (rc != 0) {
    fprintf(stderr, "[host %u] shm_enable failed: %d\n", host_id, rc);
    _exit(10);
  }

  /* Two objects; allocated by different hosts to force interleaving. */
  size_t sizeA = obj_size_bytes(host_num);
  size_t sizeB =
      obj_size_bytes(host_num) + CACHE_LINE_SIZE; /* slightly different size */

  void *a = NULL;
  void *b = NULL;

  if (host_id == 0) {
    a = shm_malloc_id(ID_A, sizeA);
    if (!a) {
      fprintf(stderr, "[host %u] shm_malloc_id(A) failed errno=%d\n", host_id,
              errno);
      _exit(11);
    }
  } else {
    a = shm_attach_id(ID_A, sizeA);
    if (!a) {
      /* might race: A not created yet; retry a bit */
      for (int k = 0; k < 1000 && !a; ++k) {
        relax_cpu();
        a = shm_attach_id(ID_A, sizeA);
      }
      if (!a) {
        fprintf(stderr, "[host %u] shm_attach_id(A) failed errno=%d\n", host_id,
                errno);
        _exit(12);
      }
    }
  }

  if (host_id == 1) {
    b = shm_malloc_id(ID_B, sizeB);
    if (!b) {
      fprintf(stderr, "[host %u] shm_malloc_id(B) failed errno=%d\n", host_id,
              errno);
      _exit(13);
    }
  } else {
    b = shm_attach_id(ID_B, sizeB);
    if (!b) {
      for (int k = 0; k < 1000 && !b; ++k) {
        relax_cpu();
        b = shm_attach_id(ID_B, sizeB);
      }
      if (!b) {
        fprintf(stderr, "[host %u] shm_attach_id(B) failed errno=%d\n", host_id,
                errno);
        _exit(14);
      }
    }
  }

  /* Negative test: wrong-size attach should fail (your shm_attach_id enforces
   * exact match if size!=0). */
  if (host_id == 0) {
    errno = 0;
    void *bad = shm_attach_id(ID_A, sizeA + 1);
    if (bad != NULL || errno != EINVAL) {
      fprintf(stderr,
              "[host %u] expected wrong-size attach to fail (EINVAL), got "
              "bad=%p errno=%d\n",
              host_id, bad, errno);
      _exit(15);
    }
  }

  /*
   * Repeated rounds:
   * - write my slice in A, set ready, wait all ready, verify A
   * - write my slice in B, set ready, wait all ready, verify B
   *
   * Ready epochs are disjoint for A vs B to avoid interference.
   */
  for (uint32_t r = 0; r < rounds; ++r) {
    uint64_t epochA = ((uint64_t)r + 1ull) | 0xA000000000000000ull;
    uint64_t epochB = ((uint64_t)r + 1ull) | 0xB000000000000000ull;

    /* Object A */
    write_slice_pattern(a, host_id, host_num, r, 0xA1);
    set_my_ready(a, host_id, host_num, epochA);
    busy_wait_all_ready(a, host_num, epochA);
    verify_all_slices(a, host_id, host_num, r, 0xA1);

    /* Object B */
    write_slice_pattern(b, host_id, host_num, r, 0xB2);
    set_my_ready(b, host_id, host_num, epochB);
    busy_wait_all_ready(b, host_num, epochB);
    verify_all_slices(b, host_id, host_num, r, 0xB2);
  }

  _exit(0);
}

static void run_case(uint32_t host_num) {
  /* Backing file: use a temp file; IMPORTANT: size it to map_size to avoid
   * SIGBUS on normal FS. */
  char path[256];
  snprintf(path, sizeof(path), "/tmp/cxlshm_test_%u_%d.dat", host_num,
           (int)getpid());

  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0666);
  cr_assert(fd >= 0, "open(%s) failed: %s", path, strerror(errno));

  size_t map_size = 64ul * 1024 * 1024; /* 64 MiB */
  cr_assert(ftruncate(fd, (off_t)map_size) == 0, "ftruncate failed: %s",
            strerror(errno));
  close(fd);

  const uint32_t rounds = 200;

  pid_t pids[MAX_HOST_NUM] = {0};
  cr_assert(host_num <= MAX_HOST_NUM);

  for (uint32_t h = 0; h < host_num; ++h) {
    pid_t pid = fork();
    cr_assert(pid >= 0, "fork failed: %s", strerror(errno));
    if (pid == 0) {
      run_host(h, host_num, path, map_size, rounds);
      /* unreachable */
    }
    pids[h] = pid;
  }

  int failures = 0;
  for (uint32_t h = 0; h < host_num; ++h) {
    int st = 0;
    pid_t w = waitpid(pids[h], &st, 0);
    cr_assert(w == pids[h], "waitpid failed: %s", strerror(errno));
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
      ++failures;
      if (WIFEXITED(st)) {
        cr_log_error("host %u exited with code %d\n", h, WEXITSTATUS(st));
      } else if (WIFSIGNALED(st)) {
        cr_log_error("host %u killed by signal %d\n", h, WTERMSIG(st));
      } else {
        cr_log_error("host %u exited abnormally\n", h);
      }
    }
  }

  unlink(path);
  cr_assert(failures == 0, "multi-host case (%u hosts) had %d failures",
            host_num, failures);
}

Test(shm, two_hosts_malloc_attach_exchange) { run_case(2); }

Test(shm, four_hosts_malloc_attach_exchange) { run_case(4); }
