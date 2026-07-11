// Copyright (c) 2015, The Regents of the University of California (Regents)
// See LICENSE.txt for license details
// Multi-Host CXL Version

#include <atomic>
#include <iomanip>
#include <iostream>
#include <cstring>
#include <omp.h>
#include <vector>

#include "../../gapbs/src/benchmark.h"
#include "../../gapbs/src/builder.h"
#include "../../gapbs/src/command_line.h"
#include "../../gapbs/src/timer.h"

#include "cxl_util.h"
#include "shm_graph.h"
#include "shm_support.h"
#include "sync_stats.h"

using gapbs_mh::ArgvPack;
using gapbs_mh::ShmAttachRetry;
using gapbs_mh::ShmBarrier;
using gapbs_mh::ShmBarrierInit;
using gapbs_mh::ShmBarrierWait;
using gapbs_mh::ShmGraph;
using gapbs_mh::ShmGraphInfo;

static const uint64_t kIdBfsState = 0x300;
static const uint64_t kIdBfsParent = 0x301;
static const uint64_t kIdBfsFrontier0 = 0x302;
static const uint64_t kIdBfsFrontier1 = 0x303;
static const uint64_t kIdBfsLocks = 0x304;
static const uint64_t kIdBfsMsgBuf = 0x305;
static const uint64_t kIdBfsBlockBitmap = 0x306;
static const uint32_t kBfsMagic = 0x4D48424D; // "MHBM"
static const uint32_t kNumLocks =
    1048576; // 1M locks (~32MB) to reduce contention
static const uint32_t kMsgCapMin = 4096;
static const uint32_t kMsgCapMax =
    128 * 1024 * 1024; // 129M entries (~1024MB per box)
static const uint32_t kFrontierBlockBytes = 4096;

enum BfsMode {
  kBfsAuto = 0,
  kBfsTdOnly = 1,
  kBfsBuOnly = 2,
};

struct RemoteUpdate {
  NodeID v;
  NodeID u;
};

struct BfsSharedState {
  uint32_t magic;
  uint32_t hosts;
  uint32_t ready;
  uint32_t done;
  uint32_t phase;
  int32_t source;
  uint32_t use_bottom_up; // 0=TD, 1=BU - shared across hosts
  uint32_t _pad0;
  ShmBarrier barrier;
  ShmGraphInfo graph;
  uint64_t parent_id;
  uint64_t frontier0_id;
  uint64_t frontier1_id;
  uint64_t locks_id;
  uint64_t msg_buf_id;
  uint64_t block_bitmap_id;
  uint32_t msg_cap;
  uint32_t logging;
  uint32_t block_bytes;
  uint32_t block_count;
  cacheline_u32 msg_counts[MAX_HOST_NUM][MAX_HOST_NUM];
  cacheline_u32 msg_overflow[MAX_HOST_NUM][MAX_HOST_NUM];
  cacheline_u64 counts[MAX_HOST_NUM];
  cacheline_u64 bu_u_scanned[MAX_HOST_NUM];
  cacheline_u64 bu_v_checked[MAX_HOST_NUM];
  cacheline_u64 bu_curr_true[MAX_HOST_NUM];
  cacheline_u64 td_local_attempts[MAX_HOST_NUM];
  cacheline_u64 td_remote_attempts[MAX_HOST_NUM];
};

static uint32_t ComputeMsgCap(uint64_t nodes, uint32_t hosts) {
  if (hosts == 0)
    return 0;
  uint64_t per_host = (nodes + hosts - 1) / hosts;
  uint64_t cap = per_host;
  if (cap < kMsgCapMin)
    cap = kMsgCapMin;
  if (cap > kMsgCapMax)
    cap = kMsgCapMax;
  return static_cast<uint32_t>(cap);
}

static inline uint32_t OwnerOf(NodeID v, uint32_t hosts, int64_t total_nodes) {
  return static_cast<uint32_t>((static_cast<uint64_t>(v) * hosts) /
                               total_nodes);
}

static inline size_t FrontierBytes(int64_t total_nodes) {
  return (static_cast<size_t>(total_nodes) + 7) / 8;
}

static inline bool FrontierTest(const uint8_t *front, NodeID n) {
  uint8_t mask = static_cast<uint8_t>(1u << (n & 7));
  return (front[static_cast<size_t>(n) >> 3] & mask) != 0;
}

static inline uint32_t BlockIndex(NodeID n, uint32_t block_bytes);

static inline bool FrontierTestBlock(const uint8_t *front,
                                     const uint8_t *blocks, NodeID n,
                                     uint32_t block_bytes) {
  if (blocks && !blocks[BlockIndex(n, block_bytes)])
    return false;
  return FrontierTest(front, n);
}

static inline void FrontierSet(uint8_t *front, NodeID n) {
  uint8_t mask = static_cast<uint8_t>(1u << (n & 7));
  __atomic_fetch_or(&front[static_cast<size_t>(n) >> 3], mask,
                    __ATOMIC_RELAXED);
}

static inline void FrontierClearRange(uint8_t *front, NodeID start, NodeID end) {
  size_t b0 = static_cast<size_t>(start) >> 3;
  size_t b1 = (static_cast<size_t>(end) + 7) >> 3;
  if (b1 > b0)
    memset(front + b0, 0, b1 - b0);
}

static inline uint32_t BlockIndex(NodeID n, uint32_t block_bytes) {
  size_t byte_idx = static_cast<size_t>(n) >> 3;
  return static_cast<uint32_t>(byte_idx / block_bytes);
}

static inline void MarkBlock(uint8_t *blocks, NodeID n,
                             uint32_t block_bytes) {
  uint32_t idx = BlockIndex(n, block_bytes);
  __atomic_store_n(&blocks[idx], 1, __ATOMIC_RELAXED);
}

static void FlushBlocksAll(uint8_t *front, uint8_t *blocks, uint32_t hosts,
                           uint32_t block_count, uint32_t block_bytes,
                           size_t front_bytes) {
  for (uint32_t b = 0; b < block_count; ++b) {
    bool dirty = false;
    for (uint32_t h = 0; h < hosts; ++h) {
      if (blocks[static_cast<size_t>(h) * block_count + b]) {
        dirty = true;
        break;
      }
    }
    if (!dirty)
      continue;
    size_t off = static_cast<size_t>(b) * block_bytes;
    size_t len = block_bytes;
    if (off + len > front_bytes)
      len = front_bytes - off;
    flush_region(front + off, len);
  }
}

static void FlushBlocksLocal(uint8_t *front, uint8_t *blocks_host,
                             uint32_t block_count, uint32_t block_bytes,
                             size_t front_bytes, NodeID start, NodeID end) {
  size_t start_byte = static_cast<size_t>(start) >> 3;
  size_t end_byte = (static_cast<size_t>(end) + 7) >> 3;
  uint32_t start_block = static_cast<uint32_t>(start_byte / block_bytes);
  uint32_t end_block = static_cast<uint32_t>(
      (end_byte + block_bytes - 1) / block_bytes);
  if (end_block > block_count)
    end_block = block_count;
  for (uint32_t b = start_block; b < end_block; ++b) {
    if (!blocks_host[b])
      continue;
    size_t off = static_cast<size_t>(b) * block_bytes;
    size_t len = block_bytes;
    if (off + len > front_bytes)
      len = front_bytes - off;
    flush_region(front + off, len);
  }
}

static uint64_t CountFrontier(uint8_t *front, uint8_t *blocks, uint32_t hosts,
                              uint32_t block_count, uint32_t block_bytes,
                              size_t front_bytes) {
  uint64_t total = 0;
  for (uint32_t b = 0; b < block_count; ++b) {
    bool dirty = false;
    for (uint32_t h = 0; h < hosts; ++h) {
      if (blocks[static_cast<size_t>(h) * block_count + b]) {
        dirty = true;
        break;
      }
    }
    if (!dirty)
      continue;
    size_t off = static_cast<size_t>(b) * block_bytes;
    size_t len = block_bytes;
    if (off + len > front_bytes)
      len = front_bytes - off;
    for (size_t i = 0; i < len; ++i) {
      total += __builtin_popcount(front[off + i]);
    }
  }
  return total;
}

static void WaitForReady(const BfsSharedState *st) {
  // First wait for magic to appear (fresh initialization)
  while (CxlLoad(&st->magic) != kBfsMagic) {
    usleep(1000);
  }
  // Then wait for ready flag
  while (CxlLoad(&st->ready) == 0) {
    usleep(1000);
  }
}

static void ResetBfsArrays(const ShmGraph &g, BfsSharedState *st,
                           NodeID *parent, uint8_t *front0, uint8_t *front1,
                           uint8_t *blocks0, uint8_t *blocks1,
                           uint32_t block_count, uint32_t hosts) {
#pragma omp parallel for schedule(static)
  for (NodeID n = 0; n < g.num_nodes(); n++) {
    // Encode degree as negative value for scout_count computation
    parent[n] = g.out_degree(n) != 0 ? -g.out_degree(n) : -1;
  }
  size_t front_bytes = FrontierBytes(g.num_nodes());
  memset(front0, 0, front_bytes);
  memset(front1, 0, front_bytes);
  if (blocks0)
    memset(blocks0, 0, static_cast<size_t>(block_count) * hosts);
  if (blocks1)
    memset(blocks1, 0, static_cast<size_t>(block_count) * hosts);

  parent[st->source] = st->source;
  FrontierSet(front0, st->source);
  if (blocks0 && block_count > 0) {
    uint32_t owner = OwnerOf(st->source, st->hosts, g.num_nodes());
    uint8_t *blocks0_host = blocks0 + static_cast<size_t>(owner) * block_count;
    MarkBlock(blocks0_host, st->source, st->block_bytes);
  }
  st->phase = 0;
  st->done = 0;
  st->use_bottom_up = 0; // Start in Top-Down mode

  // Flush all modified arrays
  flush_region(parent, g.num_nodes() * sizeof(NodeID));
  flush_region(front0, front_bytes);
  flush_region(front1, front_bytes);
  if (blocks0)
    flush_region(blocks0, static_cast<size_t>(block_count) * hosts);
  if (blocks1)
    flush_region(blocks1, static_cast<size_t>(block_count) * hosts);
  flush_region(st, sizeof(*st));
  store_fence();
}

static void RunBfsOnce(const ShmGraph &g, BfsSharedState *st, NodeID *parent,
                       uint8_t *front0, uint8_t *front1, shm_mutex_t *locks,
                       RemoteUpdate *msg_buf, uint32_t msg_cap,
                       uint8_t *block_bitmap, uint32_t block_count,
                       uint32_t block_bytes,
                       uint32_t host_id, uint32_t num_hosts,
                       bool logging_enabled, int alpha, int beta, int bfs_mode) {
  // Direction-Optimizing BFS parameters (tunable via CLI)

  int level = 0;
  const int64_t total_nodes = g.num_nodes();
  const int64_t total_edges = g.num_edges_directed();
  const size_t front_bytes = FrontierBytes(total_nodes);
  const NodeID start = static_cast<NodeID>(
      (static_cast<int64_t>(host_id) * total_nodes) / num_hosts);
  const NodeID end = static_cast<NodeID>(
      (static_cast<int64_t>(host_id + 1) * total_nodes) / num_hosts);
  const bool logging_all = CxlLoad(&st->logging) != 0;
  uint8_t *blocks0 = block_bitmap;
  uint8_t *blocks1 = block_bitmap
                         ? block_bitmap +
                               static_cast<size_t>(block_count) * num_hosts
                         : nullptr;

  int64_t scout_count = g.out_degree(st->source);
  int64_t edges_to_check = total_edges;
  // use_bottom_up follows shared state unless a fixed mode is requested

  std::vector<uint8_t> curr_blocks_any;
  if (block_count > 0)
    curr_blocks_any.resize(block_count);

  while (true) {
    uint32_t phase = CxlLoad(&st->phase);
    uint8_t *curr = phase == 0 ? front0 : front1;
    uint8_t *next = phase == 0 ? front1 : front0;
    uint8_t *curr_blocks = phase == 0 ? blocks0 : blocks1;
    uint8_t *next_blocks = phase == 0 ? blocks1 : blocks0;
    uint8_t *curr_blocks_host =
        curr_blocks ? curr_blocks + static_cast<size_t>(host_id) * block_count
                    : nullptr;
    uint8_t *next_blocks_host =
        next_blocks ? next_blocks + static_cast<size_t>(host_id) * block_count
                    : nullptr;
    if (next_blocks_host && block_count > 0) {
      memset(next_blocks_host, 0, block_count);
    }
    bool use_bu = false;
    if (bfs_mode == kBfsBuOnly) {
      use_bu = true;
    } else if (bfs_mode == kBfsTdOnly) {
      use_bu = false;
    } else {
      use_bu = CxlLoad(&st->use_bottom_up) != 0;
    }
    double td_time_s = 0.0;
    double bu_time_s = 0.0;
    double msg_send_s = 0.0;
    double msg_recv_s = 0.0;
    double msg_overflow_s = 0.0;

    if (curr_blocks) {
      flush_region(curr_blocks, static_cast<size_t>(block_count) * num_hosts);
      full_fence();
    }
    // BATCH FLUSH: Only flush the ranges we will read directly.
    if (use_bu) {
      if (curr_blocks) {
        if (!curr_blocks_any.empty())
          memset(curr_blocks_any.data(), 0, curr_blocks_any.size());
        for (uint32_t b = 0; b < block_count; ++b) {
          bool dirty = false;
          for (uint32_t h = 0; h < num_hosts; ++h) {
            if (curr_blocks[static_cast<size_t>(h) * block_count + b]) {
              dirty = true;
              if (!curr_blocks_any.empty())
                curr_blocks_any[b] = 1;
              break;
            }
          }
          if (!dirty)
            continue;
          size_t off = static_cast<size_t>(b) * block_bytes;
          size_t len = block_bytes;
          if (off + len > front_bytes)
            len = front_bytes - off;
          flush_region(curr + off, len);
        }
      } else {
        flush_region(curr, front_bytes);
      }
    } else {
      if (curr_blocks_host) {
        FlushBlocksLocal(curr, curr_blocks_host, block_count, block_bytes,
                         front_bytes, start, end);
      } else {
        size_t start_byte = static_cast<size_t>(start) >> 3;
        size_t end_byte = (static_cast<size_t>(end) + 7) >> 3;
        flush_region(curr + start_byte, end_byte - start_byte);
      }
    }
    flush_region(parent + start, (end - start) * sizeof(NodeID));
    full_fence(); // Single fence for all flushes

    int64_t local_count = 0;
    uint64_t local_u_scanned = 0;
    uint64_t local_v_checked = 0;
    uint64_t local_curr_true = 0;
    uint64_t local_td_local = 0;
    uint64_t local_td_remote = 0;

    if (use_bu) {
      //----------------------------------------------------------------------
      // Bottom-Up Step: Scan unvisited nodes, check if any neighbor in frontier
      // Using DIRECT reads (no per-read flush), since we batch-flushed above
      //----------------------------------------------------------------------
      Timer bu_timer;
      bu_timer.Start();
      const uint8_t *blocks_any =
          curr_blocks_any.empty() ? nullptr : curr_blocks_any.data();
#pragma omp parallel for reduction(+ : local_count, local_u_scanned,          \
                                   local_v_checked, local_curr_true)         \
    schedule(dynamic, 1024)
      for (NodeID u = start; u < end; u++) {
        if (parent[u] >= 0) // Direct read
          continue;
        if (logging_all)
          local_u_scanned++;
        for (NodeID v : g.in_neigh(u)) {
          if (logging_all)
            local_v_checked++;
          if (FrontierTestBlock(curr, blocks_any, v, block_bytes)) {
            parent[u] = v;             // Direct write
            FrontierSet(next, u);
            if (next_blocks_host)
              MarkBlock(next_blocks_host, u, block_bytes);
            local_count++;
            if (logging_all)
              local_curr_true++;
            break;
          }
        }
      }
      bu_timer.Stop();
      bu_time_s = bu_timer.Seconds();
      if (logging_all) {
        CACHELINE_STORE(&st->bu_u_scanned[host_id], local_u_scanned);
        CACHELINE_STORE(&st->bu_v_checked[host_id], local_v_checked);
        CACHELINE_STORE(&st->bu_curr_true[host_id], local_curr_true);
        CACHELINE_STORE(&st->td_local_attempts[host_id], 0);
        CACHELINE_STORE(&st->td_remote_attempts[host_id], 0);
      }
    } else {
      //----------------------------------------------------------------------
      // Top-Down Step: Expand frontier nodes to neighbors
      // HYBRID STRATEGY:
      // 1. Local neighbors: Use hardware atomic CAS immediately
      // 2. Remote neighbors: Buffer and flush after barrier using SW CAS
      //----------------------------------------------------------------------
      int64_t local_scout = 0;
      std::atomic<uint32_t> local_counts[MAX_HOST_NUM];
      std::atomic<uint32_t> overflow_counts[MAX_HOST_NUM];
      RemoteUpdate *outboxes[MAX_HOST_NUM];
      std::vector<std::vector<RemoteUpdate>> thread_overflow_updates(
          omp_get_max_threads());

      for (uint32_t i = 0; i < num_hosts; ++i) {
        local_counts[i].store(0, std::memory_order_relaxed);
        overflow_counts[i].store(0, std::memory_order_relaxed);
        if (msg_buf && msg_cap > 0) {
          size_t base = (static_cast<size_t>(host_id) * num_hosts + i) *
                        static_cast<size_t>(msg_cap);
          outboxes[i] = msg_buf + base;
        } else {
          outboxes[i] = nullptr;
        }
      }

      Timer td_timer;
      td_timer.Start();
#pragma omp parallel reduction(+ : local_count, local_scout, local_td_local,  \
                               local_td_remote)
      {
        int tid = omp_get_thread_num();
#pragma omp for schedule(dynamic, 16384)
        for (NodeID u = start; u < end; u++) {
          if (curr_blocks_host &&
              !curr_blocks_host[BlockIndex(u, block_bytes)])
            continue;
          if (!FrontierTest(curr, u))
            continue;
          for (NodeID v : g.out_neigh(u)) {
            uint32_t owner = OwnerOf(v, num_hosts, total_nodes);
            bool is_local = (owner == host_id || !msg_buf || msg_cap == 0);
            if (logging_all) {
              if (is_local)
                local_td_local++;
              else
                local_td_remote++;
            }
            if (is_local) {
              NodeID curr_val = parent[v];
              if (curr_val < 0) {
                if (__sync_val_compare_and_swap(&parent[v], curr_val, u) ==
                    curr_val) {
                  FrontierSet(next, v);
                  if (next_blocks_host)
                    MarkBlock(next_blocks_host, v, block_bytes);
                  local_count++;
                  local_scout += -curr_val;
                }
              }
            } else {
              uint32_t idx =
                  local_counts[owner].fetch_add(1, std::memory_order_relaxed);
              if (idx < msg_cap) {
                outboxes[owner][idx] = {v, u};
              } else {
                overflow_counts[owner].fetch_add(1, std::memory_order_relaxed);
                thread_overflow_updates[tid].push_back({v, u});
              }
            }
          }
        }
      }
      td_timer.Stop();
      td_time_s = td_timer.Seconds();
      if (logging_all) {
        CACHELINE_STORE(&st->td_local_attempts[host_id], local_td_local);
        CACHELINE_STORE(&st->td_remote_attempts[host_id], local_td_remote);
        CACHELINE_STORE(&st->bu_u_scanned[host_id], 0);
        CACHELINE_STORE(&st->bu_v_checked[host_id], 0);
        CACHELINE_STORE(&st->bu_curr_true[host_id], 0);
      }

      Timer msg_send_timer;
      msg_send_timer.Start();
      for (uint32_t dst = 0; dst < num_hosts; ++dst) {
        uint32_t count = local_counts[dst].load(std::memory_order_relaxed);
        uint32_t publish_count = count < msg_cap ? count : msg_cap;
        if (dst != host_id && publish_count > 0 && outboxes[dst]) {
          flush_region(outboxes[dst], static_cast<size_t>(publish_count) *
                                          sizeof(RemoteUpdate));
        }
        CACHELINE_STORE(&st->msg_counts[host_id][dst], publish_count);
        CACHELINE_STORE(&st->msg_overflow[host_id][dst],
                        overflow_counts[dst].load(std::memory_order_relaxed));
      }
      msg_send_timer.Stop();
      msg_send_s = msg_send_timer.Seconds();

      ShmBarrierWait(&st->barrier, num_hosts, host_id);

      Timer msg_recv_timer;
      msg_recv_timer.Start();
      if (msg_buf && msg_cap > 0) {
        for (uint32_t src = 0; src < num_hosts; ++src) {
          if (src == host_id)
            continue;
          uint32_t count = CACHELINE_LOAD(&st->msg_counts[src][host_id]);
          if (count == 0)
            continue;
          RemoteUpdate *inbox =
              msg_buf + (static_cast<size_t>(src) * num_hosts + host_id) *
                            static_cast<size_t>(msg_cap);
          flush_region(inbox,
                       static_cast<size_t>(count) * sizeof(RemoteUpdate));
          full_fence();

#pragma omp parallel for reduction(+ : local_count, local_scout)
          for (uint32_t i = 0; i < count; ++i) {
            NodeID v = inbox[i].v;
            NodeID u = inbox[i].u;
            NodeID curr_val = parent[v];
            if (curr_val < 0) {
              if (__sync_val_compare_and_swap(&parent[v], curr_val, u) ==
                  curr_val) {
                FrontierSet(next, v);
                if (next_blocks_host)
                  MarkBlock(next_blocks_host, v, block_bytes);
                local_count++;
                local_scout += -curr_val;
              }
            }
          }
        }
      }
      msg_recv_timer.Stop();
      msg_recv_s = msg_recv_timer.Seconds();

      flush_region(&st->msg_overflow, sizeof(st->msg_overflow));
      full_fence();
      bool overflow_any = false;
      for (uint32_t src = 0; src < num_hosts && !overflow_any; ++src) {
        for (uint32_t dst = 0; dst < num_hosts; ++dst) {
          if (CACHELINE_LOAD(&st->msg_overflow[src][dst]) > 0) {
            overflow_any = true;
            break;
          }
        }
      }

      if (overflow_any) {
        ShmBarrierWait(&st->barrier, num_hosts, host_id);
        Timer overflow_timer;
        overflow_timer.Start();
#pragma omp parallel for reduction(+ : local_count, local_scout)
        for (int t = 0; t < omp_get_max_threads(); ++t) {
          for (const auto &up : thread_overflow_updates[t]) {
            NodeID v = up.v;
            NodeID u = up.u;
            NodeID curr_val = CxlLoad(&parent[v]);
            if (curr_val < 0) {
              if (CxlHashedCAS(parent, static_cast<uint64_t>(v), curr_val, u,
                               locks, kNumLocks, static_cast<int>(host_id),
                               static_cast<int>(num_hosts))) {
                FrontierSet(next, v);
                if (next_blocks_host)
                  MarkBlock(next_blocks_host, v, block_bytes);
                local_count++;
                local_scout += -curr_val;
              }
            }
          }
        }
        overflow_timer.Stop();
        msg_overflow_s = overflow_timer.Seconds();
      }

      CACHELINE_STORE(&st->counts[host_id], static_cast<uint64_t>(local_scout));
    }

    if (use_bu) {
      CACHELINE_STORE(&st->counts[host_id], static_cast<uint64_t>(local_count));
    }

    // Flush this host's portion of 'next' array so other hosts can see it
    size_t start_byte = static_cast<size_t>(start) >> 3;
    size_t end_byte = (static_cast<size_t>(end) + 7) >> 3;
    flush_region(next + start_byte, end_byte - start_byte);
    // Also flush parent updates from this host's range
    flush_region(parent + start, (end - start) * sizeof(NodeID));
    if (next_blocks_host && block_count > 0) {
      flush_region(next_blocks_host, block_count);
    }
    store_fence();

    ShmBarrierWait(&st->barrier, num_hosts, host_id);

    // Host 0 aggregates and decides next phase
    if (host_id == 0) {
      uint64_t total_count = 0;
      uint64_t total_scout = 0;
      for (uint32_t i = 0; i < num_hosts; ++i) {
        if (use_bu) {
          total_count += CACHELINE_LOAD(&st->counts[i]);
        } else {
          total_scout += CACHELINE_LOAD(&st->counts[i]);
        }
      }

      if (use_bu) {
        if (logging_enabled) {
          uint64_t total_u = 0;
          uint64_t total_v = 0;
          uint64_t total_curr = 0;
          for (uint32_t i = 0; i < num_hosts; ++i) {
            total_u += CACHELINE_LOAD(&st->bu_u_scanned[i]);
            total_v += CACHELINE_LOAD(&st->bu_v_checked[i]);
            total_curr += CACHELINE_LOAD(&st->bu_curr_true[i]);
          }
          std::cout << "bu " << level << " awake=" << total_count
                    << " time=" << std::fixed << std::setprecision(6)
                    << bu_time_s << " u_scanned=" << total_u
                    << " v_checked=" << total_v
                    << " curr_true=" << total_curr << std::endl;
        }

        if (total_count == 0) {
          CxlStore(&st->done, 1u);
        } else {
          CxlStore(&st->done, 0u);
          uint32_t cur_phase = CxlLoad(&st->phase);
          CxlStore(&st->phase, cur_phase ^ 1u);
          if (bfs_mode == kBfsAuto &&
              total_count < static_cast<uint64_t>(total_nodes / beta)) {
            CxlStore(&st->use_bottom_up, 0u);
          }
        }
      } else {
        scout_count = static_cast<int64_t>(total_scout);
        edges_to_check -= scout_count;

        // Count frontier size (flush first to see remote updates)
        if (next_blocks) {
          flush_region(next_blocks,
                       static_cast<size_t>(block_count) * num_hosts);
          full_fence();
          FlushBlocksAll(next, next_blocks, num_hosts, block_count, block_bytes,
                         front_bytes);
        } else {
          flush_region(next, front_bytes);
        }
        full_fence();
        uint64_t frontier_size = 0;
        if (next_blocks) {
          frontier_size =
              CountFrontier(next, next_blocks, num_hosts, block_count,
                            block_bytes, front_bytes);
        } else {
          for (NodeID n = 0; n < total_nodes; n++) {
            if (FrontierTest(next, n))
              frontier_size++;
          }
        }

        if (logging_enabled) {
          uint64_t total_local = 0;
          uint64_t total_remote = 0;
          double sum_local_ratio = 0.0;
          uint32_t ratio_hosts = 0;
          for (uint32_t i = 0; i < num_hosts; ++i) {
            uint64_t local = CACHELINE_LOAD(&st->td_local_attempts[i]);
            uint64_t remote = CACHELINE_LOAD(&st->td_remote_attempts[i]);
            total_local += local;
            total_remote += remote;
            uint64_t denom = local + remote;
            if (denom > 0) {
              sum_local_ratio += static_cast<double>(local) /
                                 static_cast<double>(denom);
              ratio_hosts++;
            }
          }
          double total_ratio =
              (total_local + total_remote) > 0
                  ? static_cast<double>(total_local) /
                        static_cast<double>(total_local + total_remote)
                  : 0.0;
          double avg_local_ratio =
              ratio_hosts > 0 ? (sum_local_ratio / ratio_hosts) : 0.0;
          double avg_remote_ratio =
              ratio_hosts > 0 ? (1.0 - avg_local_ratio) : 0.0;

          std::cout << "td " << level << " frontier=" << frontier_size
                    << " time=" << std::fixed << std::setprecision(6)
                    << td_time_s;
          std::cout << " msg_send=" << msg_send_s << " msg_recv=" << msg_recv_s;
          if (msg_overflow_s > 0.0) {
            std::cout << " msg_overflow=" << msg_overflow_s;
          }
          std::cout << " local=" << total_local
                    << " remote=" << total_remote
                    << " lr=" << total_ratio
                    << " avg_lr=" << avg_local_ratio
                    << " avg_rr=" << avg_remote_ratio;
          std::cout << std::endl;
        }

        if (frontier_size == 0) {
          CxlStore(&st->done, 1u);
        } else {
          CxlStore(&st->done, 0u);
          uint32_t cur_phase = CxlLoad(&st->phase);
          CxlStore(&st->phase, cur_phase ^ 1u);
          if (bfs_mode == kBfsAuto &&
              scout_count > edges_to_check / alpha) {
            CxlStore(&st->use_bottom_up, 1u);
          }
        }
      }
    }

    ShmBarrierWait(&st->barrier, num_hosts, host_id);

    if (CxlLoad(&st->done))
      break;

    // Clear old frontier
    uint8_t *to_clear = CxlLoad(&st->phase) == 0 ? front1 : front0;
    size_t clear_start = static_cast<size_t>(start) >> 3;
    size_t clear_end = (static_cast<size_t>(end) + 7) >> 3;
#pragma omp parallel for schedule(static)
    for (size_t i = clear_start; i < clear_end; ++i) {
      to_clear[i] = 0;
    }
    flush_region(to_clear + clear_start, clear_end - clear_start);
    store_fence();

    ShmBarrierWait(&st->barrier, num_hosts, host_id);
    level++;
  }
}

static void PrintBFSStats(const ShmGraph &g, const NodeID *parent) {
  int64_t tree_size = 0;
  int64_t n_edges = 0;
  for (NodeID n = 0; n < g.num_nodes(); n++) {
    if (parent[n] >= 0) {
      n_edges += g.out_degree(n);
      tree_size++;
    }
  }
  std::cout << "BFS Tree has " << tree_size << " nodes and " << n_edges
            << " edges" << std::endl;
}

static bool BFSVerifier(const ShmGraph &g, NodeID source,
                        const NodeID *parent) {
  pvector<int> depth(g.num_nodes(), -1);
  depth[source] = 0;
  std::vector<NodeID> to_visit;
  to_visit.reserve(g.num_nodes());
  to_visit.push_back(source);
  for (auto it = to_visit.begin(); it != to_visit.end(); it++) {
    NodeID u = *it;
    for (NodeID v : g.out_neigh(u)) {
      if (depth[v] == -1) {
        depth[v] = depth[u] + 1;
        to_visit.push_back(v);
      }
    }
  }
  for (NodeID u = 0; u < g.num_nodes(); u++) {
    if ((depth[u] != -1) && (parent[u] != -1)) {
      if (u == source) {
        if (!((parent[u] == u) && (depth[u] == 0))) {
          std::cout << "Source wrong" << std::endl;
          return false;
        }
        continue;
      }
      bool parent_found = false;
      for (NodeID v : g.in_neigh(u)) {
        if (v == parent[u]) {
          if (depth[v] != depth[u] - 1) {
            std::cout << "Wrong depths for " << u << " & " << v << std::endl;
            return false;
          }
          parent_found = true;
          break;
        }
      }
      if (!parent_found) {
        std::cout << "Couldn't find edge from " << parent[u] << " to " << u
                  << std::endl;
        return false;
      }
    } else if (depth[u] != parent[u]) {
      std::cout << "Reachability mismatch" << std::endl;
      return false;
    }
  }
  return true;
}

int main(int argc, char *argv[]) {
  ArgvPack pack = gapbs_mh::ParseMhOptions(argc, argv);
  int mh_argc = static_cast<int>(pack.args.size());
  char **mh_argv = pack.args.data();

  CLApp cli(mh_argc, mh_argv, "breadth-first search (multi-host)");
  if (!cli.ParseArgs())
    return -1;

  uint32_t hosts = pack.opts.hosts;
  uint32_t host_id = pack.opts.host_id;
  int bfs_mode = kBfsAuto;
  if (pack.opts.td_only && pack.opts.bu_only) {
    fprintf(stderr, "invalid mode: use only one of --td-only/--bu-only\n");
    return 1;
  }
  if (pack.opts.td_only)
    bfs_mode = kBfsTdOnly;
  if (pack.opts.bu_only)
    bfs_mode = kBfsBuOnly;

  // Setup SHM arg
  struct shm_arg arg;
  memset(&arg, 0, sizeof(arg));
  arg.host_id = host_id;
  arg.host_num = hosts;
  arg.map_size = pack.opts.shm_mb * 1024ULL * 1024ULL;
  snprintf(arg.dax_dev, DEV_NAME_LENGTH, "%s", pack.opts.dev_path);

  // Pointers for shared state
  BfsSharedState *state = nullptr;
  NodeID *parent = nullptr;
  uint8_t *front0 = nullptr;
  uint8_t *front1 = nullptr;
  shm_mutex_t *locks = nullptr;
  RemoteUpdate *msg_buf = nullptr;
  uint8_t *block_bitmap = nullptr;

  if (host_id == 0) {
    // Host 0: Enable & Init
    arg.flags = SHM_ENABLE_RESET;
    int rc = shm_enable(arg);
    if (rc != 0) {
      fprintf(stderr, "shm_enable failed: %d\n", rc);
      return 1;
    }

    state = static_cast<BfsSharedState *>(
        shm_malloc_id(kIdBfsState, sizeof(BfsSharedState)));
    if (!state) {
      perror("shm_malloc_id(state)");
      return 1;
    }
    memset(state, 0, sizeof(*state)); // Clears magic and ready
    CxlStore(&state->magic, 0u);      // Explicitly clear for visibility
    CxlStore(&state->ready, 0u);
    state->hosts = hosts;
    state->parent_id = kIdBfsParent;
    state->frontier0_id = kIdBfsFrontier0;
    state->frontier1_id = kIdBfsFrontier1;
    state->locks_id = kIdBfsLocks;
    // NOTE: barrier_init moved to common path below

    // Force use of all available cores for graph generation and build
    int user_threads = omp_get_max_threads();
    int max_cores = omp_get_num_procs();
    omp_set_num_threads(max_cores);
    std::cout << "Using " << max_cores << " threads for graph generation..."
              << std::endl;

    Builder b(cli);
    Graph g_build = b.MakeGraph();

    // Also use max cores for copying to shared memory
    if (gapbs_mh::BuildSharedGraph(g_build, &state->graph) != 0) {
      fprintf(stderr, "failed to build shared graph\n");
      return 1;
    }

    // Restore user-requested thread count for BFS
    omp_set_num_threads(user_threads);
    std::cout << "Restored " << user_threads << " threads for BFS execution."
              << std::endl;

    size_t nodes = static_cast<size_t>(g_build.num_nodes());
    size_t front_bytes = FrontierBytes(g_build.num_nodes());
    parent = static_cast<NodeID *>(
        shm_malloc_id(kIdBfsParent, nodes * sizeof(NodeID)));
    front0 = static_cast<uint8_t *>(
        shm_malloc_id(kIdBfsFrontier0, front_bytes));
    front1 = static_cast<uint8_t *>(
        shm_malloc_id(kIdBfsFrontier1, front_bytes));
    locks = static_cast<shm_mutex_t *>(
        shm_malloc_id(kIdBfsLocks, kNumLocks * sizeof(shm_mutex_t)));
    state->block_bytes = kFrontierBlockBytes;
    state->block_count = static_cast<uint32_t>(
        (front_bytes + kFrontierBlockBytes - 1) / kFrontierBlockBytes);
    state->block_bitmap_id = kIdBfsBlockBitmap;
    size_t block_len = static_cast<size_t>(2) * hosts *
                       static_cast<size_t>(state->block_count);
    block_bitmap = static_cast<uint8_t *>(
        shm_malloc_id(kIdBfsBlockBitmap, block_len));
    state->msg_cap = ComputeMsgCap(nodes, hosts);
    state->msg_buf_id = kIdBfsMsgBuf;
    size_t msg_buf_len = static_cast<size_t>(hosts) * hosts *
                         static_cast<size_t>(state->msg_cap) *
                         sizeof(RemoteUpdate);
    msg_buf =
        static_cast<RemoteUpdate *>(shm_malloc_id(kIdBfsMsgBuf, msg_buf_len));
    if (!parent || !front0 || !front1 || !locks || !msg_buf ||
        !block_bitmap) {
      perror("shm_malloc_id(bfs arrays)");
      return 1;
    }

    // Initialize locks
    for (uint32_t i = 0; i < kNumLocks; ++i) {
      shm_mutex_init(&locks[i]);
    }

    state->source = cli.start_vertex();
    flush_region(state, sizeof(*state));
    store_fence();
    CxlStore(&state->magic, kBfsMagic); // Set magic LAST (after all init)
    CxlStore(&state->ready, 1u);

    std::cout << "[Host 0] Initialization complete. Waiting for other hosts..."
              << std::endl;
  } else {
    // Non-Host-0: Just enable
    arg.flags = 0;
    int rc = shm_enable(arg);
    if (rc != 0) {
      fprintf(stderr, "shm_enable failed: %d\n", rc);
      return 1;
    }
  }

  // All hosts attach to shared state
  state = static_cast<BfsSharedState *>(
      ShmAttachRetry(kIdBfsState, sizeof(BfsSharedState), "bfs_state"));
  if (!state)
    return 1;

  WaitForReady(state);

  if (CxlLoad(&state->magic) != kBfsMagic) {
    fprintf(stderr, "bfs_state magic mismatch\n");
    return 1;
  }
  flush_region(state, sizeof(*state));
  full_fence();

  ShmGraph g;
  // Flush state->graph to ensure we read fresh IDs (not cached from previous
  // run)
  flush_region(&state->graph, sizeof(state->graph));
  full_fence();
  if (gapbs_mh::AttachSharedGraph(&state->graph, &g) != 0)
    return 1;

  // Invalidate stale CPU cache for graph CSR arrays (critical for CXL!)
  g.FlushArrays();

  // All hosts must call barrier_init (idempotent, per barrier_test.c pattern)
  ShmBarrierInit(&state->barrier, hosts, host_id);

  SourcePicker<ShmGraph> sp(g, cli.start_vertex());

  // Attach arrays
  parent =
      static_cast<NodeID *>(ShmAttachRetry(state->parent_id, 0, "bfs_parent"));
  front0 = static_cast<uint8_t *>(
      ShmAttachRetry(state->frontier0_id, 0, "bfs_frontier0"));
  front1 = static_cast<uint8_t *>(
      ShmAttachRetry(state->frontier1_id, 0, "bfs_frontier1"));
  locks = static_cast<shm_mutex_t *>(
      ShmAttachRetry(state->locks_id, 0, "bfs_locks"));
  msg_buf = static_cast<RemoteUpdate *>(
      ShmAttachRetry(state->msg_buf_id, 0, "bfs_msg_buf"));
  block_bitmap = static_cast<uint8_t *>(
      ShmAttachRetry(state->block_bitmap_id, 0, "bfs_block_bitmap"));
  if (!parent || !front0 || !front1 || !locks || !msg_buf || !block_bitmap)
    return 1;

  Timer trial_timer;
  double total_seconds = 0.0;

  // Initial sync barrier to ensure all hosts are ready before starting
  std::cout << "[Host " << host_id << "] Ready, waiting at initial barrier..."
            << std::endl;
  ShmBarrierWait(&state->barrier, hosts, host_id);
  std::cout << "[Host " << host_id << "] All hosts synchronized, starting BFS"
            << std::endl;

  if (host_id == 0)
    g.PrintStats();

  for (int iter = 0; iter < cli.num_trials(); iter++) {
    if (host_id == 0) {
      state->source = sp.PickNext();
      uint8_t *blocks0 = block_bitmap;
      uint8_t *blocks1 = block_bitmap +
                         static_cast<size_t>(hosts) * state->block_count;
      ResetBfsArrays(g, state, parent, front0, front1, blocks0, blocks1,
                     state->block_count, hosts);
      CxlStore(&state->logging, cli.logging_en() ? 1u : 0u);
      flush_region(&state->logging, sizeof(state->logging));
      store_fence();
      if (bfs_mode == kBfsBuOnly) {
        CxlStore(&state->use_bottom_up, 1u);
        flush_region(&state->use_bottom_up, sizeof(state->use_bottom_up));
        store_fence();
      } else if (bfs_mode == kBfsTdOnly) {
        CxlStore(&state->use_bottom_up, 0u);
        flush_region(&state->use_bottom_up, sizeof(state->use_bottom_up));
        store_fence();
      }
    }
    ShmBarrierWait(&state->barrier, hosts, host_id);

    if (host_id == 0)
      trial_timer.Start();
    ShmBarrierWait(&state->barrier, hosts, host_id);

    RunBfsOnce(g, state, parent, front0, front1, locks, msg_buf, state->msg_cap,
               block_bitmap, state->block_count, state->block_bytes, host_id,
               hosts, cli.logging_en() && host_id == 0,
               pack.opts.alpha, pack.opts.beta, bfs_mode);

    ShmBarrierWait(&state->barrier, hosts, host_id);
    if (host_id == 0) {
      trial_timer.Stop();
      PrintTime("Trial Time", trial_timer.Seconds());
      total_seconds += trial_timer.Seconds();
      flush_region(parent, g.num_nodes() * sizeof(NodeID));
      full_fence();
      if (cli.do_analysis() && (iter == (cli.num_trials() - 1)))
        PrintBFSStats(g, parent);
      if (cli.do_verify() && (iter == (cli.num_trials() - 1))) {
#pragma omp parallel for schedule(static)
        for (NodeID n = 0; n < g.num_nodes(); n++) {
          if (parent[n] < -1)
            parent[n] = -1;
        }
        flush_region(parent, g.num_nodes() * sizeof(NodeID));
        store_fence();
        bool ok = BFSVerifier(g, static_cast<NodeID>(state->source), parent);
        PrintLabel("Verification", ok ? "PASS" : "FAIL");
      }
    } else {
      std::cout << "[Host " << host_id << "] Trial " << (iter + 1)
                << " complete" << std::endl;
    }
    ShmBarrierWait(&state->barrier, hosts, host_id);
  }

  if (host_id == 0)
    PrintTime("Average Time", total_seconds / cli.num_trials());

  sync_stats_dump("bfs");
  return 0;
}
