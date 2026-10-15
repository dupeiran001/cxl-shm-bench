#define _GNU_SOURCE

/**
 * @file rtt.c
 * @brief RTT latency benchmark for non-coherent CXL/DAX memory devices.
 */

#include <fcntl.h>
#include <getopt.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>
#include <xmmintrin.h>

#define DEFAULT_DAX_DEVICE "/dev/interleave_dax"
#define CACHE_LINE_SIZE 64

#define DAX_MAP_SIZE (2 * 1024 * 1024)

// Client start signal. It must differ from every sequence number the client
// later acknowledges, or the server can take it for ack 1, publish 2 before
// the client has seen 1, and both sides wait forever.
#define CLIENT_HELLO UINT64_MAX

// --- Shared Memory Structures ---

typedef struct {
  volatile uint64_t to_client_flag;
  uint8_t padding1[CACHE_LINE_SIZE - sizeof(uint64_t)];
  volatile uint64_t to_server_flag;
  uint8_t padding2[CACHE_LINE_SIZE - sizeof(uint64_t)];
} __attribute__((aligned(CACHE_LINE_SIZE))) latency_layout_t;

// --- Configuration Structure ---
typedef enum { MODE_SERVER, MODE_CLIENT } run_mode_t;

typedef struct {
  run_mode_t mode;
  uint64_t iterations;
  void *mapped_mem;
  size_t mapped_size;
  int use_clwb;
  size_t mmap_offset;
  int use_pause;
  const char *dax_dev;
} config_t;

// --- Function Prototypes / Forward Declarations ---
void run_latency_test(config_t *config);

void pin_to_core(int core_id) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0) {
    perror("sched_setaffinity");
    exit(EXIT_FAILURE);
  }
}

inline void flush_line_for_write(volatile void *p, const config_t *config) {
  if (config->use_clwb) {
    _mm_clwb((void *)p);
  } else {
    _mm_clflushopt((void *)p);
  }
}

// Reader side of every flag poll: drop the local copy of the line, wait for
// the flush to complete (mfence; lfence does not order a later load after
// clflushopt), then load.
static inline uint64_t observe_flag(volatile uint64_t *flag) {
  _mm_clflushopt((void *)flag);
  _mm_mfence();
  return *flag;
}

static inline uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// --- TEST IMPLEMENTATIONS ---

void run_latency_test(config_t *config) {
  latency_layout_t *layout = (latency_layout_t *)config->mapped_mem;
  if (config->mode == MODE_SERVER) {
    pin_to_core(0);
    layout->to_client_flag = 0;
    layout->to_server_flag = 0;
    flush_line_for_write((void *)&layout->to_client_flag, config);
    flush_line_for_write((void *)&layout->to_server_flag, config);
    _mm_sfence();
    printf("Latency Server (Core 0): Waiting for client to start...\n");
    while (observe_flag(&layout->to_server_flag) != CLIENT_HELLO) {
      if (config->use_pause) {
        _mm_pause();
      }
    }
    printf("Client started. Running latency test for %lu iterations...\n",
           config->iterations);
    uint64_t publish_ns_total = 0;
    uint64_t visibility_ns_total = 0;
    uint64_t poll_loops_total = 0;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (uint64_t i = 1; i <= config->iterations; ++i) {
      uint64_t pub_start = now_ns();
      layout->to_client_flag = i;
      flush_line_for_write((void *)&layout->to_client_flag, config);
      _mm_mfence();
      uint64_t pub_end = now_ns();
      publish_ns_total += (pub_end - pub_start);
      uint64_t loops = 0;
      uint64_t vis_start = now_ns();
      while (observe_flag(&layout->to_server_flag) != i) {
        if (config->use_pause) {
          _mm_pause();
        }
        loops++;
      }
      uint64_t vis_end = now_ns();
      visibility_ns_total += (vis_end - vis_start);
      poll_loops_total += loops;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double duration_sec =
        (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    double rtt_ns = (duration_sec / config->iterations) * 1e9;
    double avg_publish_ns =
        (double)publish_ns_total / (double)config->iterations;
    double avg_visibility_ns =
        (double)visibility_ns_total / (double)config->iterations;
    double avg_poll_loops =
        (double)poll_loops_total / (double)config->iterations;
    printf("\n--- Latency Test Results ---\n");
    printf("Total iterations: %lu\n", config->iterations);
    printf("Total time:       %.4f seconds\n", duration_sec);
    printf("Average RTT:      %.2f nanoseconds\n", rtt_ns);
    printf("Average One-Way:  %.2f nanoseconds\n", rtt_ns / 2.0);
    printf("Publish latency:  %.2f nanoseconds\n", avg_publish_ns);
    printf("Visibility lat.:  %.2f nanoseconds\n", avg_visibility_ns);
    printf("Avg poll loops:   %.2f\n", avg_poll_loops);
    printf("---------------------------\n");
  } else { // MODE_CLIENT
    pin_to_core(1);
    printf("Latency Client (Core 1): Starting test...\n");
    layout->to_server_flag = CLIENT_HELLO;
    flush_line_for_write((void *)&layout->to_server_flag, config);
    _mm_sfence();
    uint64_t publish_ns_total = 0;
    uint64_t visibility_ns_total = 0;
    uint64_t poll_loops_total = 0;
    for (uint64_t i = 1; i <= config->iterations; ++i) {
      uint64_t loops = 0;
      uint64_t vis_start = now_ns();
      while (observe_flag(&layout->to_client_flag) != i) {
        if (config->use_pause) {
          _mm_pause();
        }
        loops++;
      }
      uint64_t vis_end = now_ns();
      visibility_ns_total += (vis_end - vis_start);
      poll_loops_total += loops;
      uint64_t pub_start = now_ns();
      layout->to_server_flag = i;
      flush_line_for_write((void *)&layout->to_server_flag, config);
      _mm_mfence();
      uint64_t pub_end = now_ns();
      publish_ns_total += (pub_end - pub_start);
    }
    if (config->iterations > 0) {
      double avg_publish_ns =
          (double)publish_ns_total / (double)config->iterations;
      double avg_visibility_ns =
          (double)visibility_ns_total / (double)config->iterations;
      double avg_poll_loops =
          (double)poll_loops_total / (double)config->iterations;
      printf("\n--- Client Visibility/Publish ---\n");
      printf("Publish latency:  %.2f nanoseconds\n", avg_publish_ns);
      printf("Visibility lat.:  %.2f nanoseconds\n", avg_visibility_ns);
      printf("Avg poll loops:   %.2f\n", avg_poll_loops);
      printf("-------------------------------\n");
    }
    printf("Client finished.\n");
  }
}

// --- MAIN FUNCTION & ARG PARSING ---

void print_usage(const char *prog_name) {
  printf("Usage: %s --mode=<server|client> [OPTIONS]\n\n", prog_name);
  printf("Required Arguments:\n");
  printf("  --mode=<server|client>       Run as a server (reader) or client "
         "(writer).\n\n");
  printf("Optional Arguments:\n");
  printf("  --iter=<N>                   Total operations "
         "(default: 1,000,000).\n");
  printf("  --offset=<N>                 Byte offset into DAX mmap "
         "(default: 0).\n");
  printf("  --dax_dev=<path>             DAX device path (default: %s).\n",
         DEFAULT_DAX_DEVICE);
  printf(
      "  --no-pause                   Disable _mm_pause() in polling loops.\n");
  printf("  --clwb                       Use clwb for write flushes instead of "
         "clflushopt.\n");
  printf("  --help                       Print this message.\n\n");
}

int main(int argc, char **argv) {
  config_t config = {.iterations = 1000000,
                     .use_clwb = 0,
                     .mmap_offset = 0,
                     .use_pause = 1,
                     .dax_dev = DEFAULT_DAX_DEVICE};
  int mode_set = 0;

  enum { CLWB_OPTION = 256, NO_PAUSE_OPTION = 257, DAX_DEV_OPTION = 258 };

  const struct option long_opts[] = {
      {"mode", required_argument, NULL, 'm'},
      {"iter", required_argument, NULL, 'i'},
      {"offset", required_argument, NULL, 'o'},
      {"dax_dev", required_argument, NULL, DAX_DEV_OPTION},
      {"clwb", no_argument, NULL, CLWB_OPTION},
      {"no-pause", no_argument, NULL, NO_PAUSE_OPTION},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "m:i:o:h", long_opts, NULL)) != -1) {
    switch (opt) {
    case 'm':
      if (strcmp(optarg, "server") == 0)
        config.mode = MODE_SERVER;
      else if (strcmp(optarg, "client") == 0)
        config.mode = MODE_CLIENT;
      else {
        fprintf(stderr, "Invalid mode '%s'\n", optarg);
        print_usage(argv[0]);
        return 1;
      }
      mode_set = 1;
      break;
    case 'i':
      config.iterations = atol(optarg);
      break;
    case 'o': {
      unsigned long long off = strtoull(optarg, NULL, 0);
      config.mmap_offset = (size_t)off;
      break;
    }
    case DAX_DEV_OPTION:
      config.dax_dev = optarg;
      break;
    case CLWB_OPTION:
      config.use_clwb = 1;
      break;
    case NO_PAUSE_OPTION:
      config.use_pause = 0;
      break;
    case 'h':
      print_usage(argv[0]);
      return 0;
    default:
      print_usage(argv[0]);
      return 1;
    }
  }

  if (!mode_set) {
    fprintf(stderr, "Error: --mode is required.\n\n");
    print_usage(argv[0]);
    return 1;
  }

  int fd = open(config.dax_dev, O_RDWR);
  if (fd < 0) {
    fprintf(stderr, "Failed to open DAX device '%s': ", config.dax_dev);
    perror("");
    return 1;
  }

  size_t required_size = sizeof(latency_layout_t);

  if (required_size > DAX_MAP_SIZE) {
    fprintf(stderr,
            "Error: Required memory layout (%zu bytes) exceeds mapping size "
            "(%d bytes).\n",
            required_size, DAX_MAP_SIZE);
    close(fd);
    return 1;
  }

  if (config.mmap_offset % 4096 != 0) {
    fprintf(stderr, "Error: --offset must be 4KB aligned.\n");
    close(fd);
    return 1;
  }
  config.mapped_mem = mmap(NULL, DAX_MAP_SIZE, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, (off_t)config.mmap_offset);
  if (config.mapped_mem == MAP_FAILED) {
    perror("Failed to mmap DAX device");
    close(fd);
    return 1;
  }
  config.mapped_size = DAX_MAP_SIZE;
  printf("Successfully mapped %zu bytes from %s at offset %zu\n",
         config.mapped_size, config.dax_dev, config.mmap_offset);

  run_latency_test(&config);

  munmap(config.mapped_mem, config.mapped_size);
  close(fd);
  return 0;
}
