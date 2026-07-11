// shm_ht.h - chained hash table in non-coherent multi-host CXL shared memory
//
// Layout: three segments allocated with shm_malloc_id() by the initializing
// host (host 0) and shm_attach_id()'ed by everyone else:
//
//   SHM_HT_ID_ROOT     shm_ht_root_t metadata; magic is published last so
//                      attachers can safely poll it.
//   SHM_HT_ID_BUCKETS  nbuckets x ht_bucket_t. Each bucket has its own
//                      cross-host LFM lock (shm_mutex_t, every field already
//                      cacheline padded) and a chain-head offset that lives
//                      in its own cacheline.
//   SHM_HT_ID_SLABS    hosts x slab_bytes node slabs. Each host bump-
//                      allocates chain nodes ONLY from its own slab, so
//                      there is no cross-host free-list/allocator contention.
//                      Node references are byte offsets into this region
//                      (0 == NULL; the first node slot of every slab is
//                      reserved so offset 0 is never a valid node).
//
// Visibility protocol on non-coherent memory (all under the bucket lock):
//   LOOKUP  clflushopt+lfence the bucket head line and each chain node line
//           as it is traversed (and the value line on a key match), then
//           read.
//   INSERT  write the node, clwb+sfence it, THEN publish the new head
//           pointer and clwb+sfence the head line (pointer published last).
//   UPDATE  overwrite the 64B value line in place, clwb+sfence.
//
// Intra-host: the LFM lock is a HOST-level lock (one participant id per
// host), so threads of the same host are serialized per bucket through a
// striped array of process-local pthread mutexes taken before the LFM lock.
//
// Keys are uint64, values a fixed 64-byte payload (exactly one cacheline).
// Single-host "dram" runs use the very same code path (locks + flushes);
// only the memory substrate changes.

#ifndef SHM_HT_H
#define SHM_HT_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "shm_mutex.h"

/* shm_malloc_id/shm_attach_id ids ("HT" = 0x4854 prefix) */
#define SHM_HT_ID_ROOT 0x48540001ULL
#define SHM_HT_ID_BUCKETS 0x48540002ULL
#define SHM_HT_ID_SLABS 0x48540003ULL

#define SHM_HT_MAGIC 0x53484d5f48545631ULL /* "SHM_HTV1" */
#define SHM_HT_VALUE_SIZE 64
#define SHM_HT_STRIPES 4096 /* intra-host lock stripes (power of two) */
#define SHM_HT_NULL_OFF 0ULL

typedef struct {
  cacheline_u64 magic; /* SHM_HT_MAGIC, published last */
  cacheline_u64 nbuckets;
  cacheline_u64 hosts;
  cacheline_u64 slab_bytes; /* per-host slab size */
  cacheline_u64 value_size; /* always SHM_HT_VALUE_SIZE */
} shm_ht_root_t;

typedef struct {
  shm_mutex_t lock;       /* cross-host per-bucket lock (LFM) */
  cacheline_u64 head_off; /* chain head offset into slab region, 0 = empty */
} ht_bucket_t;

typedef struct {
  uint64_t key;
  uint64_t next_off; /* offset into slab region, 0 = end of chain */
  uint8_t _pad[CACHE_LINE_SIZE - 2 * sizeof(uint64_t)];
  uint8_t value[SHM_HT_VALUE_SIZE]; /* second cacheline */
} ht_node_t;

_Static_assert(sizeof(ht_node_t) == 2 * CACHE_LINE_SIZE,
               "ht_node_t must be exactly two cachelines");
_Static_assert(offsetof(ht_node_t, value) == CACHE_LINE_SIZE,
               "value payload must start on its own cacheline");
_Static_assert(sizeof(ht_bucket_t) % CACHE_LINE_SIZE == 0,
               "ht_bucket_t must be cacheline granular");

typedef struct {
  shm_ht_root_t *root;
  ht_bucket_t *buckets;
  uint8_t *slabs; /* base of the whole slab region (all hosts) */
  uint32_t nbuckets;
  uint32_t hosts;
  uint32_t host_id;
  uint32_t _pad0;
  uint64_t slab_bytes;      /* per-host */
  uint64_t slab_used;       /* local bump offset into OWN slab (atomic) */
  pthread_mutex_t *stripes; /* process-local, SHM_HT_STRIPES entries */
} shm_ht_t;

/*
 * shm_ht_create(): host 0 creates + initializes the shared table.
 * Requires shm_enable() to have succeeded. nbuckets must be a power of two.
 * Returns 0 or -errno.
 */
int shm_ht_create(shm_ht_t *ht, uint32_t nbuckets, uint32_t hosts,
                  uint32_t host_id, uint64_t slab_bytes_per_host);

/*
 * shm_ht_attach(): non-initializing hosts attach to an existing table.
 * Polls for the root segment + magic with a bounded deadline (timeout_ms);
 * a dead initializer produces -ETIMEDOUT, never a hang.
 * Returns 0 or -errno.
 */
int shm_ht_attach(shm_ht_t *ht, uint32_t hosts, uint32_t host_id,
                  uint64_t timeout_ms);

/*
 * shm_ht_put(): insert or update key with a 64-byte value.
 * Returns 1 = inserted new node, 0 = updated in place,
 * -ENOSPC = own slab exhausted, other -errno on failure.
 */
int shm_ht_put(shm_ht_t *ht, uint64_t key, const void *value);

/*
 * shm_ht_get(): lookup key, copy 64-byte value into value_out.
 * Returns 0 = found, -ENOENT = missing.
 */
int shm_ht_get(shm_ht_t *ht, uint64_t key, void *value_out);

/* Bytes bump-allocated so far from this host's slab. */
uint64_t shm_ht_slab_used(const shm_ht_t *ht);

/* Free process-local state (stripe mutexes). Shared segments stay. */
void shm_ht_close(shm_ht_t *ht);

/* splitmix64 - same mixer the allocator uses for its id map */
static inline uint64_t shm_ht_hash(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

#endif /* SHM_HT_H */
