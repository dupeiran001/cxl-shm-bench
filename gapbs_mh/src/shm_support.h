#ifndef GAPBS_MH_SHM_SUPPORT_H
#define GAPBS_MH_SHM_SUPPORT_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>

// Use Real Global Allocator (Non-Coherent CXL)
extern "C" {
#include "../../global_allocator/barrier.h"
#include "../../global_allocator/common.h"
#include "../../global_allocator/cxl_atomic.h"
#include "../../global_allocator/shm.h"
#include "../../global_allocator/shm_mutex.h"
}

namespace gapbs_mh {

struct MhOptions {
  uint32_t hosts = 2;
  uint32_t host_id = 0;
  size_t shm_mb = 1024;
  char dev_path[64]; // Match DEV_NAME_LENGTH
  int alpha = 50;
  int beta = 24;
  bool td_only = false;
  bool bu_only = false;
  uint32_t sync_k = 1;
};

struct ArgvPack {
  MhOptions opts;
  std::vector<char *> args;
};

inline bool ParseUint32(const char *s, uint32_t *out) {
  if (!s || !*s)
    return false;
  char *end = nullptr;
  unsigned long val = strtoul(s, &end, 10);
  if (!end || *end != '\0')
    return false;
  *out = static_cast<uint32_t>(val);
  return true;
}

inline bool ParseSizeT(const char *s, size_t *out) {
  if (!s || !*s)
    return false;
  char *end = nullptr;
  unsigned long long val = strtoull(s, &end, 10);
  if (!end || *end != '\0')
    return false;
  *out = static_cast<size_t>(val);
  return true;
}

inline ArgvPack ParseMhOptions(int argc, char **argv) {
  ArgvPack pack;
  snprintf(pack.opts.dev_path, sizeof(pack.opts.dev_path), "/dev/dax0.0");

  pack.args.reserve(argc);
  if (argc > 0)
    pack.args.push_back(argv[0]);

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];

    if (strncmp(arg, "--hosts=", 8) == 0) {
      ParseUint32(arg + 8, &pack.opts.hosts);
      continue;
    }
    if (strncmp(arg, "--host_id=", 10) == 0) {
      ParseUint32(arg + 10, &pack.opts.host_id);
      continue;
    }
    if (strncmp(arg, "--shm-mb=", 9) == 0) {
      ParseSizeT(arg + 9, &pack.opts.shm_mb);
      continue;
    }
    if (strncmp(arg, "--dev=", 6) == 0) {
      strncpy(pack.opts.dev_path, arg + 6, sizeof(pack.opts.dev_path) - 1);
      pack.opts.dev_path[sizeof(pack.opts.dev_path) - 1] = '\0';
      continue;
    }
    if (strncmp(arg, "--alpha=", 8) == 0) {
      uint32_t alpha = 0;
      if (!ParseUint32(arg + 8, &alpha) || alpha == 0) {
        fprintf(stderr, "invalid --alpha value: %s\n", arg + 8);
        exit(1);
      }
      pack.opts.alpha = static_cast<int>(alpha);
      continue;
    }
    if (strcmp(arg, "--alpha") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "--alpha requires a value\n");
        exit(1);
      }
      uint32_t alpha = 0;
      if (!ParseUint32(argv[i + 1], &alpha) || alpha == 0) {
        fprintf(stderr, "invalid --alpha value: %s\n", argv[i + 1]);
        exit(1);
      }
      pack.opts.alpha = static_cast<int>(alpha);
      ++i;
      continue;
    }
    if (strncmp(arg, "--beta=", 7) == 0) {
      uint32_t beta = 0;
      if (!ParseUint32(arg + 7, &beta) || beta == 0) {
        fprintf(stderr, "invalid --beta value: %s\n", arg + 7);
        exit(1);
      }
      pack.opts.beta = static_cast<int>(beta);
      continue;
    }
    if (strcmp(arg, "--td-only") == 0) {
      pack.opts.td_only = true;
      continue;
    }
    if (strcmp(arg, "--bu-only") == 0) {
      pack.opts.bu_only = true;
      continue;
    }
    if (strncmp(arg, "--sync-k=", 9) == 0) {
      uint32_t sync_k = 0;
      if (!ParseUint32(arg + 9, &sync_k) || sync_k == 0) {
        fprintf(stderr, "invalid --sync-k value: %s\n", arg + 9);
        exit(1);
      }
      pack.opts.sync_k = sync_k;
      continue;
    }
    if (strcmp(arg, "--sync-k") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "--sync-k requires a value\n");
        exit(1);
      }
      uint32_t sync_k = 0;
      if (!ParseUint32(argv[i + 1], &sync_k) || sync_k == 0) {
        fprintf(stderr, "invalid --sync-k value: %s\n", argv[i + 1]);
        exit(1);
      }
      pack.opts.sync_k = sync_k;
      ++i;
      continue;
    }
    if (strcmp(arg, "--beta") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "--beta requires a value\n");
        exit(1);
      }
      uint32_t beta = 0;
      if (!ParseUint32(argv[i + 1], &beta) || beta == 0) {
        fprintf(stderr, "invalid --beta value: %s\n", argv[i + 1]);
        exit(1);
      }
      pack.opts.beta = static_cast<int>(beta);
      ++i;
      continue;
    }
    pack.args.push_back(argv[i]);
  }
  return pack;
}

inline void *ShmAttachRetry(uint64_t id, size_t size, const char *label) {
  for (int attempt = 0; attempt < 2000; ++attempt) {
    void *ptr = shm_attach_id(id, size);
    if (ptr)
      return ptr;
    if (errno != ENOENT && errno != EINVAL)
      break;
    usleep(1000);
  }
  if (label)
    fprintf(stderr, "shm_attach_id(%s) failed: %s\n", label, strerror(errno));
  return nullptr;
}

// One per-host value on its own cache line. Hosts are not coherent with each
// other, so two hosts must never write different words of one line: whichever
// write-back lands last also carries its stale copy of the other host's word.
template <typename T> struct alignas(CACHE_LINE_SIZE) HostSlot {
  T v;
};
static_assert(sizeof(HostSlot<double>) == CACHE_LINE_SIZE,
              "HostSlot must fill exactly one cache line");
static_assert(sizeof(HostSlot<uint64_t>) == CACHE_LINE_SIZE,
              "HostSlot must fill exactly one cache line");

// Contiguous partition of [0, n) over hosts with every boundary on a cache
// line multiple of T, so no line of a line-aligned T array is written by two
// hosts. All hosts and all phases that write such an array (reset, compute,
// publish) must use the same partition. Hosts can get an empty range when
// n is small.
template <typename T>
inline void LinePartition(int64_t n, uint32_t host_id, uint32_t hosts,
                          int64_t *begin, int64_t *end) {
  static_assert(CACHE_LINE_SIZE % sizeof(T) == 0,
                "element size must divide the cache line");
  const int64_t per_line = CACHE_LINE_SIZE / sizeof(T);
  const int64_t lines = (n + per_line - 1) / per_line;
  auto at = [&](uint32_t h) {
    int64_t v = lines * static_cast<int64_t>(h) / hosts * per_line;
    return v < n ? v : n;
  };
  *begin = at(host_id);
  *end = at(host_id + 1);
}

inline bool LineAligned(const void *p) {
  return (reinterpret_cast<uintptr_t>(p) & (CACHE_LINE_SIZE - 1)) == 0;
}

// Wrapper for barrier - need to store parties inside the wrapper scope
using ShmBarrier = shm_barrier_t;

// NOTE: ShmBarrierInit is called once by each participant, passing their my_id.
// Host 0 builds the barrier; the others wait (bounded) for it. Returns 0 or
// the negative errno from barrier_init(), which callers must treat as fatal.
inline int ShmBarrierInit(ShmBarrier *b, uint32_t total, uint32_t my_id) {
  int rc = barrier_init(b, total, my_id);
  if (rc != 0)
    fprintf(stderr, "[Host %u] barrier_init failed: %s\n", my_id,
            strerror(-rc));
  return rc;
}

inline void ShmBarrierWait(ShmBarrier *b, uint32_t parties, uint32_t host_id) {
  if (parties <= 1)
    return;
  barrier_wait(b, parties, host_id);
}

} // namespace gapbs_mh

#endif
