// ht_bench.c - concurrent hash table benchmark for multi-host CXL shared
// memory (artifact e2). Host 0 creates + preloads the table; all hosts
// rendezvous on a shared-memory barrier, hammer the table for a fixed
// duration, then run a self-check (sampled readability, cross-host check
// keys, shared vs local op counters) and publish per-host results so host 0
// can print an aggregate.
#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "barrier.h"
#include "common.h"
#include "shm.h"
#include "shm_mutex.h"

#include "shm_ht.h"

#define HT_ID_CTRL 0x48540010ULL
#define HT_CTRL_MAGIC 0x48544354524c3031ULL /* "HTCTRL01" */
#define HT_MAX_THREADS 64
#define HT_CHECK_KEYS 64          /* post-run per-host insert probes */
#define HT_SAMPLE_CAP (1u << 19)  /* per-thread latency samples (bounded) */
#define HT_READ_SAMPLE 1024       /* preloaded keys sampled by self-check */
#define ZIPF_THETA 0.99

/* Shared control block (one 2MB allocator block). */
typedef struct {
  cacheline_u64 magic;
  shm_barrier_t barrier;
  cacheline_u64 host_ops[MAX_HOST_NUM];
  cacheline_u64 host_elapsed_ns[MAX_HOST_NUM];
  cacheline_u64 host_errs[MAX_HOST_NUM];
  cacheline_u64 thread_ops[MAX_HOST_NUM][HT_MAX_THREADS];
} ht_ctrl_t;

typedef struct {
  const char *dev;
  uint32_t hosts;
  uint32_t host_id;
  uint32_t threads;
  uint32_t buckets;
  uint64_t keys;
  uint64_t slab_mb;
  uint64_t shm_mb;
  int mix; /* read percentage 0..100 */
  double seconds;
  uint32_t sample; /* sample every Nth op (power of two) */
  int zipf;
  int init;
  uint64_t attach_timeout_s;
  uint64_t barrier_timeout_s;
} config_t;

typedef struct {
  uint64_t n;
  double theta;
  double alpha;
  double zetan;
  double eta;
  double half_pow_theta;
} zipf_t;

typedef struct {
  const config_t *cfg;
  shm_ht_t *ht;
  ht_ctrl_t *ctrl;
  const zipf_t *zipf;
  uint32_t thread_id;
  uint64_t start_ns;
  uint64_t duration_ns;
  uint64_t rng;
  /* outputs */
  uint64_t ops;
  uint64_t reads;
  uint64_t writes;
  uint64_t errs;
  uint64_t *samples;
  size_t sample_count;
} worker_t;

static inline uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* xorshift64* */
static inline uint64_t rng_next(uint64_t *s) {
  uint64_t x = *s;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *s = x;
  return x * 0x2545F4914F6CDD1DULL;
}

/* ---- percentiles (same approach as synchronization/lock_bench.c) --------*/

static int cmp_u64(const void *a, const void *b) {
  uint64_t aa = *(const uint64_t *)a;
  uint64_t bb = *(const uint64_t *)b;
  if (aa < bb) return -1;
  if (aa > bb) return 1;
  return 0;
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

/* ---- zipf (Gray et al. quick method, theta=0.99) -------------------------*/

static void zipf_init(zipf_t *z, uint64_t n, double theta) {
  z->n = n;
  z->theta = theta;
  z->alpha = 1.0 / (1.0 - theta);
  z->zetan = 0.0;
  for (uint64_t i = 1; i <= n; i++) {
    z->zetan += 1.0 / pow((double)i, theta);
  }
  double zeta2 = 1.0 + pow(0.5, theta);
  z->eta = (1.0 - pow(2.0 / (double)n, 1.0 - theta)) /
           (1.0 - zeta2 / z->zetan);
  z->half_pow_theta = pow(0.5, theta);
}

static inline uint64_t zipf_next(const zipf_t *z, uint64_t r) {
  double u = (double)(r >> 11) * (1.0 / 9007199254740992.0); /* [0,1) */
  double uz = u * z->zetan;
  if (uz < 1.0) {
    return 0;
  }
  if (uz < 1.0 + z->half_pow_theta) {
    return 1;
  }
  uint64_t v =
      (uint64_t)((double)z->n * pow(z->eta * u - z->eta + 1.0, z->alpha));
  return v >= z->n ? z->n - 1 : v;
}

/* ---- bounded barrier ------------------------------------------------------
 * Same shm_barrier_t (initialized with barrier_init()) as the rest of the
 * artifact, but the generation wait is reimplemented here with a deadline so
 * a dead peer produces a clean failure instead of a hang.
 */
static int timed_barrier(shm_barrier_t *b, uint32_t parties, uint32_t my_id,
                         uint64_t timeout_s, const char *tag) {
  if (parties <= 1) {
    return 0;
  }
  uint64_t deadline = now_ns() + timeout_s * 1000000000ull;

  shm_mutex_lock(&b->lock, (int)my_id, (int)parties);
  uint64_t my_gen = CACHELINE_LOAD(&b->gen);
  uint64_t c = CACHELINE_LOAD(&b->count) + 1;
  CACHELINE_STORE(&b->count, c);
  if (c == parties) {
    CACHELINE_STORE(&b->count, 0);
    CACHELINE_STORE(&b->gen, my_gen + 1);
    shm_mutex_unlock(&b->lock, (int)my_id);
    return 0;
  }
  shm_mutex_unlock(&b->lock, (int)my_id);

  while (CACHELINE_LOAD(&b->gen) == my_gen) {
    if (now_ns() >= deadline) {
      fprintf(stderr, "FATAL: barrier '%s' timed out after %lus on host %u "
                      "(peer dead?)\n",
              tag, timeout_s, my_id);
      return -ETIMEDOUT;
    }
    relax_cpu();
  }
  return 0;
}

/* ---- value payload --------------------------------------------------------
 * 64B payload layout: [0..7]=key echo  [8..15]=writer host  [16..23]=seq,
 * rest is a static per-writer pattern. Readers verify the key echo.
 */
static inline void fill_value(uint8_t *v, uint64_t key, uint64_t host,
                              uint64_t seq) {
  memcpy(v, &key, 8);
  memcpy(v + 8, &host, 8);
  memcpy(v + 16, &seq, 8);
}

static inline int value_echo_ok(const uint8_t *v, uint64_t key) {
  uint64_t echo;
  memcpy(&echo, v, 8);
  return echo == key;
}

static inline uint64_t check_key(const config_t *cfg, uint32_t host,
                                 uint32_t i) {
  return cfg->keys + (uint64_t)host * HT_CHECK_KEYS + i;
}

/* ---- worker ---------------------------------------------------------------*/

static void *worker_thread(void *arg) {
  worker_t *w = (worker_t *)arg;
  const config_t *cfg = w->cfg;
  shm_ht_t *ht = w->ht;
  cacheline_u64 *shared_slot =
      &w->ctrl->thread_ops[cfg->host_id][w->thread_id];

  uint8_t val[SHM_HT_VALUE_SIZE];
  for (size_t i = 0; i < sizeof(val); i++) {
    val[i] = (uint8_t)(0xA0u + w->thread_id + i);
  }

  const uint64_t end_ns = w->start_ns + w->duration_ns;
  const uint64_t sample_mask = (uint64_t)cfg->sample - 1;

  while (now_ns() < end_ns) {
    uint64_t r = rng_next(&w->rng);
    uint64_t key = cfg->zipf ? zipf_next(w->zipf, r) : (r % cfg->keys);
    bool is_read = (int)(rng_next(&w->rng) % 100) < cfg->mix;
    bool sampled =
        ((w->ops & sample_mask) == 0) && (w->sample_count < HT_SAMPLE_CAP);
    uint64_t t0 = sampled ? now_ns() : 0;

    if (is_read) {
      uint8_t out[SHM_HT_VALUE_SIZE];
      int rc = shm_ht_get(ht, key, out);
      if (rc != 0 || !value_echo_ok(out, key)) {
        w->errs++;
      }
      w->reads++;
    } else {
      fill_value(val, key, cfg->host_id, w->ops);
      int rc = shm_ht_put(ht, key, val);
      if (rc < 0) {
        w->errs++;
      }
      w->writes++;
    }

    if (sampled) {
      w->samples[w->sample_count++] = now_ns() - t0;
    }
    w->ops++;
    if ((w->ops & 4095) == 0) {
      /* periodic flush-through publish of the per-thread op counter */
      CACHELINE_STORE(shared_slot, w->ops);
    }
  }

  CACHELINE_STORE(shared_slot, w->ops);
  return NULL;
}

/* ---- setup helpers --------------------------------------------------------*/

static ht_ctrl_t *ctrl_create(const config_t *cfg) {
  ht_ctrl_t *ctrl = shm_malloc_id(HT_ID_CTRL, sizeof(ht_ctrl_t));
  if (!ctrl) {
    fprintf(stderr, "FATAL: alloc control block failed: %s\n",
            strerror(errno));
    return NULL;
  }
  memset(ctrl, 0, sizeof(*ctrl));
  flush_region(ctrl, sizeof(*ctrl));
  if (barrier_init(&ctrl->barrier, cfg->hosts, cfg->host_id) != 0) {
    fprintf(stderr, "FATAL: barrier_init failed\n");
    return NULL;
  }
  CACHELINE_STORE(&ctrl->magic, HT_CTRL_MAGIC);
  return ctrl;
}

static ht_ctrl_t *ctrl_attach(const config_t *cfg) {
  uint64_t deadline =
      now_ns() + cfg->attach_timeout_s * 1000000000ull;
  ht_ctrl_t *ctrl = NULL;
  while (!ctrl) {
    ctrl = shm_attach_id(HT_ID_CTRL, sizeof(ht_ctrl_t));
    if (ctrl) {
      break;
    }
    if (errno != ENOENT && errno != EINVAL) {
      fprintf(stderr, "FATAL: attach control block failed: %s\n",
              strerror(errno));
      return NULL;
    }
    if (now_ns() >= deadline) {
      fprintf(stderr, "FATAL: control block attach timed out\n");
      return NULL;
    }
    usleep(1000);
  }
  while (CACHELINE_LOAD(&ctrl->magic) != HT_CTRL_MAGIC) {
    if (now_ns() >= deadline) {
      fprintf(stderr, "FATAL: control block magic never published\n");
      return NULL;
    }
    usleep(1000);
  }
  if (barrier_init(&ctrl->barrier, cfg->hosts, cfg->host_id) != 0) {
    fprintf(stderr, "FATAL: barrier_init failed\n");
    return NULL;
  }
  return ctrl;
}

static int preload(shm_ht_t *ht, const config_t *cfg) {
  uint8_t val[SHM_HT_VALUE_SIZE];
  memset(val, 0x5A, sizeof(val));
  uint64_t t0 = now_ns();
  for (uint64_t k = 0; k < cfg->keys; k++) {
    fill_value(val, k, cfg->host_id, 0);
    int rc = shm_ht_put(ht, k, val);
    if (rc < 0) {
      fprintf(stderr, "FATAL: preload key %lu failed: %s\n", k,
              strerror(-rc));
      return -1;
    }
  }
  double secs = (double)(now_ns() - t0) / 1e9;
  printf("preload: %lu keys in %.2f s (slab_used_mb=%.1f)\n", cfg->keys, secs,
         (double)shm_ht_slab_used(ht) / (1024.0 * 1024.0));
  fflush(stdout);
  return 0;
}

/* Self-check phase 1: sampled preloaded keys readable + shared per-thread
 * op counters round-trip to the values counted locally; then insert this
 * host's check keys. */
static uint64_t self_check_local(shm_ht_t *ht, const config_t *cfg,
                                 ht_ctrl_t *ctrl, worker_t *workers) {
  uint64_t errs = 0;

  for (uint32_t t = 0; t < cfg->threads; t++) {
    uint64_t shared =
        CACHELINE_LOAD(&ctrl->thread_ops[cfg->host_id][t]);
    if (shared != workers[t].ops) {
      fprintf(stderr, "self-check: thread %u shared ops %lu != local %lu\n",
              t, shared, workers[t].ops);
      errs++;
    }
  }

  uint64_t step = cfg->keys / HT_READ_SAMPLE;
  if (step == 0) {
    step = 1;
  }
  uint8_t out[SHM_HT_VALUE_SIZE];
  for (uint64_t k = 0; k < cfg->keys; k += step) {
    if (shm_ht_get(ht, k, out) != 0 || !value_echo_ok(out, k)) {
      fprintf(stderr, "self-check: preloaded key %lu unreadable/corrupt\n", k);
      errs++;
    }
  }

  uint8_t val[SHM_HT_VALUE_SIZE];
  memset(val, 0xC3, sizeof(val));
  for (uint32_t i = 0; i < HT_CHECK_KEYS; i++) {
    uint64_t k = check_key(cfg, cfg->host_id, i);
    fill_value(val, k, cfg->host_id, i);
    if (shm_ht_put(ht, k, val) < 0) {
      fprintf(stderr, "self-check: insert check key %lu failed\n", k);
      errs++;
    }
  }
  return errs;
}

/* Self-check phase 2 (after barrier): every host must observe every other
 * host's check keys — cross-host visibility through the flush protocol. */
static uint64_t self_check_cross(shm_ht_t *ht, const config_t *cfg) {
  uint64_t errs = 0;
  uint8_t out[SHM_HT_VALUE_SIZE];
  for (uint32_t h = 0; h < cfg->hosts; h++) {
    for (uint32_t i = 0; i < HT_CHECK_KEYS; i++) {
      uint64_t k = check_key(cfg, h, i);
      if (shm_ht_get(ht, k, out) != 0 || !value_echo_ok(out, k)) {
        fprintf(stderr, "self-check: host %u check key %lu missing/corrupt\n",
                h, k);
        errs++;
      }
    }
  }
  return errs;
}

static void usage(const char *prog) {
  printf("Usage: %s [OPTIONS]\n\n", prog);
  printf("  --dev=<PATH>           dax device or file (default /dev/dax0.0;\n");
  printf("                         mock mode: a tmpfs file, e.g. /dev/shm/x)\n");
  printf("  --hosts=<N>            participating hosts (default 1, max %d)\n",
         MAX_HOST_NUM);
  printf("  --host_id=<I>          this host's id in [0, hosts)\n");
  printf("  --threads=<T>          worker threads (default 8, max %d)\n",
         HT_MAX_THREADS);
  printf("  --buckets=<N>          bucket count, power of two (default 2^20)\n");
  printf("  --slab-mb=<N>          per-host node slab MB (default 256)\n");
  printf("  --shm-mb=<N>           shared mapping size MB (default 4096)\n");
  printf("  --mix=<P>              read percentage 0..100 (default 95)\n");
  printf("  --seconds=<S>          run duration (default 10)\n");
  printf("  --keys=<N>             keyspace, preloaded by host 0 (default 1M)\n");
  printf("  --zipf                 zipfian key distribution (default uniform)\n");
  printf("  --sample=<N>           sample every Nth op latency, pow2 (def 32)\n");
  printf("  --init                 host 0 only: reset + create + preload\n");
  printf("  --attach-timeout=<S>   attach poll cap in seconds (default 180)\n");
  printf("  --barrier-timeout=<S>  barrier wait cap in seconds (default 600)\n");
  printf("  --help\n");
}

int main(int argc, char **argv) {
  config_t cfg = {
      .dev = "/dev/dax0.0",
      .hosts = 1,
      .host_id = 0,
      .threads = 8,
      .buckets = 1u << 20,
      .keys = 1000000,
      .slab_mb = 256,
      .shm_mb = 4096,
      .mix = 95,
      .seconds = 10.0,
      .sample = 32,
      .zipf = 0,
      .init = 0,
      .attach_timeout_s = 180,
      .barrier_timeout_s = 600,
  };

  enum {
    OPT_DEV = 1000,
    OPT_HOSTS,
    OPT_HOST_ID,
    OPT_THREADS,
    OPT_BUCKETS,
    OPT_SLAB_MB,
    OPT_SHM_MB,
    OPT_MIX,
    OPT_SECONDS,
    OPT_KEYS,
    OPT_ZIPF,
    OPT_SAMPLE,
    OPT_INIT,
    OPT_ATTACH_TO,
    OPT_BARRIER_TO,
  };
  static struct option opts[] = {
      {"dev", required_argument, NULL, OPT_DEV},
      {"hosts", required_argument, NULL, OPT_HOSTS},
      {"host_id", required_argument, NULL, OPT_HOST_ID},
      {"threads", required_argument, NULL, OPT_THREADS},
      {"buckets", required_argument, NULL, OPT_BUCKETS},
      {"slab-mb", required_argument, NULL, OPT_SLAB_MB},
      {"shm-mb", required_argument, NULL, OPT_SHM_MB},
      {"mix", required_argument, NULL, OPT_MIX},
      {"seconds", required_argument, NULL, OPT_SECONDS},
      {"keys", required_argument, NULL, OPT_KEYS},
      {"zipf", no_argument, NULL, OPT_ZIPF},
      {"sample", required_argument, NULL, OPT_SAMPLE},
      {"init", no_argument, NULL, OPT_INIT},
      {"attach-timeout", required_argument, NULL, OPT_ATTACH_TO},
      {"barrier-timeout", required_argument, NULL, OPT_BARRIER_TO},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0},
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
    switch (opt) {
    case OPT_DEV: cfg.dev = optarg; break;
    case OPT_HOSTS: cfg.hosts = (uint32_t)strtoul(optarg, NULL, 0); break;
    case OPT_HOST_ID: cfg.host_id = (uint32_t)strtoul(optarg, NULL, 0); break;
    case OPT_THREADS: cfg.threads = (uint32_t)strtoul(optarg, NULL, 0); break;
    case OPT_BUCKETS: cfg.buckets = (uint32_t)strtoul(optarg, NULL, 0); break;
    case OPT_SLAB_MB: cfg.slab_mb = strtoull(optarg, NULL, 0); break;
    case OPT_SHM_MB: cfg.shm_mb = strtoull(optarg, NULL, 0); break;
    case OPT_MIX: cfg.mix = atoi(optarg); break;
    case OPT_SECONDS: cfg.seconds = strtod(optarg, NULL); break;
    case OPT_KEYS: cfg.keys = strtoull(optarg, NULL, 0); break;
    case OPT_ZIPF: cfg.zipf = 1; break;
    case OPT_SAMPLE: cfg.sample = (uint32_t)strtoul(optarg, NULL, 0); break;
    case OPT_INIT: cfg.init = 1; break;
    case OPT_ATTACH_TO: cfg.attach_timeout_s = strtoull(optarg, NULL, 0); break;
    case OPT_BARRIER_TO: cfg.barrier_timeout_s = strtoull(optarg, NULL, 0); break;
    case 'h': usage(argv[0]); return 0;
    default: usage(argv[0]); return 1;
    }
  }

  /* ---- validate ---- */
  if (cfg.hosts == 0 || cfg.hosts > MAX_HOST_NUM || cfg.host_id >= cfg.hosts) {
    fprintf(stderr, "Error: need 1 <= --hosts <= %d and --host_id < hosts\n",
            MAX_HOST_NUM);
    return 1;
  }
  if (cfg.threads == 0 || cfg.threads > HT_MAX_THREADS) {
    fprintf(stderr, "Error: need 1 <= --threads <= %d\n", HT_MAX_THREADS);
    return 1;
  }
  if (cfg.buckets < 16 || (cfg.buckets & (cfg.buckets - 1)) != 0) {
    fprintf(stderr, "Error: --buckets must be a power of two >= 16\n");
    return 1;
  }
  if (cfg.mix < 0 || cfg.mix > 100) {
    fprintf(stderr, "Error: --mix must be in [0,100]\n");
    return 1;
  }
  if (cfg.seconds <= 0.0 || cfg.keys == 0 || cfg.slab_mb == 0 ||
      cfg.shm_mb == 0) {
    fprintf(stderr, "Error: --seconds/--keys/--slab-mb/--shm-mb must be > 0\n");
    return 1;
  }
  if (cfg.sample == 0 || (cfg.sample & (cfg.sample - 1)) != 0) {
    fprintf(stderr, "Error: --sample must be a power of two\n");
    return 1;
  }
  if (cfg.init && cfg.host_id != 0) {
    fprintf(stderr, "Error: --init is only valid on host 0\n");
    return 1;
  }
  uint64_t slab_bytes = cfg.slab_mb << 20;
  if (cfg.init &&
      (cfg.keys + HT_CHECK_KEYS + 2) * sizeof(ht_node_t) > slab_bytes) {
    fprintf(stderr,
            "Error: host 0 slab too small for preload: need %lu MB, have %lu "
            "MB (raise --slab-mb)\n",
            ((cfg.keys + HT_CHECK_KEYS + 2) * sizeof(ht_node_t)) >> 20,
            cfg.slab_mb);
    return 1;
  }

  /* ---- shared memory ---- */
  struct shm_arg arg = {
      .host_id = cfg.host_id,
      .host_num = cfg.hosts,
      .flags = cfg.init ? SHM_ENABLE_RESET : 0,
      .map_size = (size_t)cfg.shm_mb << 20,
  };
  snprintf(arg.dax_dev, sizeof(arg.dax_dev), "%s", cfg.dev);
  int rc = shm_enable(arg);
  if (rc < 0) {
    fprintf(stderr, "FATAL: shm_enable(%s) failed: %s\n", cfg.dev,
            strerror(-rc));
    return 1;
  }

  shm_ht_t ht;
  ht_ctrl_t *ctrl;
  if (cfg.init) {
    rc = shm_ht_create(&ht, cfg.buckets, cfg.hosts, cfg.host_id, slab_bytes);
    if (rc != 0) {
      fprintf(stderr, "FATAL: shm_ht_create failed: %s\n", strerror(-rc));
      return 1;
    }
    ctrl = ctrl_create(&cfg);
    if (!ctrl) {
      return 1;
    }
    if (preload(&ht, &cfg) != 0) {
      return 1;
    }
  } else {
    rc = shm_ht_attach(&ht, cfg.hosts, cfg.host_id,
                       cfg.attach_timeout_s * 1000ull);
    if (rc != 0) {
      fprintf(stderr, "FATAL: shm_ht_attach failed: %s\n", strerror(-rc));
      return 1;
    }
    ctrl = ctrl_attach(&cfg);
    if (!ctrl) {
      return 1;
    }
  }

  /* ---- zipf table (once per process) ---- */
  zipf_t zipf;
  if (cfg.zipf) {
    zipf_init(&zipf, cfg.keys, ZIPF_THETA);
  }

  /* ---- rendezvous: host 0 arrives only after the preload ---- */
  if (timed_barrier(&ctrl->barrier, cfg.hosts, cfg.host_id,
                    cfg.barrier_timeout_s, "start") != 0) {
    return 1;
  }

  /* ---- timed run ---- */
  worker_t *workers = calloc(cfg.threads, sizeof(worker_t));
  pthread_t *tids = calloc(cfg.threads, sizeof(pthread_t));
  if (!workers || !tids) {
    fprintf(stderr, "FATAL: worker alloc failed\n");
    return 1;
  }
  uint64_t start_ns = now_ns();
  uint64_t duration_ns = (uint64_t)(cfg.seconds * 1e9);
  for (uint32_t t = 0; t < cfg.threads; t++) {
    workers[t].cfg = &cfg;
    workers[t].ht = &ht;
    workers[t].ctrl = ctrl;
    workers[t].zipf = cfg.zipf ? &zipf : NULL;
    workers[t].thread_id = t;
    workers[t].start_ns = start_ns;
    workers[t].duration_ns = duration_ns;
    workers[t].rng =
        shm_ht_hash(((uint64_t)cfg.host_id << 32) | (t + 1) | 1);
    workers[t].samples = calloc(HT_SAMPLE_CAP, sizeof(uint64_t));
    if (!workers[t].samples) {
      fprintf(stderr, "FATAL: sample buffer alloc failed\n");
      return 1;
    }
    pthread_create(&tids[t], NULL, worker_thread, &workers[t]);
  }
  for (uint32_t t = 0; t < cfg.threads; t++) {
    pthread_join(tids[t], NULL);
  }
  uint64_t elapsed_ns = now_ns() - start_ns;

  /* ---- everyone stops before the self-check reads ---- */
  if (timed_barrier(&ctrl->barrier, cfg.hosts, cfg.host_id,
                    cfg.barrier_timeout_s, "run-end") != 0) {
    return 1;
  }

  uint64_t total_ops = 0, total_reads = 0, total_writes = 0, errs = 0;
  for (uint32_t t = 0; t < cfg.threads; t++) {
    total_ops += workers[t].ops;
    total_reads += workers[t].reads;
    total_writes += workers[t].writes;
    errs += workers[t].errs;
  }

  errs += self_check_local(&ht, &cfg, ctrl, workers);

  if (timed_barrier(&ctrl->barrier, cfg.hosts, cfg.host_id,
                    cfg.barrier_timeout_s, "check-mid") != 0) {
    return 1;
  }

  errs += self_check_cross(&ht, &cfg);

  /* ---- publish per-host results, then final rendezvous ---- */
  CACHELINE_STORE(&ctrl->host_ops[cfg.host_id], total_ops);
  CACHELINE_STORE(&ctrl->host_elapsed_ns[cfg.host_id], elapsed_ns);
  CACHELINE_STORE(&ctrl->host_errs[cfg.host_id], errs);
  if (timed_barrier(&ctrl->barrier, cfg.hosts, cfg.host_id,
                    cfg.barrier_timeout_s, "final") != 0) {
    return 1;
  }

  /* ---- local latency percentiles (lock_bench-style qsort) ---- */
  size_t nsamples = 0;
  for (uint32_t t = 0; t < cfg.threads; t++) {
    nsamples += workers[t].sample_count;
  }
  uint64_t *all = calloc(nsamples ? nsamples : 1, sizeof(uint64_t));
  if (!all) {
    fprintf(stderr, "FATAL: sample merge alloc failed\n");
    return 1;
  }
  size_t off = 0;
  for (uint32_t t = 0; t < cfg.threads; t++) {
    memcpy(all + off, workers[t].samples,
           workers[t].sample_count * sizeof(uint64_t));
    off += workers[t].sample_count;
  }
  qsort(all, nsamples, sizeof(uint64_t), cmp_u64);
  uint64_t p50 = percentile_u64(all, nsamples, 0.50);
  uint64_t p95 = percentile_u64(all, nsamples, 0.95);
  uint64_t p99 = percentile_u64(all, nsamples, 0.99);

  double elapsed_s = (double)elapsed_ns / 1e9;
  double mops = elapsed_s > 0.0 ? (double)total_ops / elapsed_s / 1e6 : 0.0;

  printf("\nHOST %u RESULTS\n", cfg.host_id);
  printf("dev=%s hosts=%u host_id=%u threads=%u buckets=%u keys=%lu mix=%d "
         "zipf=%d seconds=%.2f sample=%u\n",
         cfg.dev, cfg.hosts, cfg.host_id, cfg.threads, cfg.buckets, cfg.keys,
         cfg.mix, cfg.zipf, cfg.seconds, cfg.sample);
  printf("elapsed_s: %.3f\n", elapsed_s);
  printf("ops: %lu (reads=%lu writes=%lu)\n", total_ops, total_reads,
         total_writes);
  printf("mops: %.3f\n", mops);
  printf("lat_samples: %zu\n", nsamples);
  printf("lat p50/p95/p99 ns: %lu / %lu / %lu\n", p50, p95, p99);
  printf("slab_used_mb: %.1f\n",
         (double)shm_ht_slab_used(&ht) / (1024.0 * 1024.0));
  printf("error_count: %lu\n", errs);

  uint64_t errors_all = errs;
  if (cfg.host_id == 0) {
    double agg_mops = 0.0;
    uint64_t agg_ops = 0;
    errors_all = 0;
    for (uint32_t h = 0; h < cfg.hosts; h++) {
      uint64_t ops_h = CACHELINE_LOAD(&ctrl->host_ops[h]);
      uint64_t el_h = CACHELINE_LOAD(&ctrl->host_elapsed_ns[h]);
      uint64_t er_h = CACHELINE_LOAD(&ctrl->host_errs[h]);
      agg_ops += ops_h;
      errors_all += er_h;
      if (el_h > 0) {
        agg_mops += (double)ops_h * 1000.0 / (double)el_h;
      }
    }
    printf("AGGREGATE mops=%.3f p50_ns=%lu p95_ns=%lu p99_ns=%lu "
           "total_ops=%lu hosts=%u errors_all=%lu\n",
           agg_mops, p50, p95, p99, agg_ops, cfg.hosts, errors_all);
  }
  fflush(stdout);

  for (uint32_t t = 0; t < cfg.threads; t++) {
    free(workers[t].samples);
  }
  free(all);
  free(workers);
  free(tids);
  shm_ht_close(&ht);

  return (errs > 0 || errors_all > 0) ? 1 : 0;
}
