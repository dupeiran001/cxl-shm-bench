// shm_ht.c - chained hash table on non-coherent multi-host CXL shared memory
#include "shm_ht.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "shm.h"

/* ---- flush helpers (visibility protocol) ---------------------------------
 *
 * ht_inval(): clflushopt every line of [p, p+len) then mfence, so the next
 * reads miss the local cache and fetch whatever the last writer pushed to
 * the (non-coherent) shared media. Used on the LOOKUP path and before
 * reading chain metadata on the PUT path. lfence is not enough: it does not
 * wait for the flushes to complete, so a later load can still be served from
 * the line being flushed.
 *
 * ht_wb(): clwb (fallback clflushopt/clflush) every line then sfence, so
 * prior stores reach the media before anything published after them.
 * Used on the INSERT/UPDATE path; the chain head pointer is always written
 * back last so a node is fully visible before it is reachable.
 */

static inline void ht_wb_line(void *p) {
#if defined(__CLWB__)
  _mm_clwb(p);
#elif defined(__CLFLUSHOPT__)
  _mm_clflushopt(p);
#else
  _mm_clflush(p);
#endif
}

static inline void ht_inval(const void *p, size_t len) {
  uintptr_t start = (uintptr_t)p & ~(uintptr_t)(CACHE_LINE_SIZE - 1);
  uintptr_t end = (uintptr_t)p + len;
  compiler_barrier();
  for (uintptr_t a = start; a < end; a += CACHE_LINE_SIZE) {
    flush_line((void *)a); /* clflushopt (clflush fallback) */
  }
  _mm_mfence();
  compiler_barrier();
}

static inline void ht_wb(void *p, size_t len) {
  uintptr_t start = (uintptr_t)p & ~(uintptr_t)(CACHE_LINE_SIZE - 1);
  uintptr_t end = (uintptr_t)p + len;
  compiler_barrier();
  for (uintptr_t a = start; a < end; a += CACHE_LINE_SIZE) {
    ht_wb_line((void *)a);
  }
  _mm_sfence();
  compiler_barrier();
}

/* ---- internal helpers ---------------------------------------------------*/

static inline uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static inline ht_node_t *node_at(const shm_ht_t *ht, uint64_t off) {
  return (ht_node_t *)(ht->slabs + off);
}

static inline ht_bucket_t *bucket_for(const shm_ht_t *ht, uint64_t hash) {
  return &ht->buckets[hash & (ht->nbuckets - 1)];
}

static inline pthread_mutex_t *stripe_for(shm_ht_t *ht, uint64_t hash) {
  return &ht->stripes[(hash & (ht->nbuckets - 1)) & (SHM_HT_STRIPES - 1)];
}

static int local_init(shm_ht_t *ht) {
  ht->stripes = calloc(SHM_HT_STRIPES, sizeof(pthread_mutex_t));
  if (!ht->stripes) {
    return -ENOMEM;
  }
  for (size_t i = 0; i < SHM_HT_STRIPES; i++) {
    pthread_mutex_init(&ht->stripes[i], NULL);
  }
  /* Reserve node slot 0 of the own slab so offset 0 stays the NULL
   * sentinel (host 0's first node would otherwise land at offset 0). */
  ht->slab_used = sizeof(ht_node_t);
  return 0;
}

/* ---- create / attach ----------------------------------------------------*/

int shm_ht_create(shm_ht_t *ht, uint32_t nbuckets, uint32_t hosts,
                  uint32_t host_id, uint64_t slab_bytes_per_host) {
  if (!ht || nbuckets < 2 || (nbuckets & (nbuckets - 1)) != 0 || hosts == 0 ||
      hosts > MAX_HOST_NUM || host_id >= hosts ||
      slab_bytes_per_host < 4 * sizeof(ht_node_t)) {
    return -EINVAL;
  }
  memset(ht, 0, sizeof(*ht));
  ht->nbuckets = nbuckets;
  ht->hosts = hosts;
  ht->host_id = host_id;
  ht->slab_bytes = slab_bytes_per_host;

  shm_ht_root_t *root = shm_malloc_id(SHM_HT_ID_ROOT, sizeof(shm_ht_root_t));
  if (!root) {
    fprintf(stderr, "shm_ht: alloc root failed: %s (increase --shm-mb?)\n",
            strerror(errno));
    return -errno;
  }
  /* Segment content may be stale from an earlier run: kill the magic before
   * anything else so attachers can't see a half-built table. */
  CACHELINE_STORE(&root->magic, 0);

  size_t bucket_bytes = (size_t)nbuckets * sizeof(ht_bucket_t);
  ht_bucket_t *buckets = shm_malloc_id(SHM_HT_ID_BUCKETS, bucket_bytes);
  if (!buckets) {
    fprintf(stderr, "shm_ht: alloc buckets (%zu MB) failed: %s "
                    "(increase --shm-mb?)\n",
            bucket_bytes >> 20, strerror(errno));
    return -errno;
  }

  size_t slab_total = (size_t)hosts * slab_bytes_per_host;
  uint8_t *slabs = shm_malloc_id(SHM_HT_ID_SLABS, slab_total);
  if (!slabs) {
    fprintf(stderr, "shm_ht: alloc slabs (%zu MB) failed: %s "
                    "(increase --shm-mb?)\n",
            slab_total >> 20, strerror(errno));
    return -errno;
  }

  for (uint32_t i = 0; i < nbuckets; i++) {
    shm_mutex_init(&buckets[i].lock);
    CACHELINE_STORE(&buckets[i].head_off, SHM_HT_NULL_OFF);
  }

  CACHELINE_STORE(&root->nbuckets, nbuckets);
  CACHELINE_STORE(&root->hosts, hosts);
  CACHELINE_STORE(&root->slab_bytes, slab_bytes_per_host);
  CACHELINE_STORE(&root->value_size, SHM_HT_VALUE_SIZE);
  /* Publish magic last. */
  CACHELINE_STORE(&root->magic, SHM_HT_MAGIC);

  ht->root = root;
  ht->buckets = buckets;
  ht->slabs = slabs;
  return local_init(ht);
}

/* Bounded attach helper: retries shm_attach_id until deadline_ms. */
static void *attach_retry(uint64_t id, size_t size, uint64_t deadline_ms,
                          const char *label) {
  for (;;) {
    void *p = shm_attach_id(id, size);
    if (p) {
      return p;
    }
    if (errno != ENOENT && errno != EINVAL) {
      fprintf(stderr, "shm_ht: attach %s failed hard: %s\n", label,
              strerror(errno));
      return NULL;
    }
    if (now_ms() >= deadline_ms) {
      fprintf(stderr, "shm_ht: attach %s timed out (initializer dead?)\n",
              label);
      errno = ETIMEDOUT;
      return NULL;
    }
    usleep(1000);
  }
}

int shm_ht_attach(shm_ht_t *ht, uint32_t hosts, uint32_t host_id,
                  uint64_t timeout_ms) {
  if (!ht || hosts == 0 || hosts > MAX_HOST_NUM || host_id >= hosts) {
    return -EINVAL;
  }
  memset(ht, 0, sizeof(*ht));
  ht->hosts = hosts;
  ht->host_id = host_id;

  uint64_t deadline = now_ms() + timeout_ms;

  shm_ht_root_t *root =
      attach_retry(SHM_HT_ID_ROOT, sizeof(shm_ht_root_t), deadline, "root");
  if (!root) {
    return -errno;
  }
  /* Bounded wait for the initializer to publish the magic. */
  while (CACHELINE_LOAD(&root->magic) != SHM_HT_MAGIC) {
    if (now_ms() >= deadline) {
      fprintf(stderr, "shm_ht: root magic never published (timeout)\n");
      return -ETIMEDOUT;
    }
    usleep(1000);
  }

  uint64_t nbuckets = CACHELINE_LOAD(&root->nbuckets);
  uint64_t rhosts = CACHELINE_LOAD(&root->hosts);
  uint64_t slab_bytes = CACHELINE_LOAD(&root->slab_bytes);
  uint64_t vsize = CACHELINE_LOAD(&root->value_size);
  if (rhosts != hosts || vsize != SHM_HT_VALUE_SIZE || nbuckets < 2 ||
      (nbuckets & (nbuckets - 1)) != 0) {
    fprintf(stderr,
            "shm_ht: root mismatch (hosts %lu/%u vsize %lu nbuckets %lu)\n",
            rhosts, hosts, vsize, nbuckets);
    return -EINVAL;
  }
  ht->nbuckets = (uint32_t)nbuckets;
  ht->slab_bytes = slab_bytes;

  size_t bucket_bytes = (size_t)nbuckets * sizeof(ht_bucket_t);
  ht_bucket_t *buckets =
      attach_retry(SHM_HT_ID_BUCKETS, bucket_bytes, deadline, "buckets");
  if (!buckets) {
    return -errno;
  }
  size_t slab_total = (size_t)hosts * slab_bytes;
  uint8_t *slabs = attach_retry(SHM_HT_ID_SLABS, slab_total, deadline, "slabs");
  if (!slabs) {
    return -errno;
  }

  ht->root = root;
  ht->buckets = buckets;
  ht->slabs = slabs;
  return local_init(ht);
}

/* ---- operations ----------------------------------------------------------
 * Chain walks are bounded by construction (chains are finite, cycles are
 * impossible: nodes are only ever prepended with next = current head).
 */

int shm_ht_put(shm_ht_t *ht, uint64_t key, const void *value) {
  if (!ht || !ht->buckets || !value) {
    return -EINVAL;
  }
  uint64_t h = shm_ht_hash(key);
  ht_bucket_t *b = bucket_for(ht, h);
  pthread_mutex_t *stripe = stripe_for(ht, h);
  int rc;

  pthread_mutex_lock(stripe);
  if (shm_mutex_lock(&b->lock, (int)ht->host_id, (int)ht->hosts) ==
      UINT64_MAX) {
    pthread_mutex_unlock(stripe);
    return -EINVAL;
  }

  /* Invalidate + read the chain head. */
  ht_inval((const void *)&b->head_off, CACHE_LINE_SIZE);
  uint64_t head = b->head_off.value;

  /* Walk the chain looking for the key (invalidate metadata line only;
   * on update the whole value line is overwritten anyway). */
  ht_node_t *found = NULL;
  for (uint64_t off = head; off != SHM_HT_NULL_OFF;) {
    ht_node_t *n = node_at(ht, off);
    ht_inval(n, CACHE_LINE_SIZE);
    if (n->key == key) {
      found = n;
      break;
    }
    off = n->next_off;
  }

  if (found) {
    /* UPDATE: overwrite value line in place, write back + fence. */
    memcpy(found->value, value, SHM_HT_VALUE_SIZE);
    ht_wb(found->value, SHM_HT_VALUE_SIZE);
    rc = 0;
  } else {
    /* INSERT: bump-allocate from OWN slab (no cross-host contention). */
    uint64_t local_off =
        __atomic_fetch_add(&ht->slab_used, sizeof(ht_node_t),
                           __ATOMIC_RELAXED);
    if (local_off + sizeof(ht_node_t) > ht->slab_bytes) {
      rc = -ENOSPC;
    } else {
      uint64_t goff = (uint64_t)ht->host_id * ht->slab_bytes + local_off;
      ht_node_t *n = node_at(ht, goff);
      n->key = key;
      n->next_off = head;
      memcpy(n->value, value, SHM_HT_VALUE_SIZE);
      /* Node contents reach media first ... */
      ht_wb(n, sizeof(ht_node_t));
      /* ... then the head pointer is published. */
      b->head_off.value = goff;
      ht_wb((void *)&b->head_off, CACHE_LINE_SIZE);
      rc = 1;
    }
  }

  shm_mutex_unlock(&b->lock, (int)ht->host_id);
  pthread_mutex_unlock(stripe);
  return rc;
}

int shm_ht_get(shm_ht_t *ht, uint64_t key, void *value_out) {
  if (!ht || !ht->buckets || !value_out) {
    return -EINVAL;
  }
  uint64_t h = shm_ht_hash(key);
  ht_bucket_t *b = bucket_for(ht, h);
  pthread_mutex_t *stripe = stripe_for(ht, h);
  int rc = -ENOENT;

  pthread_mutex_lock(stripe);
  if (shm_mutex_lock(&b->lock, (int)ht->host_id, (int)ht->hosts) ==
      UINT64_MAX) {
    pthread_mutex_unlock(stripe);
    return -EINVAL;
  }

  ht_inval((const void *)&b->head_off, CACHE_LINE_SIZE);
  for (uint64_t off = b->head_off.value; off != SHM_HT_NULL_OFF;) {
    ht_node_t *n = node_at(ht, off);
    ht_inval(n, CACHE_LINE_SIZE); /* metadata line */
    if (n->key == key) {
      ht_inval(n->value, SHM_HT_VALUE_SIZE); /* value line */
      memcpy(value_out, n->value, SHM_HT_VALUE_SIZE);
      rc = 0;
      break;
    }
    off = n->next_off;
  }

  shm_mutex_unlock(&b->lock, (int)ht->host_id);
  pthread_mutex_unlock(stripe);
  return rc;
}

uint64_t shm_ht_slab_used(const shm_ht_t *ht) {
  return __atomic_load_n(&((shm_ht_t *)ht)->slab_used, __ATOMIC_RELAXED);
}

void shm_ht_close(shm_ht_t *ht) {
  if (!ht) {
    return;
  }
  if (ht->stripes) {
    for (size_t i = 0; i < SHM_HT_STRIPES; i++) {
      pthread_mutex_destroy(&ht->stripes[i]);
    }
    free(ht->stripes);
    ht->stripes = NULL;
  }
}
