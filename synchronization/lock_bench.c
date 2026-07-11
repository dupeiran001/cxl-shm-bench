#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../global_allocator/common.h"
#include "bakery_lock.h"
#include "centralized_lock.h"
#include "lfm_lock.h"
#include "peterson_lock.h"
#include "spin_lock.h"

#define LOCK_TEST_MAGIC 0x4c4f434b54455354ULL /* "LOCKTEST" */
#define DAX_ALIGN (2 * 1024 * 1024)
#define PAGE_ALIGN 4096

typedef enum {
  LOCK_LFM,
  LOCK_PETERSON,
  LOCK_BAKERY,
  LOCK_CENTRALIZED,
  LOCK_SPIN
} lock_type_t;

typedef struct {
  cacheline_u64 magic;
  cacheline_u64 ready;
  cacheline_u64 lock_type;
  cacheline_u64 num_hosts;
  cacheline_u64 num_locks;
  cacheline_u64 channels;
} lock_header_t;

typedef struct {
  cacheline_u64 global_counter;
  cacheline_u64 error_count;
} lock_stats_t;

typedef struct {
  lock_stats_t *stats;
} monitor_ctx_t;

typedef struct {
  lock_type_t lock_type;
  int host_id;
  int num_hosts;
  int threads;
  int num_locks;
  int channels;
  uint64_t iterations;
  double seconds;
  uint64_t hold_ns;
  uint64_t gap_ns;
  int daemon;
  int init;
  int debug;
  int no_flush_stats;
  size_t mmap_offset;
  const char *dax_dev;
} config_t;

typedef struct {
  uint64_t *lock_lat_ns;
  uint64_t *unlock_lat_ns;
  size_t count;
  size_t cap;
  uint64_t retries;
  uint64_t poll_loops;
} thread_stats_t;

typedef struct {
  config_t *cfg;
  pthread_mutex_t *local_mutexes;
  void *lock_mem;
  central_layout_t central_layout;
  central_local_t *central_local;
  lock_stats_t *shared_stats;
  cacheline_u64 *per_host_count;
  cacheline_u64 *in_cs_locks;
  cacheline_u64 *per_host_state;
  uint64_t local_count;
  int thread_id;
  uint64_t start_ns;
  uint64_t duration_ns;
  int time_mode;
  thread_stats_t stats;
} thread_ctx_t;

static inline uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline uint64_t stat_load(const config_t *cfg, cacheline_u64 *ptr) {
  if (cfg->no_flush_stats && cfg->lock_type == LOCK_SPIN) {
    compiler_barrier();
    uint64_t v = ptr->value;
    compiler_barrier();
    return v;
  }
  return CACHELINE_LOAD(ptr);
}

static inline void stat_store(const config_t *cfg, cacheline_u64 *ptr,
                              uint64_t val) {
  if (cfg->no_flush_stats && cfg->lock_type == LOCK_SPIN) {
    compiler_barrier();
    ptr->value = val;
    compiler_barrier();
    return;
  }
  CACHELINE_STORE(ptr, val);
}

static void sleep_ns(uint64_t ns) {
  if (ns == 0) {
    return;
  }
  struct timespec req;
  req.tv_sec = (time_t)(ns / 1000000000ull);
  req.tv_nsec = (long)(ns % 1000000000ull);
  while (nanosleep(&req, &req) == -1 && errno == EINTR) {
  }
}

static inline size_t align_up_size(size_t v, size_t a) {
  return (v + a - 1) & ~(a - 1);
}

static void ensure_stats_capacity(thread_stats_t *stats, size_t need) {
  if (need <= stats->cap) {
    return;
  }
  size_t new_cap = stats->cap ? stats->cap : 1024;
  while (new_cap < need) {
    new_cap *= 2;
  }
  uint64_t *new_lock = realloc(stats->lock_lat_ns,
                               new_cap * sizeof(uint64_t));
  uint64_t *new_unlock = realloc(stats->unlock_lat_ns,
                                 new_cap * sizeof(uint64_t));
  if (!new_lock || !new_unlock) {
    fprintf(stderr, "stats realloc failed\n");
    exit(1);
  }
  stats->lock_lat_ns = new_lock;
  stats->unlock_lat_ns = new_unlock;
  stats->cap = new_cap;
}

static void usage(const char *prog) {
  printf("Usage: %s [OPTIONS]\n\n", prog);
  printf("Required:\n");
  printf("  --lock=lfm|peterson|bakery|centralized|spin\n");
  printf("  --hosts=<N>\n");
  printf("  --host_id=<ID>\n");
  printf("  --threads=<N>\n");
  printf("  --locks=<N>\n\n");
  printf("Optional:\n");
  printf("  --iters=<N>              iterations per lock per host (default: 1000)\n");
  printf("  --seconds=<N>            run for N seconds per host (overrides --iters)\n");
  printf("  --hold_ns=<N>            hold time inside lock (default: 0)\n");
  printf("  --gap_ns=<N>             gap after unlock (default: 0)\n");
  printf("  --channels=<N>           channels per host/lock (centralized only)\n");
  printf("  --dax_dev=<PATH>         DAX device (default: /dev/dax0.0)\n");
  printf("  --offset=<N>             mmap offset (alignment depends on dax dev)\n");
  printf("  --daemon                 run centralized lock daemon (centralized only)\n");
  printf("  --init                   initialize non-centralized lock state\n");
  printf("  --debug                  print parsed args and config\n");
  printf("  --no_flush_stats         avoid flushing stats (spin lock only)\n");
  printf("  --help                   show this message\n");
}

static void dump_argv(int argc, char **argv) {
  fprintf(stderr, "[debug] argv:");
  for (int i = 0; i < argc; i++) {
    fprintf(stderr, " %s", argv[i]);
  }
  fprintf(stderr, "\n");
}

static void dump_cfg(const config_t *cfg) {
  fprintf(stderr,
          "[debug] lock=%d hosts=%d host_id=%d threads=%d locks=%d iters=%lu "
          "seconds=%.3f channels=%d dax_dev=%s offset=%zu daemon=%d init=%d\n",
          cfg->lock_type, cfg->num_hosts, cfg->host_id, cfg->threads,
          cfg->num_locks, cfg->iterations, cfg->seconds, cfg->channels,
          cfg->dax_dev, cfg->mmap_offset, cfg->daemon, cfg->init);
}

static size_t dax_map_align(const char *path) {
  if (!path) {
    return PAGE_ALIGN;
  }
  if (strncmp(path, "/dev/dax", 8) != 0) {
    return PAGE_ALIGN;
  }
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  char sysfs_path[256];
  snprintf(sysfs_path, sizeof(sysfs_path), "/sys/class/dax/%s/align", name);
  FILE *fp = fopen(sysfs_path, "r");
  if (!fp) {
    return DAX_ALIGN;
  }
  unsigned long long align = 0;
  if (fscanf(fp, "%llu", &align) != 1 || align == 0) {
    align = DAX_ALIGN;
  }
  fclose(fp);
  return (size_t)align;
}

static void *map_region(const config_t *cfg, size_t size, size_t map_align,
                        int *fd_out) {
  int fd = open(cfg->dax_dev, O_RDWR);
  if (fd < 0) {
    perror("open dax_dev");
    return NULL;
  }

  if (cfg->mmap_offset % map_align != 0) {
    fprintf(stderr, "Error: --offset must be aligned to %zu bytes.\n",
            map_align);
    close(fd);
    return NULL;
  }

  size_t map_size = align_up_size(size, map_align);
  void *addr = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                    (off_t)cfg->mmap_offset);
  if (addr == MAP_FAILED) {
    perror("mmap");
    close(fd);
    return NULL;
  }

  *fd_out = fd;
  return addr;
}

static void wait_ready(lock_header_t *hdr) {
  while (CACHELINE_LOAD(&hdr->ready) == 0) {
    relax_cpu();
  }
}

static void init_header(lock_header_t *hdr, const config_t *cfg) {
  CACHELINE_STORE(&hdr->magic, LOCK_TEST_MAGIC);
  CACHELINE_STORE(&hdr->lock_type, (uint64_t)cfg->lock_type);
  CACHELINE_STORE(&hdr->num_hosts, (uint64_t)cfg->num_hosts);
  CACHELINE_STORE(&hdr->num_locks, (uint64_t)cfg->num_locks);
  CACHELINE_STORE(&hdr->channels, (uint64_t)cfg->channels);
  CACHELINE_STORE(&hdr->ready, 1);
}

static size_t stats_region_bytes(int num_hosts, int num_locks) {
  size_t off = align_up_size(sizeof(lock_stats_t), CACHE_LINE_SIZE);
  off += (size_t)num_hosts * sizeof(cacheline_u64);
  off = align_up_size(off, CACHE_LINE_SIZE);
  off += (size_t)num_locks * sizeof(cacheline_u64);
  off = align_up_size(off, CACHE_LINE_SIZE);
  off += (size_t)num_hosts * sizeof(cacheline_u64);
  return off;
}

static uint64_t percentile_u64(uint64_t *sorted, size_t n, double p) {
  if (n == 0) {
    return 0;
  }
  double rank = p * (double)n;
  size_t idx = (size_t)(rank + 0.999999) - 1;
  if (idx >= n) {
    idx = n - 1;
  }
  return sorted[idx];
}

static int cmp_u64(const void *a, const void *b) {
  uint64_t aa = *(const uint64_t *)a;
  uint64_t bb = *(const uint64_t *)b;
  if (aa < bb) return -1;
  if (aa > bb) return 1;
  return 0;
}

static void *monitor_thread(void *arg) {
  monitor_ctx_t *ctx = (monitor_ctx_t *)arg;
  uint64_t last_err = 0;
  while (1) {
    uint64_t err = CACHELINE_LOAD(&ctx->stats->error_count);
    if (err != last_err) {
      printf("[daemon] mutual exclusion errors: %lu\n", err);
      last_err = err;
    }
    usleep(500000);
  }
  return NULL;
}

static void *worker_thread(void *arg) {
  thread_ctx_t *ctx = (thread_ctx_t *)arg;
  const config_t *cfg = ctx->cfg;
  uint64_t ops = 0;

  if (ctx->time_mode) {
    uint64_t end_ns = ctx->start_ns + ctx->duration_ns;
    while (now_ns() < end_ns) {
      for (int lock_id = 0; lock_id < cfg->num_locks; lock_id++) {
        if (now_ns() >= end_ns) {
          break;
        }
        ensure_stats_capacity(&ctx->stats, ops + 1);
        uint64_t t0 = now_ns();
        if (cfg->lock_type != LOCK_SPIN) {
          pthread_mutex_lock(&ctx->local_mutexes[lock_id]);
        }
        if (cfg->lock_type == LOCK_LFM) {
          lfm_mutex_t *locks = (lfm_mutex_t *)ctx->lock_mem;
          uint64_t retry = lfm_mutex_lock(&locks[lock_id], cfg->host_id,
                                          cfg->num_hosts);
          if (retry != UINT64_MAX) {
            ctx->stats.retries += retry;
          }
        } else if (cfg->lock_type == LOCK_PETERSON) {
          peterson_mutex_t *locks = (peterson_mutex_t *)ctx->lock_mem;
          peterson_mutex_lock(&locks[lock_id], cfg->host_id, cfg->num_hosts);
        } else if (cfg->lock_type == LOCK_BAKERY) {
          bakery_mutex_t *locks = (bakery_mutex_t *)ctx->lock_mem;
          bakery_mutex_lock(&locks[lock_id], cfg->host_id, cfg->num_hosts);
        } else if (cfg->lock_type == LOCK_SPIN) {
          spin_lock_t *locks = (spin_lock_t *)ctx->lock_mem;
          spin_lock_lock(&locks[lock_id]);
        } else {
          central_handle_t handle;
          central_acquire(&ctx->central_layout, ctx->central_local,
                          cfg->host_id, ctx->thread_id, lock_id, &handle,
                          &ctx->stats.poll_loops, &ctx->stats.retries);
          ctx->stats.lock_lat_ns[ops] = now_ns() - t0;
          uint64_t in_cs = stat_load(cfg, &ctx->in_cs_locks[lock_id]);
          if (in_cs != 0) {
            uint64_t err = stat_load(cfg, &ctx->shared_stats->error_count);
            stat_store(cfg, &ctx->shared_stats->error_count, err + 1);
          }
          stat_store(cfg, &ctx->in_cs_locks[lock_id],
                     (uint64_t)(cfg->host_id + 1));
          uint64_t global =
              stat_load(cfg, &ctx->shared_stats->global_counter);
          global++;
          stat_store(cfg, &ctx->shared_stats->global_counter, global);
          ctx->local_count++;
          stat_store(cfg, &ctx->per_host_count[cfg->host_id], ctx->local_count);
          sleep_ns(cfg->hold_ns);
          uint64_t u0 = now_ns();
          stat_store(cfg, &ctx->in_cs_locks[lock_id], 0);
          central_release(&ctx->central_layout, ctx->central_local, cfg->host_id,
                          &handle, &ctx->stats.poll_loops);
          pthread_mutex_unlock(&ctx->local_mutexes[lock_id]);
          ctx->stats.unlock_lat_ns[ops] = now_ns() - u0;
          sleep_ns(cfg->gap_ns);
          ops++;
          continue;
        }

        uint64_t t1 = now_ns();
        ctx->stats.lock_lat_ns[ops] = t1 - t0;

        uint64_t in_cs = stat_load(cfg, &ctx->in_cs_locks[lock_id]);
        if (in_cs != 0) {
          uint64_t err = stat_load(cfg, &ctx->shared_stats->error_count);
          stat_store(cfg, &ctx->shared_stats->error_count, err + 1);
        }
        stat_store(cfg, &ctx->in_cs_locks[lock_id],
                   (uint64_t)(cfg->host_id + 1));
        uint64_t global = stat_load(cfg, &ctx->shared_stats->global_counter);
        global++;
        stat_store(cfg, &ctx->shared_stats->global_counter, global);
        ctx->local_count++;
        stat_store(cfg, &ctx->per_host_count[cfg->host_id], ctx->local_count);

        sleep_ns(cfg->hold_ns);

        uint64_t u0 = now_ns();
        stat_store(cfg, &ctx->in_cs_locks[lock_id], 0);
        if (cfg->lock_type == LOCK_LFM) {
          lfm_mutex_t *locks = (lfm_mutex_t *)ctx->lock_mem;
          lfm_mutex_unlock(&locks[lock_id], cfg->host_id);
        } else if (cfg->lock_type == LOCK_PETERSON) {
          peterson_mutex_t *locks = (peterson_mutex_t *)ctx->lock_mem;
          peterson_mutex_unlock(&locks[lock_id], cfg->host_id);
        } else if (cfg->lock_type == LOCK_BAKERY) {
          bakery_mutex_t *locks = (bakery_mutex_t *)ctx->lock_mem;
          bakery_mutex_unlock(&locks[lock_id], cfg->host_id);
        } else if (cfg->lock_type == LOCK_SPIN) {
          spin_lock_t *locks = (spin_lock_t *)ctx->lock_mem;
          spin_lock_unlock(&locks[lock_id]);
        }
        if (cfg->lock_type != LOCK_SPIN) {
          pthread_mutex_unlock(&ctx->local_mutexes[lock_id]);
        }
        uint64_t u1 = now_ns();
        ctx->stats.unlock_lat_ns[ops] = u1 - u0;

        sleep_ns(cfg->gap_ns);
        ops++;
      }
    }
  } else {
    uint64_t base = cfg->iterations / (uint64_t)cfg->threads;
    uint64_t rem = cfg->iterations % (uint64_t)cfg->threads;
    uint64_t local_iters = base + ((uint64_t)ctx->thread_id < rem ? 1 : 0);
    for (uint64_t iter = 0; iter < local_iters; iter++) {
      for (int lock_id = 0; lock_id < cfg->num_locks; lock_id++) {
        ensure_stats_capacity(&ctx->stats, ops + 1);
      uint64_t t0 = now_ns();
      if (cfg->lock_type != LOCK_SPIN) {
        pthread_mutex_lock(&ctx->local_mutexes[lock_id]);
      }

      if (cfg->lock_type == LOCK_LFM) {
        lfm_mutex_t *locks = (lfm_mutex_t *)ctx->lock_mem;
        uint64_t retry = lfm_mutex_lock(&locks[lock_id], cfg->host_id,
                                        cfg->num_hosts);
        if (retry != UINT64_MAX) {
          ctx->stats.retries += retry;
        }
      } else if (cfg->lock_type == LOCK_PETERSON) {
        peterson_mutex_t *locks = (peterson_mutex_t *)ctx->lock_mem;
        peterson_mutex_lock(&locks[lock_id], cfg->host_id, cfg->num_hosts);
      } else if (cfg->lock_type == LOCK_BAKERY) {
        bakery_mutex_t *locks = (bakery_mutex_t *)ctx->lock_mem;
        bakery_mutex_lock(&locks[lock_id], cfg->host_id, cfg->num_hosts);
      } else if (cfg->lock_type == LOCK_SPIN) {
        spin_lock_t *locks = (spin_lock_t *)ctx->lock_mem;
        spin_lock_lock(&locks[lock_id]);
      } else {
        central_handle_t handle;
        central_acquire(&ctx->central_layout, ctx->central_local, cfg->host_id,
                        ctx->thread_id, lock_id, &handle,
                        &ctx->stats.poll_loops, &ctx->stats.retries);
        ctx->stats.lock_lat_ns[ops] = now_ns() - t0;
          uint64_t in_cs = stat_load(cfg, &ctx->in_cs_locks[lock_id]);
          if (in_cs != 0) {
            uint64_t err = stat_load(cfg, &ctx->shared_stats->error_count);
            stat_store(cfg, &ctx->shared_stats->error_count, err + 1);
          }
          stat_store(cfg, &ctx->in_cs_locks[lock_id],
                     (uint64_t)(cfg->host_id + 1));
        uint64_t global = stat_load(cfg, &ctx->shared_stats->global_counter);
        global++;
        stat_store(cfg, &ctx->shared_stats->global_counter, global);
        ctx->local_count++;
        stat_store(cfg, &ctx->per_host_count[cfg->host_id], ctx->local_count);
        sleep_ns(cfg->hold_ns);
        uint64_t u0 = now_ns();
          stat_store(cfg, &ctx->in_cs_locks[lock_id], 0);
        central_release(&ctx->central_layout, ctx->central_local, cfg->host_id,
                        &handle, &ctx->stats.poll_loops);
        pthread_mutex_unlock(&ctx->local_mutexes[lock_id]);
        ctx->stats.unlock_lat_ns[ops] = now_ns() - u0;
        sleep_ns(cfg->gap_ns);
        ops++;
        continue;
      }

      uint64_t t1 = now_ns();
      ctx->stats.lock_lat_ns[ops] = t1 - t0;

        uint64_t in_cs = stat_load(cfg, &ctx->in_cs_locks[lock_id]);
        if (in_cs != 0) {
          uint64_t err = stat_load(cfg, &ctx->shared_stats->error_count);
          stat_store(cfg, &ctx->shared_stats->error_count, err + 1);
        }
        stat_store(cfg, &ctx->in_cs_locks[lock_id],
                   (uint64_t)(cfg->host_id + 1));
      uint64_t global = stat_load(cfg, &ctx->shared_stats->global_counter);
      global++;
      stat_store(cfg, &ctx->shared_stats->global_counter, global);
      ctx->local_count++;
      stat_store(cfg, &ctx->per_host_count[cfg->host_id], ctx->local_count);

      sleep_ns(cfg->hold_ns);

      uint64_t u0 = now_ns();
        stat_store(cfg, &ctx->in_cs_locks[lock_id], 0);
      if (cfg->lock_type == LOCK_LFM) {
        lfm_mutex_t *locks = (lfm_mutex_t *)ctx->lock_mem;
        lfm_mutex_unlock(&locks[lock_id], cfg->host_id);
      } else if (cfg->lock_type == LOCK_PETERSON) {
        peterson_mutex_t *locks = (peterson_mutex_t *)ctx->lock_mem;
        peterson_mutex_unlock(&locks[lock_id], cfg->host_id);
      } else if (cfg->lock_type == LOCK_BAKERY) {
        bakery_mutex_t *locks = (bakery_mutex_t *)ctx->lock_mem;
        bakery_mutex_unlock(&locks[lock_id], cfg->host_id);
      } else if (cfg->lock_type == LOCK_SPIN) {
        spin_lock_t *locks = (spin_lock_t *)ctx->lock_mem;
        spin_lock_unlock(&locks[lock_id]);
      }
      if (cfg->lock_type != LOCK_SPIN) {
        pthread_mutex_unlock(&ctx->local_mutexes[lock_id]);
      }
      uint64_t u1 = now_ns();
      ctx->stats.unlock_lat_ns[ops] = u1 - u0;

      sleep_ns(cfg->gap_ns);
      ops++;
    }
    }
  }

  ctx->stats.count = ops;
  return NULL;
}

int main(int argc, char **argv) {
  config_t cfg = {
      .lock_type = LOCK_LFM,
      .host_id = -1,
      .num_hosts = 0,
      .threads = 1,
      .num_locks = 1,
      .channels = 1,
      .iterations = 1000,
      .seconds = 0.0,
      .hold_ns = 0,
      .gap_ns = 0,
      .daemon = 0,
      .init = 0,
      .debug = 0,
      .no_flush_stats = 0,
      .mmap_offset = 0,
      .dax_dev = "/dev/dax0.0",
  };

  enum {
    OPT_LOCK = 1000,
    OPT_HOSTS,
    OPT_HOST_ID,
    OPT_THREADS,
    OPT_LOCKS,
    OPT_ITERS,
    OPT_HOLD,
    OPT_GAP,
    OPT_CHANNELS,
    OPT_DAX_DEV,
    OPT_OFFSET,
    OPT_DAEMON,
    OPT_INIT,
    OPT_DEBUG,
    OPT_SECONDS,
    OPT_NO_FLUSH_STATS,
  };

  static struct option opts[] = {
      {"lock", required_argument, NULL, OPT_LOCK},
      {"hosts", required_argument, NULL, OPT_HOSTS},
      {"host_id", required_argument, NULL, OPT_HOST_ID},
      {"threads", required_argument, NULL, OPT_THREADS},
      {"locks", required_argument, NULL, OPT_LOCKS},
      {"iters", required_argument, NULL, OPT_ITERS},
      {"hold_ns", required_argument, NULL, OPT_HOLD},
      {"gap_ns", required_argument, NULL, OPT_GAP},
      {"channels", required_argument, NULL, OPT_CHANNELS},
      {"dax_dev", required_argument, NULL, OPT_DAX_DEV},
      {"offset", required_argument, NULL, OPT_OFFSET},
      {"daemon", no_argument, NULL, OPT_DAEMON},
      {"init", no_argument, NULL, OPT_INIT},
      {"debug", no_argument, NULL, OPT_DEBUG},
      {"seconds", required_argument, NULL, OPT_SECONDS},
      {"no_flush_stats", no_argument, NULL, OPT_NO_FLUSH_STATS},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0},
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
    switch (opt) {
    case OPT_LOCK:
      if (strcmp(optarg, "lfm") == 0) {
        cfg.lock_type = LOCK_LFM;
      } else if (strcmp(optarg, "peterson") == 0) {
        cfg.lock_type = LOCK_PETERSON;
      } else if (strcmp(optarg, "bakery") == 0) {
        cfg.lock_type = LOCK_BAKERY;
      } else if (strcmp(optarg, "centralized") == 0) {
        cfg.lock_type = LOCK_CENTRALIZED;
      } else if (strcmp(optarg, "spin") == 0) {
        cfg.lock_type = LOCK_SPIN;
      } else {
        fprintf(stderr, "Unknown lock type: %s\n", optarg);
        return 1;
      }
      break;
    case OPT_HOSTS:
      cfg.num_hosts = atoi(optarg);
      break;
    case OPT_HOST_ID:
      cfg.host_id = atoi(optarg);
      break;
    case OPT_THREADS:
      cfg.threads = atoi(optarg);
      break;
    case OPT_LOCKS:
      cfg.num_locks = atoi(optarg);
      break;
    case OPT_ITERS:
      cfg.iterations = strtoull(optarg, NULL, 0);
      break;
    case OPT_HOLD:
      cfg.hold_ns = strtoull(optarg, NULL, 0);
      break;
    case OPT_GAP:
      cfg.gap_ns = strtoull(optarg, NULL, 0);
      break;
    case OPT_CHANNELS:
      cfg.channels = atoi(optarg);
      break;
    case OPT_DAX_DEV:
      cfg.dax_dev = optarg;
      break;
    case OPT_OFFSET:
      cfg.mmap_offset = (size_t)strtoull(optarg, NULL, 0);
      break;
    case OPT_DAEMON:
      cfg.daemon = 1;
      break;
    case OPT_INIT:
      cfg.init = 1;
      break;
    case OPT_DEBUG:
      cfg.debug = 1;
      break;
    case OPT_SECONDS:
      cfg.seconds = strtod(optarg, NULL);
      break;
    case OPT_NO_FLUSH_STATS:
      cfg.no_flush_stats = 1;
      break;
    case 'h':
      usage(argv[0]);
      return 0;
    default:
      if (optind > 0 && optind <= argc) {
        fprintf(stderr, "Unknown option near: %s\n", argv[optind - 1]);
      }
      usage(argv[0]);
      return 1;
    }
  }

  if (cfg.debug) {
    dump_argv(argc, argv);
    dump_cfg(&cfg);
    fprintf(stderr, "[debug] centralized=%d daemon=%d seconds=%.3f\n",
            cfg.lock_type == LOCK_CENTRALIZED, cfg.daemon, cfg.seconds);
  }

  if (cfg.num_hosts <= 0) {
    fprintf(stderr, "Error: --hosts must be > 0\n");
    dump_argv(argc, argv);
    dump_cfg(&cfg);
    usage(argv[0]);
    return 1;
  }
  if (cfg.host_id < 0 || cfg.host_id >= cfg.num_hosts) {
    fprintf(stderr, "Error: --host_id must be in [0, %d]\n",
            cfg.num_hosts - 1);
    dump_argv(argc, argv);
    dump_cfg(&cfg);
    usage(argv[0]);
    return 1;
  }
  if (cfg.threads <= 0) {
    fprintf(stderr, "Error: --threads must be > 0\n");
    dump_argv(argc, argv);
    dump_cfg(&cfg);
    usage(argv[0]);
    return 1;
  }
  if (cfg.num_locks <= 0) {
    fprintf(stderr, "Error: --locks must be > 0\n");
    dump_argv(argc, argv);
    dump_cfg(&cfg);
    usage(argv[0]);
    return 1;
  }
  if (cfg.seconds < 0.0) {
    fprintf(stderr, "Error: --seconds must be >= 0\n");
    dump_argv(argc, argv);
    dump_cfg(&cfg);
    usage(argv[0]);
    return 1;
  }
  if (cfg.lock_type != LOCK_CENTRALIZED && cfg.num_hosts > MAX_HOST_NUM) {
    fprintf(stderr, "max supported hosts for this lock is %d\n", MAX_HOST_NUM);
    return 1;
  }
  if (cfg.lock_type == LOCK_SPIN && cfg.num_hosts != 1) {
    fprintf(stderr, "spin lock only supports --hosts=1\n");
    return 1;
  }

  if (cfg.lock_type == LOCK_CENTRALIZED && cfg.channels <= 0) {
    fprintf(stderr, "--channels must be >= 1 for centralized lock\n");
    return 1;
  }
  if (cfg.lock_type != LOCK_CENTRALIZED && cfg.daemon) {
    fprintf(stderr, "--daemon is only valid for centralized lock\n");
    return 1;
  }
  if (cfg.lock_type == LOCK_CENTRALIZED && cfg.debug) {
    central_set_debug(1);
  }

  size_t required_size = 0;
  size_t header_size =
      (sizeof(lock_header_t) + CACHE_LINE_SIZE - 1) &
      ~(size_t)(CACHE_LINE_SIZE - 1);
  size_t lock_region_size = 0;
  if (cfg.lock_type == LOCK_CENTRALIZED) {
    lock_region_size = central_layout_bytes(cfg.num_hosts, cfg.num_locks,
                                            cfg.channels);
  } else if (cfg.lock_type == LOCK_LFM) {
    lock_region_size = (size_t)cfg.num_locks * sizeof(lfm_mutex_t);
  } else if (cfg.lock_type == LOCK_PETERSON) {
    lock_region_size = (size_t)cfg.num_locks * sizeof(peterson_mutex_t);
  } else if (cfg.lock_type == LOCK_SPIN) {
    lock_region_size = (size_t)cfg.num_locks * sizeof(spin_lock_t);
  } else {
    lock_region_size = (size_t)cfg.num_locks * sizeof(bakery_mutex_t);
  }
  lock_region_size = align_up_size(lock_region_size, CACHE_LINE_SIZE);
  size_t stats_size = stats_region_bytes(cfg.num_hosts, cfg.num_locks);
  stats_size = align_up_size(stats_size, CACHE_LINE_SIZE);
  required_size = header_size + lock_region_size + stats_size;

  size_t map_align = dax_map_align(cfg.dax_dev);
  int fd = -1;
  size_t map_size = align_up_size(required_size, map_align);
  void *base = map_region(&cfg, map_size, map_align, &fd);
  if (!base) {
    return 1;
  }

  lock_header_t *hdr = (lock_header_t *)base;
  void *data_region = (void *)((uintptr_t)base + header_size);
  void *stats_region = (void *)((uintptr_t)data_region + lock_region_size);
  lock_stats_t *stats = (lock_stats_t *)stats_region;
  size_t stats_off = align_up_size(sizeof(lock_stats_t), CACHE_LINE_SIZE);
  cacheline_u64 *per_host = (cacheline_u64 *)((uintptr_t)stats_region +
                                              stats_off);
  stats_off += (size_t)cfg.num_hosts * sizeof(cacheline_u64);
  stats_off = align_up_size(stats_off, CACHE_LINE_SIZE);
  cacheline_u64 *in_cs_locks =
      (cacheline_u64 *)((uintptr_t)stats_region + stats_off);
  stats_off += (size_t)cfg.num_locks * sizeof(cacheline_u64);
  stats_off = align_up_size(stats_off, CACHE_LINE_SIZE);
  cacheline_u64 *per_host_state =
      (cacheline_u64 *)((uintptr_t)stats_region + stats_off);

  int is_initializer = 0;
  if (cfg.lock_type == LOCK_CENTRALIZED) {
    is_initializer = cfg.daemon;
  } else {
    is_initializer = cfg.init;
    if (cfg.host_id == 0 && !cfg.init) {
      fprintf(stderr,
              "Note: non-centralized locks require --init on exactly one "
              "host.\n");
    }
  }

  pid_t daemon_pid = -1;
  if (is_initializer) {
    memset(base, 0, required_size);
    flush_region(base, required_size);

    if (cfg.lock_type == LOCK_CENTRALIZED) {
      central_init_layout(data_region, cfg.num_hosts, cfg.num_locks,
                          cfg.channels);
    } else if (cfg.lock_type == LOCK_LFM) {
      lfm_mutex_t *locks = (lfm_mutex_t *)data_region;
      for (int i = 0; i < cfg.num_locks; i++) {
        lfm_mutex_init(&locks[i]);
      }
    } else if (cfg.lock_type == LOCK_PETERSON) {
      peterson_mutex_t *locks = (peterson_mutex_t *)data_region;
      for (int i = 0; i < cfg.num_locks; i++) {
        peterson_mutex_init(&locks[i]);
      }
    } else if (cfg.lock_type == LOCK_SPIN) {
      spin_lock_t *locks = (spin_lock_t *)data_region;
      for (int i = 0; i < cfg.num_locks; i++) {
        spin_lock_init(&locks[i]);
      }
    } else {
      bakery_mutex_t *locks = (bakery_mutex_t *)data_region;
      for (int i = 0; i < cfg.num_locks; i++) {
        bakery_mutex_init(&locks[i]);
      }
    }

    init_header(hdr, &cfg);
    printf("Initialized locks (lock=%d hosts=%d locks=%d channels=%d)\n",
           cfg.lock_type, cfg.num_hosts, cfg.num_locks, cfg.channels);

    if (cfg.daemon && cfg.lock_type == LOCK_CENTRALIZED) {
      pid_t pid = fork();
      if (pid < 0) {
        perror("fork");
        munmap(base, map_size);
        close(fd);
        return 1;
      }
      if (pid == 0) {
        fprintf(stderr, "[daemon] child pid=%d starting\n", getpid());
        (void)setsid();
        central_layout_t layout =
            central_attach_layout(data_region, cfg.num_hosts, cfg.num_locks,
                                  cfg.channels);
        pthread_t monitor;
        monitor_ctx_t mctx = {.stats = stats};
        pthread_create(&monitor, NULL, monitor_thread, &mctx);
        central_manager_run(&layout);
        fprintf(stderr, "[daemon] child pid=%d exiting\n", getpid());
        _exit(0);
      }
      daemon_pid = pid;
      if (cfg.debug) {
        fprintf(stderr, "[debug] forked daemon pid=%d\n", pid);
      }
      usleep(100000);
    }

    if (cfg.daemon && cfg.lock_type != LOCK_CENTRALIZED) {
      munmap(base, map_size);
      close(fd);
      return 0;
    }
  }

  if (!is_initializer || cfg.lock_type == LOCK_CENTRALIZED) {
    wait_ready(hdr);
  }

  CACHELINE_STORE(&per_host_state[cfg.host_id], 1);

  pthread_mutex_t *local_mutexes =
      calloc((size_t)cfg.num_locks, sizeof(pthread_mutex_t));
  if (!local_mutexes) {
    fprintf(stderr, "local mutex alloc failed\n");
    return 1;
  }
  for (int i = 0; i < cfg.num_locks; i++) {
    pthread_mutex_init(&local_mutexes[i], NULL);
  }

  central_layout_t central_layout = {0};
  central_local_t central_local = {0};
  if (cfg.lock_type == LOCK_CENTRALIZED) {
    central_layout = central_attach_layout(data_region, cfg.num_hosts,
                                           cfg.num_locks, cfg.channels);
    if (central_local_init(&central_local, cfg.num_locks, cfg.channels,
                           cfg.threads) != 0) {
      fprintf(stderr, "central_local_init failed\n");
      return 1;
    }
  }

  pthread_t *threads = calloc((size_t)cfg.threads, sizeof(pthread_t));
  thread_ctx_t *ctxs = calloc((size_t)cfg.threads, sizeof(thread_ctx_t));
  if (!threads || !ctxs) {
    fprintf(stderr, "thread alloc failed\n");
    return 1;
  }

  int time_mode = (cfg.seconds > 0.0);
  uint64_t duration_ns = 0;
  if (time_mode) {
    duration_ns = (uint64_t)(cfg.seconds * 1000000000.0);
  }
  size_t per_thread_ops = 0;
  if (time_mode) {
    per_thread_ops = (size_t)cfg.num_locks * 1024;
  } else {
    size_t per_thread_iters =
        (cfg.iterations + (uint64_t)cfg.threads - 1) / (uint64_t)cfg.threads;
    per_thread_ops = per_thread_iters * (size_t)cfg.num_locks;
  }
  for (int t = 0; t < cfg.threads; t++) {
    ctxs[t].cfg = &cfg;
    ctxs[t].local_mutexes = local_mutexes;
    ctxs[t].lock_mem = data_region;
    ctxs[t].central_layout = central_layout;
    ctxs[t].central_local = (cfg.lock_type == LOCK_CENTRALIZED)
                                ? &central_local
                                : NULL;
    ctxs[t].shared_stats = stats;
    ctxs[t].per_host_count = per_host;
    ctxs[t].in_cs_locks = in_cs_locks;
    ctxs[t].per_host_state = per_host_state;
    ctxs[t].local_count = 0;
    ctxs[t].thread_id = t;
    ctxs[t].start_ns = 0;
    ctxs[t].duration_ns = duration_ns;
    ctxs[t].time_mode = time_mode;
    ctxs[t].stats.cap = per_thread_ops;
    ctxs[t].stats.lock_lat_ns =
        calloc(per_thread_ops, sizeof(uint64_t));
    ctxs[t].stats.unlock_lat_ns =
        calloc(per_thread_ops, sizeof(uint64_t));
    if (!ctxs[t].stats.lock_lat_ns || !ctxs[t].stats.unlock_lat_ns) {
      fprintf(stderr, "stats alloc failed\n");
      return 1;
    }
  }

  if (daemon_pid > 0 && cfg.num_hosts > 1) {
    if (cfg.debug) {
      fprintf(stderr, "[debug] waiting for all hosts ready\n");
    }
    while (1) {
      int all_ready = 1;
      for (int i = 0; i < cfg.num_hosts; i++) {
        if (CACHELINE_LOAD(&per_host_state[i]) == 0) {
          all_ready = 0;
          break;
        }
      }
      if (all_ready) {
        break;
      }
      usleep(1000);
    }
  }

  uint64_t start_ns = now_ns();
  for (int t = 0; t < cfg.threads; t++) {
    ctxs[t].start_ns = start_ns;
    pthread_create(&threads[t], NULL, worker_thread, &ctxs[t]);
  }
  for (int t = 0; t < cfg.threads; t++) {
    pthread_join(threads[t], NULL);
  }
  if (cfg.debug) {
    fprintf(stderr, "[debug] workers joined\n");
  }
  CACHELINE_STORE(&per_host_state[cfg.host_id], 2);
  if (daemon_pid > 0) {
    if (cfg.debug) {
      fprintf(stderr, "[debug] stopping daemon pid=%d\n", daemon_pid);
    }
    if (cfg.num_hosts > 1) {
      if (cfg.debug) {
        fprintf(stderr, "[debug] waiting for all hosts done\n");
      }
      while (1) {
        int all_done = 1;
        for (int i = 0; i < cfg.num_hosts; i++) {
          if (CACHELINE_LOAD(&per_host_state[i]) != 2) {
            all_done = 0;
            break;
          }
        }
        if (all_done) {
          break;
        }
        usleep(1000);
      }
    }
    kill(daemon_pid, SIGTERM);
    waitpid(daemon_pid, NULL, 0);
    if (cfg.debug) {
      fprintf(stderr, "[debug] daemon stopped\n");
    }
  }
  uint64_t end_ns = now_ns();

  size_t total_ops = 0;
  uint64_t total_retries = 0;
  uint64_t total_poll_loops = 0;
  uint64_t local_counter_total = 0;
  for (int t = 0; t < cfg.threads; t++) {
    total_ops += ctxs[t].stats.count;
    total_retries += ctxs[t].stats.retries;
    total_poll_loops += ctxs[t].stats.poll_loops;
    local_counter_total += ctxs[t].local_count;
  }

  uint64_t *lock_all = calloc(total_ops, sizeof(uint64_t));
  uint64_t *unlock_all = calloc(total_ops, sizeof(uint64_t));
  size_t offset = 0;
  for (int t = 0; t < cfg.threads; t++) {
    memcpy(lock_all + offset, ctxs[t].stats.lock_lat_ns,
           ctxs[t].stats.count * sizeof(uint64_t));
    memcpy(unlock_all + offset, ctxs[t].stats.unlock_lat_ns,
           ctxs[t].stats.count * sizeof(uint64_t));
    offset += ctxs[t].stats.count;
  }

  qsort(lock_all, total_ops, sizeof(uint64_t), cmp_u64);
  qsort(unlock_all, total_ops, sizeof(uint64_t), cmp_u64);

  double lock_avg = 0.0;
  double unlock_avg = 0.0;
  for (size_t i = 0; i < total_ops; i++) {
    lock_avg += (double)lock_all[i];
    unlock_avg += (double)unlock_all[i];
  }
  if (total_ops > 0) {
    lock_avg /= (double)total_ops;
    unlock_avg /= (double)total_ops;
  }

  printf("\n--- Lock Benchmark Results ---\n");
  printf("lock type:           %d\n", cfg.lock_type);
  printf("hosts/host_id:       %d/%d\n", cfg.num_hosts, cfg.host_id);
  printf("threads:             %d\n", cfg.threads);
  printf("locks:               %d\n", cfg.num_locks);
  printf("channels:            %d\n", cfg.channels);
  if (cfg.seconds > 0.0) {
    printf("seconds:             %.3f\n", cfg.seconds);
  } else {
    printf("iterations:          %lu\n", cfg.iterations);
  }
  printf("total ops:           %zu\n", total_ops);
  printf("local counter:       %lu\n", local_counter_total);
  printf("total time:          %.3f ms\n",
         (double)(end_ns - start_ns) / 1e6);
  printf("lock avg:            %.2f ns\n", lock_avg);
  printf("lock p50/p95/p99:    %lu / %lu / %lu ns\n",
         percentile_u64(lock_all, total_ops, 0.50),
         percentile_u64(lock_all, total_ops, 0.95),
         percentile_u64(lock_all, total_ops, 0.99));
  printf("unlock avg:          %.2f ns\n", unlock_avg);
  printf("unlock p50/p95/p99:  %lu / %lu / %lu ns\n",
         percentile_u64(unlock_all, total_ops, 0.50),
         percentile_u64(unlock_all, total_ops, 0.95),
         percentile_u64(unlock_all, total_ops, 0.99));
  printf("total retries:       %lu\n", total_retries);
  if (total_ops > 0) {
    printf("avg retries/op:      %.4f\n",
           (double)total_retries / (double)total_ops);
  }
  printf("total poll loops:    %lu\n", total_poll_loops);
  if (total_ops > 0) {
    printf("avg poll loops/op:   %.4f\n",
           (double)total_poll_loops / (double)total_ops);
  }
  printf("global counter:      %lu\n",
         CACHELINE_LOAD(&stats->global_counter));
  printf("error count:         %lu\n",
         CACHELINE_LOAD(&stats->error_count));
  printf("----------------------------\n");

  for (int t = 0; t < cfg.threads; t++) {
    free(ctxs[t].stats.lock_lat_ns);
    free(ctxs[t].stats.unlock_lat_ns);
  }
  free(lock_all);
  free(unlock_all);
  free(threads);
  free(ctxs);
  if (cfg.lock_type == LOCK_CENTRALIZED) {
    central_local_destroy(&central_local);
  }
  free(local_mutexes);
  munmap(base, map_size);
  close(fd);
  return 0;
}
#include <errno.h>
