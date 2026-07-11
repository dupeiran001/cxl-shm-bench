/*
 * bw_mem.c - simple memory write bandwidth benchmark
 *
 * Usage: bw_mem [-P <parallelism>] [-W <warmup>] [-N <repetitions>] size what
 *        what: rd wr rdwr cp fwr frd fcp bzero bcopy
 *
 * Copyright (c) 1994-1996 Larry McVoy.  Distributed under the FSF GPL with
 * additional restriction that results may published only if
 * (1) the benchmark is unmodified, and
 * (2) the version in the sccsid below is included in the report.
 * Support for this development by Sun Microsystems is gratefully acknowledged.
 */
char *id = "$Id$";
#include <errno.h>
#include <numa.h>
#include <string.h>
#include <x86intrin.h>

#include "lmbench/bench.h"
#include "lmbench/lmbench_mem.h"

#define TYPE int

#define NO_NUMA_MEMORY (-1)
#define CACHE_LINESIZE (64)
/*
 * rd - 4 byte read, 32 byte stride
 * wr - 4 byte write, 32 byte stride
 * rdwr - 4 byte read followed by 4 byte write to same place, 32 byte stride
 * cp - 4 byte read then 4 byte write to different place, 32 byte stride
 * fwr - write every 4 byte word
 * frd - read every 4 byte word
 * fcp - copy every 4 byte word
 *
 * All tests do 512 byte chunks in a loop.
 *
 * XXX - do a 64bit version of this.
 */
static void rd(iter_t iterations, void *cookie);
static void wr(iter_t iterations, void *cookie);
static void ntwr(iter_t iterations, void *cookie);
static void rdwr(iter_t iterations, void *cookie);
static void mcp(iter_t iterations, void *cookie);
static void fwr(iter_t iterations, void *cookie);
static void frd(iter_t iterations, void *cookie);
static void fcp(iter_t iterations, void *cookie);
static void loop_bzero(iter_t iterations, void *cookie);
static void loop_bcopy(iter_t iterations, void *cookie);
static void init_overhead(iter_t iterations, void *cookie);
static void init_loop(iter_t iterations, void *cookie);
static void cleanup(iter_t iterations, void *cookie);

typedef struct _state {
  double overhead;
  size_t nbytes;
  int need_buf2;
  int aligned;
  TYPE *buf;
  TYPE *buf2;
  TYPE *buf2_orig;
  TYPE *lastone;
  size_t N;
  int numa_node;
  const char *dax_path;
  uint64_t dax_offset;
  int dax_fd;
  off_t dax_next_off;
  void *buf_map;
  size_t buf_map_len;
  void *buf2_map;
  size_t buf2_map_len;
} state_t;

static void adjusted_bandwidth(uint64 t, uint64 b, uint64 iter, double ovrhd);

static size_t align_up(size_t size, size_t align) {
  return (size + align - 1) & ~(align - 1);
}

static void *map_dax_region(int fd, size_t map_len, off_t offset) {
  void *addr = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                    offset);
  if (addr == MAP_FAILED) {
    return NULL;
  }
  return addr;
}

int bw_mem(int parallel, int warmup, int repetitions, uint64_t size,
           int aligned, enum lmbench_mem_t work_type, int numa_node,
           const char *dax_dev, uint64_t dax_offset) {
  size_t nbytes;
  state_t state;

  state.overhead = 0;

  state.numa_node = numa_node;
  state.dax_path = dax_dev;
  state.dax_offset = dax_offset;
  state.dax_fd = -1;
  state.dax_next_off = 0;
  state.buf_map = NULL;
  state.buf_map_len = 0;
  state.buf2_map = NULL;
  state.buf2_map_len = 0;

  /* should have two, possibly three [indicates align] arguments left */
  state.aligned = state.need_buf2 = 0;

  state.aligned = aligned;

  nbytes = state.nbytes = size;

  if (work_type == CP || work_type == FCP || work_type == BCOPY) {
    state.need_buf2 = 1;
  }

  if (work_type == READ) {
    benchmp(init_loop, rd, cleanup, 0, parallel, warmup, repetitions, &state);
  } else if (work_type == WRITE) {
    benchmp(init_loop, wr, cleanup, 0, parallel, warmup, repetitions, &state);
  } else if (work_type == RDWR) {
    benchmp(init_loop, rdwr, cleanup, 0, parallel, warmup, repetitions, &state);
  } else if (work_type == CP) {
    benchmp(init_loop, mcp, cleanup, 0, parallel, warmup, repetitions, &state);
  } else if (work_type == FRD) {
    benchmp(init_loop, frd, cleanup, 0, parallel, warmup, repetitions, &state);
  } else if (work_type == FWR) {
    benchmp(init_loop, fwr, cleanup, 0, parallel, warmup, repetitions, &state);
  } else if (work_type == FCP) {
    benchmp(init_loop, fcp, cleanup, 0, parallel, warmup, repetitions, &state);
  } else if (work_type == BZERO) {
    benchmp(init_loop, loop_bzero, cleanup, 0, parallel, warmup, repetitions,
            &state);
  } else if (work_type == BCOPY) {
    benchmp(init_loop, loop_bcopy, cleanup, 0, parallel, warmup, repetitions,
            &state);
  } else if (work_type == NTWRITE) {
    benchmp(init_loop, ntwr, cleanup, 0, parallel, warmup, repetitions, &state);
  }
  show_bandwidth(gettime(), nbytes, get_n() * parallel, state.overhead);
  return (0);
}

static void init_overhead(iter_t iterations, void *cookie) {
  state_t *state = (state_t *)cookie;
}

static void init_loop(iter_t iterations, void *cookie) {
  state_t *state = (state_t *)cookie;

  if (iterations) return;

  const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
  size_t buf_len = 0;
  size_t buf2_len = 0;
  size_t per_child_span = 0;
  if (state->dax_path && state->dax_path[0] != '\0') {
    buf_len = align_up(state->nbytes, page_size);
    buf2_len =
        state->need_buf2 ? align_up(state->nbytes + 2048, page_size) : 0;
    per_child_span = buf_len + buf2_len;
    if (state->dax_offset % page_size != 0) {
      fprintf(stderr, "dax_offset must be page-aligned (page size %zu)\n",
              page_size);
      exit(1);
    }
    if (state->dax_fd < 0) {
      state->dax_fd = open(state->dax_path, O_RDWR);
      if (state->dax_fd < 0) {
        fprintf(stderr, "open dax device %s failed: %s\n", state->dax_path,
                strerror(errno));
        exit(1);
      }
    }
    state->dax_next_off =
        (off_t)state->dax_offset + (off_t)benchmp_childid() *
                                       (off_t)per_child_span;
    state->buf_map_len = buf_len;
    state->buf_map = map_dax_region(state->dax_fd, state->buf_map_len,
                                    state->dax_next_off);
    if (!state->buf_map) {
      fprintf(stderr, "mmap dax buffer failed: %s\n", strerror(errno));
      exit(1);
    }
    state->dax_next_off += (off_t)buf_len;
    state->buf = (TYPE *)state->buf_map;
  } else if (state->numa_node == NO_NUMA_MEMORY) {
    posix_memalign((void **)&state->buf, CACHE_LINESIZE, state->nbytes);
  } else {
    state->buf = (TYPE *)numa_alloc_onnode(state->nbytes, state->numa_node);
  }

  state->buf2_orig = NULL;
  state->lastone = (TYPE *)state->buf - 1;
  state->lastone = (TYPE *)((char *)state->buf + state->nbytes - 512);
  state->N = state->nbytes;

  if (!state->buf) {
    perror("malloc");
    exit(1);
  }
  bzero((void *)state->buf, state->nbytes);

  if (state->need_buf2 == 1) {
    if (state->dax_path && state->dax_path[0] != '\0') {
      state->buf2_map_len = buf2_len;
      state->buf2_map = map_dax_region(state->dax_fd, state->buf2_map_len,
                                       state->dax_next_off);
      if (!state->buf2_map) {
        fprintf(stderr, "mmap dax buffer2 failed: %s\n", strerror(errno));
        exit(1);
      }
      state->dax_next_off += (off_t)buf2_len;
      state->buf2_orig = state->buf2 = (TYPE *)state->buf2_map;
    } else if (state->numa_node == NO_NUMA_MEMORY) {
      posix_memalign((void **)&state->buf2, CACHE_LINESIZE,
                     state->nbytes + 2048);
      state->buf2_orig = state->buf2;
    } else {
      state->buf2_orig = state->buf2 =
          (TYPE *)numa_alloc_onnode(state->nbytes + 2048, state->numa_node);
    }

    if (!state->buf2) {
      perror("malloc");
      exit(1);
    }

    /* default is to have stuff unaligned wrt each other */
    /* XXX - this is not well tested or thought out */
    if (state->aligned) {
      char *tmp = (char *)state->buf2;

      tmp += 2048 - 128;
      state->buf2 = (TYPE *)tmp;
    }
  }
}

static void cleanup(iter_t iterations, void *cookie) {
  state_t *state = (state_t *)cookie;

  if (iterations) return;
  if (state->dax_path && state->dax_path[0] != '\0') {
    if (state->buf_map) munmap(state->buf_map, state->buf_map_len);
    if (state->buf2_map) munmap(state->buf2_map, state->buf2_map_len);
    if (state->dax_fd >= 0) close(state->dax_fd);
  } else if (state->numa_node == NO_NUMA_MEMORY) {
    free(state->buf);
    if (state->buf2_orig) free(state->buf2_orig);
  } else {
    numa_free(state->buf, state->nbytes);
    if (state->buf2_orig) numa_free(state->buf2_orig, state->nbytes + 2048);
  }
}

static void rd(iter_t iterations, void *cookie) {
  state_t *state = (state_t *)cookie;
  register TYPE *lastone = state->lastone;
  register int sum = 0;

  while (iterations-- > 0) {
    register TYPE *p = state->buf;
    while (p <= lastone) {
      sum +=
#define DOIT(i) p[i] +
          DOIT(0) DOIT(4) DOIT(8) DOIT(12) DOIT(16) DOIT(20) DOIT(24) DOIT(28)
              DOIT(32) DOIT(36) DOIT(40) DOIT(44) DOIT(48) DOIT(52) DOIT(56)
                  DOIT(60) DOIT(64) DOIT(68) DOIT(72) DOIT(76) DOIT(80) DOIT(84)
                      DOIT(88) DOIT(92) DOIT(96) DOIT(100) DOIT(104) DOIT(108)
                          DOIT(112) DOIT(116) DOIT(120) p[124];
      p += 128;
    }
  }
  use_int(sum);
}
#undef DOIT

static void wr(iter_t iterations, void *cookie) {
  state_t *state = (state_t *)cookie;
  register TYPE *lastone = state->lastone;

  while (iterations-- > 0) {
    register TYPE *p = state->buf;
    while (p <= lastone) {
#define DOIT(i) p[i] = 1;
      DOIT(0)
      DOIT(4) DOIT(8) DOIT(12) DOIT(16) DOIT(20) DOIT(24) DOIT(28) DOIT(32)
          DOIT(36) DOIT(40) DOIT(44) DOIT(48) DOIT(52) DOIT(56) DOIT(60)
              DOIT(64) DOIT(68) DOIT(72) DOIT(76) DOIT(80) DOIT(84) DOIT(88)
                  DOIT(92) DOIT(96) DOIT(100) DOIT(104) DOIT(108) DOIT(112)
                      DOIT(116) DOIT(120) DOIT(124);
      p += 128;
    }
  }
}
#undef DOIT

static void ntwr(iter_t iterations, void *cookie) {
  state_t *state = (state_t *)cookie;
  register TYPE *lastone = state->lastone;
  register __m256d r0;

  while (iterations-- > 0) {
    register TYPE *p = state->buf;
    while (p <= lastone) {
      _mm256_stream_pd((double *)(p + 0), r0);
      _mm256_stream_pd((double *)(p + 8), r0);
      _mm256_stream_pd((double *)(p + 16), r0);
      _mm256_stream_pd((double *)(p + 24), r0);

      _mm256_stream_pd((double *)(p + 32), r0);
      _mm256_stream_pd((double *)(p + 40), r0);
      _mm256_stream_pd((double *)(p + 48), r0);
      _mm256_stream_pd((double *)(p + 56), r0);

      _mm256_stream_pd((double *)(p + 64), r0);
      _mm256_stream_pd((double *)(p + 72), r0);
      _mm256_stream_pd((double *)(p + 80), r0);
      _mm256_stream_pd((double *)(p + 88), r0);

      _mm256_stream_pd((double *)(p + 96), r0);
      _mm256_stream_pd((double *)(p + 104), r0);
      _mm256_stream_pd((double *)(p + 112), r0);
      _mm256_stream_pd((double *)(p + 120), r0);
      p += 128;
    }
  }
}

static void rdwr(iter_t iterations, void *cookie) {
  state_t *state = (state_t *)cookie;
  register TYPE *lastone = state->lastone;
  register int sum = 0;

  while (iterations-- > 0) {
    register TYPE *p = state->buf;
    while (p <= lastone) {
#define DOIT(i) \
  sum += p[i];  \
  p[i] = 1;
      DOIT(0)
      DOIT(4) DOIT(8) DOIT(12) DOIT(16) DOIT(20) DOIT(24) DOIT(28) DOIT(32)
          DOIT(36) DOIT(40) DOIT(44) DOIT(48) DOIT(52) DOIT(56) DOIT(60)
              DOIT(64) DOIT(68) DOIT(72) DOIT(76) DOIT(80) DOIT(84) DOIT(88)
                  DOIT(92) DOIT(96) DOIT(100) DOIT(104) DOIT(108) DOIT(112)
                      DOIT(116) DOIT(120) DOIT(124);
      p += 128;
    }
  }
  use_int(sum);
}
#undef DOIT

static void mcp(iter_t iterations, void *cookie) {
  state_t *state = (state_t *)cookie;
  register TYPE *lastone = state->lastone;
  TYPE *p_save = NULL;

  while (iterations-- > 0) {
    register TYPE *p = state->buf;
    register TYPE *dst = state->buf2;
    while (p <= lastone) {
#define DOIT(i) dst[i] = p[i];
      DOIT(0)
      DOIT(4) DOIT(8) DOIT(12) DOIT(16) DOIT(20) DOIT(24) DOIT(28) DOIT(32)
          DOIT(36) DOIT(40) DOIT(44) DOIT(48) DOIT(52) DOIT(56) DOIT(60)
              DOIT(64) DOIT(68) DOIT(72) DOIT(76) DOIT(80) DOIT(84) DOIT(88)
                  DOIT(92) DOIT(96) DOIT(100) DOIT(104) DOIT(108) DOIT(112)
                      DOIT(116) DOIT(120) DOIT(124);
      p += 128;
      dst += 128;
    }
    p_save = p;
  }
  use_pointer(p_save);
}
#undef DOIT

static void fwr(iter_t iterations, void *cookie) {
  state_t *state = (state_t *)cookie;
  register TYPE *lastone = state->lastone;
  TYPE *p_save = NULL;

  while (iterations-- > 0) {
    register TYPE *p = state->buf;
    while (p <= lastone) {
#define DOIT(i) p[i] =
      DOIT(0)
      DOIT(1) DOIT(2) DOIT(3) DOIT(4) DOIT(5) DOIT(6) DOIT(7) DOIT(8) DOIT(
          9) DOIT(10) DOIT(11) DOIT(12) DOIT(13) DOIT(14) DOIT(15) DOIT(16)
          DOIT(17) DOIT(18) DOIT(19) DOIT(20) DOIT(21) DOIT(22) DOIT(23) DOIT(
              24) DOIT(25) DOIT(26) DOIT(27) DOIT(28) DOIT(29) DOIT(30) DOIT(31)
              DOIT(32) DOIT(33) DOIT(34) DOIT(35) DOIT(36) DOIT(37) DOIT(
                  38) DOIT(39) DOIT(40) DOIT(41) DOIT(42) DOIT(43) DOIT(44)
                  DOIT(45) DOIT(46) DOIT(47) DOIT(48) DOIT(49) DOIT(50) DOIT(
                      51) DOIT(52) DOIT(53) DOIT(54) DOIT(55) DOIT(56) DOIT(57)
                      DOIT(58) DOIT(59) DOIT(60) DOIT(61) DOIT(62) DOIT(
                          63) DOIT(64) DOIT(65) DOIT(66) DOIT(67) DOIT(68)
                          DOIT(69) DOIT(70) DOIT(71) DOIT(72) DOIT(73) DOIT(
                              74) DOIT(75) DOIT(76) DOIT(77) DOIT(78) DOIT(79)
                              DOIT(80) DOIT(81) DOIT(82) DOIT(83) DOIT(84) DOIT(
                                  85) DOIT(86) DOIT(87) DOIT(88) DOIT(89)
                                  DOIT(90) DOIT(91) DOIT(92) DOIT(93) DOIT(
                                      94) DOIT(95) DOIT(96) DOIT(97) DOIT(98)
                                      DOIT(99) DOIT(100) DOIT(101) DOIT(
                                          102) DOIT(103) DOIT(104) DOIT(105)
                                          DOIT(106) DOIT(107) DOIT(108) DOIT(
                                              109) DOIT(110) DOIT(111)
                                              DOIT(112) DOIT(
                                                  113) DOIT(114) DOIT(115)
                                                  DOIT(116) DOIT(
                                                      117) DOIT(118) DOIT(119)
                                                      DOIT(120) DOIT(121)
                                                          DOIT(122) DOIT(123)
                                                              DOIT(124) DOIT(
                                                                  125) DOIT(126)
                                                                  DOIT(127) 1;
      p += 128;
    }
    p_save = p;
  }
  use_pointer(p_save);
}
#undef DOIT

static void frd(iter_t iterations, void *cookie) {
  state_t *state = (state_t *)cookie;
  register int sum = 0;
  register TYPE *lastone = state->lastone;

  while (iterations-- > 0) {
    register TYPE *p = state->buf;
    while (p <= lastone) {
      sum +=
#define DOIT(i) p[i] +
          DOIT(0) DOIT(1) DOIT(2) DOIT(3) DOIT(4) DOIT(5) DOIT(6) DOIT(7) DOIT(
              8) DOIT(9) DOIT(10) DOIT(11) DOIT(12) DOIT(13) DOIT(14) DOIT(15)
              DOIT(16) DOIT(17) DOIT(18) DOIT(19) DOIT(20) DOIT(21) DOIT(
                  22) DOIT(23) DOIT(24) DOIT(25) DOIT(26) DOIT(27) DOIT(28)
                  DOIT(29) DOIT(30) DOIT(31) DOIT(32) DOIT(33) DOIT(34) DOIT(
                      35) DOIT(36) DOIT(37) DOIT(38) DOIT(39) DOIT(40) DOIT(41)
                      DOIT(42) DOIT(43) DOIT(44) DOIT(45) DOIT(46) DOIT(47) DOIT(
                          48) DOIT(49) DOIT(50) DOIT(51) DOIT(52) DOIT(53)
                          DOIT(54) DOIT(55) DOIT(56) DOIT(57) DOIT(58) DOIT(
                              59) DOIT(60) DOIT(61) DOIT(62) DOIT(63) DOIT(64)
                              DOIT(65) DOIT(66) DOIT(67) DOIT(68) DOIT(69) DOIT(
                                  70) DOIT(71) DOIT(72) DOIT(73) DOIT(74) DOIT(75)
                                  DOIT(76) DOIT(77) DOIT(78) DOIT(79) DOIT(
                                      80) DOIT(81) DOIT(82) DOIT(83) DOIT(84)
                                      DOIT(85) DOIT(86) DOIT(87) DOIT(88) DOIT(
                                          89) DOIT(90) DOIT(91) DOIT(92) DOIT(93)
                                          DOIT(94) DOIT(95) DOIT(96) DOIT(
                                              97) DOIT(98) DOIT(99) DOIT(100)
                                              DOIT(101) DOIT(102) DOIT(103) DOIT(
                                                  104) DOIT(105) DOIT(106)
                                                  DOIT(107) DOIT(108) DOIT(
                                                      109) DOIT(110) DOIT(111)
                                                      DOIT(112) DOIT(113) DOIT(
                                                          114) DOIT(115) DOIT(116)
                                                          DOIT(117) DOIT(
                                                              118) DOIT(119)
                                                              DOIT(120) DOIT(
                                                                  121) DOIT(122)
                                                                  DOIT(123) DOIT(
                                                                      124) DOIT(125)
                                                                      DOIT(
                                                                          126) p
                                                                          [127];
      p += 128;
    }
  }
  use_int(sum);
}
#undef DOIT

static void fcp(iter_t iterations, void *cookie) {
  state_t *state = (state_t *)cookie;
  register TYPE *lastone = state->lastone;

  while (iterations-- > 0) {
    register TYPE *p = state->buf;
    register TYPE *dst = state->buf2;
    while (p <= lastone) {
#define DOIT(i) dst[i] = p[i];
      DOIT(0)
      DOIT(1) DOIT(2) DOIT(3) DOIT(4) DOIT(5) DOIT(6) DOIT(7) DOIT(8) DOIT(
          9) DOIT(10) DOIT(11) DOIT(12) DOIT(13) DOIT(14) DOIT(15) DOIT(16)
          DOIT(17) DOIT(18) DOIT(19) DOIT(20) DOIT(21) DOIT(22) DOIT(23) DOIT(
              24) DOIT(25) DOIT(26) DOIT(27) DOIT(28) DOIT(29) DOIT(30) DOIT(31)
              DOIT(32) DOIT(33) DOIT(34) DOIT(35) DOIT(36) DOIT(37) DOIT(
                  38) DOIT(39) DOIT(40) DOIT(41) DOIT(42) DOIT(43) DOIT(44)
                  DOIT(45) DOIT(46) DOIT(47) DOIT(48) DOIT(49) DOIT(50) DOIT(
                      51) DOIT(52) DOIT(53) DOIT(54) DOIT(55) DOIT(56) DOIT(57)
                      DOIT(58) DOIT(59) DOIT(60) DOIT(61) DOIT(62) DOIT(
                          63) DOIT(64) DOIT(65) DOIT(66) DOIT(67) DOIT(68)
                          DOIT(69) DOIT(70) DOIT(71) DOIT(72) DOIT(73) DOIT(
                              74) DOIT(75) DOIT(76) DOIT(77) DOIT(78) DOIT(79)
                              DOIT(80) DOIT(81) DOIT(82) DOIT(83) DOIT(84) DOIT(
                                  85) DOIT(86) DOIT(87) DOIT(88) DOIT(89)
                                  DOIT(90) DOIT(91) DOIT(92) DOIT(93) DOIT(
                                      94) DOIT(95) DOIT(96) DOIT(97) DOIT(98)
                                      DOIT(99) DOIT(100) DOIT(101) DOIT(
                                          102) DOIT(103) DOIT(104) DOIT(105)
                                          DOIT(106) DOIT(107) DOIT(108) DOIT(
                                              109) DOIT(110) DOIT(111)
                                              DOIT(112) DOIT(
                                                  113) DOIT(114) DOIT(115)
                                                  DOIT(116) DOIT(
                                                      117) DOIT(118) DOIT(119)
                                                      DOIT(120) DOIT(121)
                                                          DOIT(122) DOIT(123)
                                                              DOIT(124) DOIT(
                                                                  125) DOIT(126)
                                                                  DOIT(127) p +=
          128;
      dst += 128;
    }
  }
}

static void loop_bzero(iter_t iterations, void *cookie) {
  state_t *state = (state_t *)cookie;
  register TYPE *p = state->buf;
  register TYPE *dst = state->buf2;
  register size_t N = state->N;

  while (iterations-- > 0) {
    bzero(p, N);
  }
}

static void loop_bcopy(iter_t iterations, void *cookie) {
  state_t *state = (state_t *)cookie;
  register TYPE *p = state->buf;
  register TYPE *dst = state->buf2;
  register size_t N = state->N;

  while (iterations-- > 0) {
    bcopy(p, dst, N);
  }
}

/*
 * Almost like bandwidth() in lib_timing.c, but we need to adjust
 * bandwidth based upon loop overhead.
 */
static void adjusted_bandwidth(uint64 time, uint64 bytes, uint64 iter,
                               double overhd) {
#define MB (1000. * 1000.)
  extern FILE *ftiming;
  double secs = ((double)time / (double)iter - overhd) / 1000000.0;
  double mb;

  mb = bytes / MB;

  if (secs <= 0.) return;

  if (!ftiming) ftiming = stderr;
  if (mb < 1.) {
    (void)fprintf(ftiming, "%.6f ", mb);
  } else {
    (void)fprintf(ftiming, "%.2f ", mb);
  }
  if (mb / secs < 1.) {
    (void)fprintf(ftiming, "%.6f\n", mb / secs);
  } else {
    (void)fprintf(ftiming, "%.2f\n", mb / secs);
  }
}
