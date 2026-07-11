#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"
#include "shm.h"
#include "shm_mutex.h"

#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)

/*
 * Buddy allocator:
 * - Minimum block = 2MB (CHUNK_SIZE)
 * - Orders: block_size = CHUNK_SIZE << order
 * - Free lists per order (singly-linked via "next" stored in the free block)
 * - Per-order locks (shm_mutex_t) protect each free list head and list content
 *
 * Visibility rules:
 * - All metadata that must be visible cross-host uses CACHELINE_LOAD/STORE.
 * - Free-list "next" pointers inside blocks are also updated via
 * STORE_CACHELINE and read via LOAD_CACHELINE.
 */

#define CHUNK_SHIFT 21 // 2MB
#define CHUNK_SIZE (1UL << CHUNK_SHIFT)

#define MAX_IDS 1024
#define IDS_CAPACITY (2 * MAX_IDS) /* power-of-two: 2048 */

#define SHM_MAGIC 0x43584C53484D3031ULL /* "CXLSHM01" */
#define SHM_VALID(p) ((p) && CACHELINE_LOAD(&(p)->metadata.magic) == SHM_MAGIC)

enum {
  SLOT_FREE = 0,
  SLOT_OCCUPIED = 1,
  SLOT_TOMBSTONE = 2,
};

enum {
  ALLOC_NONE = 0,
  ALLOC_ALLOCATED = 1,
  ALLOC_ATTACHED = 2,
};

/*
 * We cap max order to something sane.
 * For a 512GB pool with 2MB min blocks:
 * 512GB / 2MB = 262144 blocks => max_order about 18 (2^18 * 2MB = 512GB)
 * So 0..20 is plenty.
 */
#define BUDDY_MAX_ORDER 20

typedef struct {
  cacheline_u64 slot_state; // FREE/OCCUPIED/TOMBSTONE
  uint64_t id;              // key
  uint64_t off;             // offset (from mapping base)
  uint64_t size;            // requested bytes
  cacheline_u32
      alloc_order; // buddy order used for this allocation (cacheline-visible)
  uint32_t _pad0;
  uint64_t alloc_state; // ALLOCATED/ATTACHED (debug/optional)
  uint8_t user_bitmap;  // host attach bitmap
  uint8_t _pad[7];
} shm_id_entry_t;

/* Free block header stored inside each free block (at its start). */
typedef struct {
  cacheline_u64 next_off; /* 0 => end of list; otherwise absolute offset */
} free_blk_hdr_t;

typedef struct {
  shm_mutex_t lock; // protects init/reset

  struct {
    cacheline_u64 magic;
    cacheline_u32 host_num;
    cacheline_size_t map_size;

    /* buddy pool metadata */
    cacheline_u64 first_chunk_off; /* start of allocator pool */
    cacheline_u64 pool_bytes;      /* bytes managed by buddy */
    cacheline_u32 max_order;       /* max order usable */
    cacheline_u32 _pad;
  } metadata;

  /* Buddy free-list heads (one per order), plus per-order locks. */
  struct {
    cacheline_u64
        free_head[BUDDY_MAX_ORDER + 1]; /* offset of first free block */
    shm_mutex_t free_lock[BUDDY_MAX_ORDER + 1];
  } buddy;

  shm_mutex_t id_map_lock;
  struct {
    shm_id_entry_t map[IDS_CAPACITY];
  } id_map;

  char chunks[]; /* placement for pool start */
} shared_area_t;

static struct shm_state {
  bool initialized;
  uint32_t host_id;
  uint32_t host_num;
  uint32_t flags;
  int fd;
  char dax_dev[DEV_NAME_LENGTH];
  size_t map_size;
  shared_area_t *shared_area;
  pid_t pid;
} shm_state;

/* IDS_CAPACITY is power of two (2048). */
static inline uint64_t id_hash64(uint64_t x) {
  /* splitmix64 */
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  x = x ^ (x >> 31);
  return x;
}

static inline bool shm_ready(void) {
  return shm_state.initialized && shm_state.shared_area &&
         SHM_VALID(shm_state.shared_area);
}

static inline void *off_to_ptr(shared_area_t *sa, uint64_t off) {
  return (void *)((char *)sa + off);
}

static inline uint64_t ptr_to_off(shared_area_t *sa, void *p) {
  return (uint64_t)((char *)p - (char *)sa);
}

/* ---- id map probing ---- */
static inline void idmap_load_entry(const shm_id_entry_t *src,
                                    shm_id_entry_t *dst) {
  flush_region((void *)src, sizeof(*dst));
  full_fence();
  memcpy(dst, src, sizeof(*dst));
}

/* find an empty slot, or reuse a tombstone*/
static int idmap_find_slot(shared_area_t *sa, uint64_t id, bool *found,
                           uint64_t *out_idx) {
  if (id == 0)
    return -EINVAL;

  uint64_t h = id_hash64(id);
  uint64_t mask = IDS_CAPACITY - 1;
  uint64_t start = h & mask;

  uint64_t first_tomb = UINT64_MAX;

  for (uint64_t step = 0; step < IDS_CAPACITY; ++step) {
    uint64_t i = (start + step) & mask;
    shm_id_entry_t *e = &sa->id_map.map[i];

    uint64_t st = CACHELINE_LOAD(&e->slot_state);

    if (st == SLOT_FREE) {
      *found = false;
      *out_idx = (first_tomb != UINT64_MAX) ? first_tomb : i;
      return 0;
    }

    if (st == SLOT_TOMBSTONE) {
      if (first_tomb == UINT64_MAX)
        first_tomb = i;
      continue;
    }

    /* OCCUPIED */
    shm_id_entry_t cur;
    idmap_load_entry(e, &cur);
    if (cur.id == id) {
      *found = true;
      *out_idx = i;
      return 0;
    }
  }

  /* Table contains no SLOT_FREE. If we saw a tombstone, reuse it. */
  if (first_tomb != UINT64_MAX) {
    *found = false;
    *out_idx = first_tomb;
    return 0;
  }

  return -ENOSPC;
}
/* ---- buddy helpers ---- */

static inline uint64_t buddy_first(shared_area_t *sa) {
  return CACHELINE_LOAD(&sa->metadata.first_chunk_off);
}
static inline uint64_t buddy_pool_bytes(shared_area_t *sa) {
  return CACHELINE_LOAD(&sa->metadata.pool_bytes);
}
static inline uint32_t buddy_max_order(shared_area_t *sa) {
  uint32_t mo = CACHELINE_LOAD(&sa->metadata.max_order);
  if (mo > BUDDY_MAX_ORDER)
    mo = BUDDY_MAX_ORDER;
  return mo;
}

static inline uint64_t order_size(uint32_t order) {
  return (uint64_t)CHUNK_SIZE << order;
}

static inline bool off_in_pool(shared_area_t *sa, uint64_t off) {
  uint64_t first = buddy_first(sa);
  uint64_t pool = buddy_pool_bytes(sa);
  return (off >= first) && (off < first + pool);
}

static inline bool off_valid(shared_area_t *sa, uint64_t off) {
  if (!off_in_pool(sa, off))
    return false;
  uint64_t first = buddy_first(sa);
  return ((off - first) & (CHUNK_SIZE - 1)) == 0;
}

static inline free_blk_hdr_t *free_hdr(shared_area_t *sa, uint64_t off) {
  return (free_blk_hdr_t *)off_to_ptr(sa, off);
}

static inline uint64_t free_get_next(shared_area_t *sa, uint64_t off) {
  if (!off_valid(sa, off)) {
    fprintf(stderr,
            "buddy: invalid free list off=0x%llx (host=%u) in get_next\n",
            (unsigned long long)off, shm_state.host_id);
    return 0;
  }
  free_blk_hdr_t *h = free_hdr(sa, off);
  return LOAD_CACHELINE(&h->next_off.value);
}

static inline void free_set_next(shared_area_t *sa, uint64_t off,
                                 uint64_t next) {
  if (!off_valid(sa, off)) {
    fprintf(stderr,
            "buddy: invalid free list off=0x%llx (host=%u) in set_next\n",
            (unsigned long long)off, shm_state.host_id);
    return;
  }
  free_blk_hdr_t *h = free_hdr(sa, off);
  STORE_CACHELINE(&h->next_off.value, next);
}

static inline uint64_t head_get(shared_area_t *sa, uint32_t order) {
  return CACHELINE_LOAD(&sa->buddy.free_head[order]);
}

static inline void head_set(shared_area_t *sa, uint32_t order, uint64_t off) {
  CACHELINE_STORE(&sa->buddy.free_head[order], off);
}

static inline void lock_order(shared_area_t *sa, uint32_t order) {
  shm_mutex_lock(&sa->buddy.free_lock[order], shm_state.host_id,
                 shm_state.host_num);
}
static inline void unlock_order(shared_area_t *sa, uint32_t order) {
  shm_mutex_unlock(&sa->buddy.free_lock[order], shm_state.host_id);
}

static uint32_t size_to_order(shared_area_t *sa, uint64_t size_rounded) {
  /* size_rounded already aligned to CHUNK_SIZE */
  uint32_t order = 0;
  uint64_t s = CHUNK_SIZE;
  while (s < size_rounded && order < buddy_max_order(sa)) {
    s <<= 1;
    order++;
  }
  return order;
}

static uint64_t buddy_of(shared_area_t *sa, uint64_t off, uint32_t order) {
  uint64_t first = buddy_first(sa);
  uint64_t rel = off - first;
  uint64_t bsz = order_size(order);
  uint64_t buddy_rel = rel ^ bsz;
  return first + buddy_rel;
}

/* Push a block onto free list (order lock must be held). */
static void buddy_push_locked(shared_area_t *sa, uint32_t order, uint64_t off) {
  uint64_t head = head_get(sa, order);
  free_set_next(sa, off, head);
  head_set(sa, order, off);
}

/* Pop a block from free list; returns 0 if empty (order lock must be held). */
static uint64_t buddy_pop_locked(shared_area_t *sa, uint32_t order) {
  uint64_t head = head_get(sa, order);
  if (head == 0)
    return 0;
  if (!off_valid(sa, head)) {
    fprintf(stderr, "buddy: invalid head off=0x%llx (host=%u) order=%u\n",
            (unsigned long long)head, shm_state.host_id, order);
    head_set(sa, order, 0);
    return 0;
  }
  uint64_t next = free_get_next(sa, head);
  head_set(sa, order, next);
  free_set_next(sa, head, 0);
  return head;
}

/*
 * Remove a specific block 'target' from free list (order lock must be held).
 * Returns true if removed.
 */
static bool buddy_remove_locked(shared_area_t *sa, uint32_t order,
                                uint64_t target) {
  uint64_t head = head_get(sa, order);
  uint64_t prev = 0;
  uint64_t cur = head;

  while (cur != 0) {
    if (!off_valid(sa, cur)) {
      fprintf(stderr, "buddy: invalid list off=0x%llx (host=%u) order=%u\n",
              (unsigned long long)cur, shm_state.host_id, order);
      break;
    }
    if (cur == target) {
      uint64_t next = free_get_next(sa, cur);
      if (prev == 0) {
        head_set(sa, order, next);
      } else {
        free_set_next(sa, prev, next);
      }
      free_set_next(sa, cur, 0);
      return true;
    }
    prev = cur;
    cur = free_get_next(sa, cur);
  }
  return false;
}

/*
 * Allocate a block of given order.
 * Policy: search from 'order' upward for first non-empty list.
 * If found at higher order k, split down to requested order.
 *
 * Locking:
 * - We lock only one order while scanning.
 * - Once we find k, we lock orders [order..k] in ascending order,
 *   re-check head[k], then pop and split deterministically.
 */
static int buddy_alloc(shared_area_t *sa, uint32_t order, uint64_t *out_off) {
  uint32_t maxo = buddy_max_order(sa);
  if (order > maxo)
    return -ENOSPC;

  uint32_t k = order;

  /* 1) Lock progressively upward and KEEP locks held. */
  for (; k <= maxo; ++k) {
    lock_order(sa, k);

    /* Check under lock. */
    if (head_get(sa, k) != 0) { /* CACHELINE_LOAD */
      break; /* found a non-empty order k, locks [order..k] are held */
    }
  }

  if (k > maxo) {
    /* Nothing available: unlock what we locked */
    for (uint32_t u = order; u <= maxo; ++u)
      unlock_order(sa, u);
    return -ENOSPC;
  }

  /* 2) Pop from found order under lock. (Should succeed, but be defensive.) */
  uint64_t blk = buddy_pop_locked(sa, k);
  if (blk == 0) {
    /* Rare: list became empty unexpectedly (corruption or bug). */
    for (uint32_t u = order; u <= k; ++u)
      unlock_order(sa, u);
    return -ENOSPC;
  }

  /* 3) Split down to requested order, pushing the “upper halves” into
        lower-order lists we already hold locks for. */
  for (uint32_t cur = k; cur > order; --cur) {
    uint32_t down = cur - 1;
    uint64_t half = order_size(down);
    uint64_t bud = blk + half;

    buddy_push_locked(sa, down, bud);
  }

  /* 4) Unlock in the same direction you locked (either is fine since all held).
   */
  for (uint32_t u = order; u <= k; ++u)
    unlock_order(sa, u);

  *out_off = blk;
  return 0;
}

/*
 * Free a block; try to merge with buddy if buddy is free.
 * We lock only the current order while checking/removing buddy.
 * If buddy found, we merge and go up one order and try again.
 */
static int buddy_free(shared_area_t *sa, uint64_t off, uint32_t order) {
  uint32_t maxo = buddy_max_order(sa);
  if (order > maxo)
    return -EINVAL;
  if (!off_valid(sa, off))
    return -EINVAL;

  uint64_t cur_off = off;
  uint32_t cur_order = order;

  while (cur_order < maxo) {
    uint64_t bud = buddy_of(sa, cur_off, cur_order);
    if (!off_in_pool(sa, bud))
      break;

    lock_order(sa, cur_order);
    bool removed = buddy_remove_locked(sa, cur_order, bud);
    if (removed) {
      /* buddy was free; merge */
      unlock_order(sa, cur_order);
      cur_off = (cur_off < bud) ? cur_off : bud;
      cur_order++;
      continue;
    }

    /* buddy not free; just insert current block here */
    buddy_push_locked(sa, cur_order, cur_off);
    unlock_order(sa, cur_order);
    return 0;
  }

  /* reached max order, or cannot merge further */
  lock_order(sa, cur_order);
  buddy_push_locked(sa, cur_order, cur_off);
  unlock_order(sa, cur_order);
  return 0;
}

/* Initialize free lists to cover the pool with maximal aligned blocks. */
static void buddy_init_pool(shared_area_t *sa, uint64_t first,
                            uint64_t pool_bytes, uint32_t maxo) {
  /* Clear heads */
  for (uint32_t o = 0; o <= BUDDY_MAX_ORDER; ++o) {
    head_set(sa, o, 0);
  }

  uint64_t off = first;
  uint64_t rem = pool_bytes;

  while (rem >= CHUNK_SIZE) {
    uint32_t o = maxo;

    /* Find largest order that fits in rem AND satisfies alignment. */
    while (o > 0) {
      uint64_t bsz = order_size(o);
      if (bsz <= rem && ((off - first) % bsz) == 0)
        break;
      o--;
    }

    /* Ensure it fits; o==0 always fits if rem>=CHUNK_SIZE */
    uint64_t bsz = order_size(o);
    if (bsz > rem)
      o = 0, bsz = CHUNK_SIZE;

    /* Push block */
    lock_order(sa, o);
    buddy_push_locked(sa, o, off);
    unlock_order(sa, o);

    off += bsz;
    rem -= bsz;
  }
}

/* ---- reset/init ---- */

static int shm_reset_under_lock(shared_area_t *sa) {
  /* pool start aligned to 2MB */
  uint64_t off = (uint64_t)offsetof(shared_area_t, chunks);
  uint64_t first = align_up_u64(off, CHUNK_SIZE);
  if (first >= shm_state.map_size)
    return -EINVAL;

  uint64_t pool_bytes = (uint64_t)shm_state.map_size - first;
  pool_bytes &= ~((uint64_t)CHUNK_SIZE - 1); /* round down to 2MB */
  if (pool_bytes < CHUNK_SIZE)
    return -EINVAL;

  /* compute max order */
  uint32_t maxo = 0;
  while (maxo < BUDDY_MAX_ORDER) {
    uint64_t bsz = order_size(maxo + 1);
    if (bsz > pool_bytes)
      break;
    maxo++;
  }

  /* Clear magic first (publish-valid last) */
  CACHELINE_STORE(&sa->metadata.magic, 0);

  CACHELINE_STORE(&sa->metadata.host_num, shm_state.host_num);
  CACHELINE_STORE(&sa->metadata.map_size, shm_state.map_size);

  CACHELINE_STORE(&sa->metadata.first_chunk_off, first);
  CACHELINE_STORE(&sa->metadata.pool_bytes, pool_bytes);
  CACHELINE_STORE(&sa->metadata.max_order, maxo);

  /* Init locks */
  shm_mutex_init(&sa->id_map_lock);
  for (uint32_t o = 0; o <= BUDDY_MAX_ORDER; ++o) {
    shm_mutex_init(&sa->buddy.free_lock[o]);
    head_set(sa, o, 0);
  }

  /* Reset id map entries */
  for (int i = 0; i < IDS_CAPACITY; ++i) {
    CACHELINE_STORE(&sa->id_map.map[i].slot_state, SLOT_FREE);
    sa->id_map.map[i].id = 0;
    sa->id_map.map[i].off = 0;
    sa->id_map.map[i].size = 0;
    CACHELINE_STORE(&sa->id_map.map[i].alloc_order, 0);
    sa->id_map.map[i].alloc_state = 0;
    sa->id_map.map[i].user_bitmap = 0;
  }
  flush_region(&sa->id_map.map[0], sizeof(sa->id_map.map));

  /* Build buddy free lists */
  buddy_init_pool(sa, first, pool_bytes, maxo);

  /* Publish magic last */
  CACHELINE_STORE(&sa->metadata.magic, SHM_MAGIC);
  return 0;
}

/* ---- API ---- */

int32_t shm_enable(struct shm_arg arg) {
  pid_t cur_pid = getpid();
  if (shm_state.initialized && shm_state.pid == cur_pid) {
    return -EALREADY;
  }

  if (arg.host_num == 0 || arg.host_num > MAX_HOST_NUM ||
      arg.host_id >= arg.host_num) {
    return -EINVAL;
  }

  /* Only host0 may request reset */
  if ((arg.flags & SHM_ENABLE_RESET) && arg.host_id != 0) {
    return -EPERM;
  }

  shm_state = (struct shm_state){
      .initialized = true,
      .host_id = arg.host_id,
      .host_num = arg.host_num,
      .flags = arg.flags,
      .map_size = arg.map_size,
      .pid = cur_pid,
  };

  (void)snprintf(shm_state.dax_dev, DEV_NAME_LENGTH, "%s", arg.dax_dev);

  int fd = open(shm_state.dax_dev, O_RDWR, 0666);
  if (fd < 0 && errno == ENOENT) {
    fprintf(stderr, "open(%s) returned ENOENT, retrying with O_CREAT\n",
            shm_state.dax_dev);
    fd = open(shm_state.dax_dev, O_RDWR | O_CREAT, 0666);
  }
  if (fd < 0) {
    fprintf(stderr, "open(%s) failed: %s\n", shm_state.dax_dev,
            strerror(errno));
    return -errno;
  }

  struct stat st;
  if (fstat(fd, &st) != 0) {
    int err = errno;
    close(fd);
    return -err;
  }

  /* Only size regular files. Avoid ftruncate on /dev/dax* char devices. */
  if (S_ISREG(st.st_mode)) {
    if ((uint64_t)st.st_size < (uint64_t)shm_state.map_size) {
      if (ftruncate(fd, (off_t)shm_state.map_size) != 0) {
        int err = errno;
        close(fd);
        return -err;
      }
    }
  }

  int mmap_flags = MAP_SHARED;
#ifdef MAP_SYNC
  mmap_flags |= MAP_SYNC | MAP_SHARED_VALIDATE;
#endif

  void *mapped =
      mmap(NULL, shm_state.map_size, PROT_READ | PROT_WRITE, mmap_flags, fd, 0);

#ifdef MAP_SYNC
  if (mapped == MAP_FAILED && (errno == EINVAL || errno == EOPNOTSUPP)) {
    mapped = mmap(NULL, shm_state.map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  fd, 0);
  }
#endif

  if (mapped == MAP_FAILED) {
    int err = errno;
    fprintf(stderr, "mmap failed: %s\n", strerror(err));
    close(fd);
    return -err;
  }
  shm_state.fd = fd;

  shm_state.shared_area = (shared_area_t *)mapped;
  printf("Mapped %s at %p (len=%zu)\n", shm_state.dax_dev, mapped,
         shm_state.map_size);

  shared_area_t *sa = shm_state.shared_area;

  /* Ensure global lock exists before locking */
  if (!SHM_MUTEX_VALID(&sa->lock)) {
    shm_mutex_init(&sa->lock);
  }

  shm_mutex_lock(&sa->lock, shm_state.host_id, shm_state.host_num);

  /* Reset request */
  if ((shm_state.flags & SHM_ENABLE_RESET) != 0) {
    int rrc = shm_reset_under_lock(sa);
    shm_mutex_unlock(&sa->lock, shm_state.host_id);
    if (rrc != 0)
      return rrc;
    /* Fall through to validation. */
  }

  /* Normal init if not valid */
  if (!SHM_VALID(sa)) {
    int rrc = shm_reset_under_lock(sa);
    shm_mutex_unlock(&sa->lock, shm_state.host_id);
    if (rrc != 0)
      return rrc;
    /* On success, continue to validation. */
  }

  /* Validate */
  if (CACHELINE_LOAD(&sa->metadata.host_num) != shm_state.host_num ||
      CACHELINE_LOAD(&sa->metadata.map_size) != shm_state.map_size) {
    fprintf(stderr, "shm metadata mismatch (hosts %u/%u, size %zu/%zu)\n",
            CACHELINE_LOAD(&sa->metadata.host_num), shm_state.host_num,
            CACHELINE_LOAD(&sa->metadata.map_size), shm_state.map_size);
    shm_mutex_unlock(&sa->lock, shm_state.host_id);
    return -EINVAL;
  }

  /* Sanity-check pool */
  uint64_t first = CACHELINE_LOAD(&sa->metadata.first_chunk_off);
  uint64_t pool = CACHELINE_LOAD(&sa->metadata.pool_bytes);
  uint32_t maxo = CACHELINE_LOAD(&sa->metadata.max_order);
  if ((first & (CHUNK_SIZE - 1)) != 0 || first >= shm_state.map_size ||
      pool < CHUNK_SIZE || first + pool > shm_state.map_size ||
      maxo > BUDDY_MAX_ORDER) {
    fprintf(stderr, "bad buddy metadata: first=%llu pool=%llu maxo=%u\n",
            (unsigned long long)first, (unsigned long long)pool, maxo);
    shm_mutex_unlock(&sa->lock, shm_state.host_id);
    return -EINVAL;
  }

  shm_mutex_unlock(&sa->lock, shm_state.host_id);
  return 0;
}

void *shm_malloc_id(uint64_t id, size_t size) {
  if (!shm_ready()) {
    errno = EINVAL;
    return NULL;
  }
  if (size == 0) {
    errno = EINVAL;
    return NULL;
  }

  shared_area_t *sa = shm_state.shared_area;

  /* Round to 2MB minimum for allocator request */
  uint64_t need = align_up_u64((uint64_t)size, CHUNK_SIZE);
  uint32_t order = size_to_order(sa, need);

  shm_mutex_lock(&sa->id_map_lock, shm_state.host_id, shm_state.host_num);

  bool found = false;
  uint64_t idx = 0;
  int rc = idmap_find_slot(sa, id, &found, &idx);
  if (rc != 0) {
    shm_mutex_unlock(&sa->id_map_lock, shm_state.host_id);
    errno = -rc;
    return NULL;
  }
  if (found) {
    shm_mutex_unlock(&sa->id_map_lock, shm_state.host_id);
    errno = EEXIST;
    return NULL;
  }

  /* Allocate from buddy */
  uint64_t off = 0;
  rc = buddy_alloc(sa, order, &off);
  if (rc != 0) {
    shm_mutex_unlock(&sa->id_map_lock, shm_state.host_id);
    errno = -rc; /* typically ENOSPC */
    return NULL;
  }
  if (!off_valid(sa, off)) {
    shm_mutex_unlock(&sa->id_map_lock, shm_state.host_id);
    errno = EIO;
    return NULL;
  }

  shm_id_entry_t *e = &sa->id_map.map[idx];
  e->id = id;
  e->off = off;
  e->size = (uint64_t)size;
  CACHELINE_STORE(&e->alloc_order, order);
  e->alloc_state = ALLOC_ALLOCATED;

  uint8_t bit = (uint8_t)(1u << (shm_state.host_id & 7));
  e->user_bitmap = bit;

  flush_region(e, sizeof(*e));
  store_fence();
  /* Publish OCCUPIED last */
  CACHELINE_STORE(&e->slot_state, SLOT_OCCUPIED);

  shm_mutex_unlock(&sa->id_map_lock, shm_state.host_id);

  return off_to_ptr(sa, off);
}

void *shm_attach_id(uint64_t id, size_t size) {
  if (!shm_ready()) {
    errno = EINVAL;
    return NULL;
  }

  shared_area_t *sa = shm_state.shared_area;

  shm_mutex_lock(&sa->id_map_lock, shm_state.host_id, shm_state.host_num);

  bool found = false;
  uint64_t idx = 0;
  int rc = idmap_find_slot(sa, id, &found, &idx);
  if (rc != 0) {
    shm_mutex_unlock(&sa->id_map_lock, shm_state.host_id);
    errno = -rc;
    return NULL;
  }
  if (!found) {
    shm_mutex_unlock(&sa->id_map_lock, shm_state.host_id);
    errno = ENOENT;
    return NULL;
  }

  shm_id_entry_t *e = &sa->id_map.map[idx];
  shm_id_entry_t cur;
  idmap_load_entry(e, &cur);

  /*
   * Allow callers to request <= the published size. Some apps pass a smaller
   * length than the original allocation; only reject if the request exceeds
   * the recorded size.
   */
  if (size != 0 && (uint64_t)size > cur.size) {
    shm_mutex_unlock(&sa->id_map_lock, shm_state.host_id);
    errno = EINVAL;
    return NULL;
  }

  uint8_t bit = (uint8_t)(1u << (shm_state.host_id & 7));
  e->user_bitmap = (uint8_t)(cur.user_bitmap | bit);
  e->alloc_state = ALLOC_ATTACHED;

  flush_region(e, sizeof(*e));

  uint64_t off = cur.off;

  shm_mutex_unlock(&sa->id_map_lock, shm_state.host_id);

  if (!off_valid(sa, off)) {
    errno = EINVAL;
    return NULL;
  }

  return off_to_ptr(sa, off);
}

int32_t shm_detach_id(uint64_t id) {
  if (!shm_ready())
    return -EINVAL;

  shared_area_t *sa = shm_state.shared_area;

  shm_mutex_lock(&sa->id_map_lock, shm_state.host_id, shm_state.host_num);

  bool found = false;
  uint64_t idx = 0;
  int rc = idmap_find_slot(sa, id, &found, &idx);
  if (rc != 0) {
    shm_mutex_unlock(&sa->id_map_lock, shm_state.host_id);
    return rc;
  }
  if (!found) {
    shm_mutex_unlock(&sa->id_map_lock, shm_state.host_id);
    return -ENOENT;
  }

  shm_id_entry_t *e = &sa->id_map.map[idx];
  shm_id_entry_t cur;
  idmap_load_entry(e, &cur);
  uint8_t bit = (uint8_t)(1u << (shm_state.host_id & 7));
  e->user_bitmap = (uint8_t)(cur.user_bitmap & (uint8_t)~bit);

  flush_region(e, sizeof(*e));

  shm_mutex_unlock(&sa->id_map_lock, shm_state.host_id);
  return 0;
}

int32_t shm_free_id(uint64_t id) {
  if (!shm_ready())
    return -EINVAL;

  shared_area_t *sa = shm_state.shared_area;

  shm_mutex_lock(&sa->id_map_lock, shm_state.host_id, shm_state.host_num);

  bool found = false;
  uint64_t idx = 0;
  int rc = idmap_find_slot(sa, id, &found, &idx);
  if (rc != 0) {
    shm_mutex_unlock(&sa->id_map_lock, shm_state.host_id);
    return rc;
  }
  if (!found) {
    shm_mutex_unlock(&sa->id_map_lock, shm_state.host_id);
    return -ENOENT;
  }

  shm_id_entry_t *e = &sa->id_map.map[idx];
  shm_id_entry_t cur;
  idmap_load_entry(e, &cur);

  /* Must be fully detached before free */
  if (cur.user_bitmap != 0) {
    shm_mutex_unlock(&sa->id_map_lock, shm_state.host_id);
    return -EBUSY;
  }

  uint64_t off = cur.off;
  uint32_t order = (uint32_t)cur.alloc_order.value;
  if (!off_valid(sa, off)) {
    shm_mutex_unlock(&sa->id_map_lock, shm_state.host_id);
    return -EINVAL;
  }

  /* Tombstone ID entry first (so others won't attach new) */
  e->id = 0;
  e->off = 0;
  e->size = 0;
  CACHELINE_STORE(&e->alloc_order, 0);
  e->alloc_state = ALLOC_NONE;
  e->user_bitmap = 0;

  flush_region(e, sizeof(*e));
  store_fence();
  CACHELINE_STORE(&e->slot_state, SLOT_TOMBSTONE);

  shm_mutex_unlock(&sa->id_map_lock, shm_state.host_id);

  /* Return memory to buddy */
  if (!off_in_pool(sa, off)) {
    return -EINVAL;
  }
  int frc = buddy_free(sa, off, order);
  return frc;
}

