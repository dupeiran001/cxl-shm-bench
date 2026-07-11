// Copyright (c) 2015, The Regents of the University of California (Regents)
// Multi-Host CXL Version - Triangle Counting
// See LICENSE.txt for license details

#include <algorithm>
#include <cinttypes>
#include <iostream>
#include <omp.h>
#include <vector>

#include "../../gapbs/src/benchmark.h"
#include "../../gapbs/src/builder.h"
#include "../../gapbs/src/command_line.h"

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

static const uint64_t kIdTcState = 0x800;
static const uint32_t kTcMagic = 0x4D485443; // "MHTC"

struct TcSharedState {
  uint32_t magic;
  uint32_t hosts;
  uint32_t ready;
  uint32_t _pad;
  ShmBarrier barrier;
  ShmGraphInfo graph;
  uint64_t total_degree;
  int64_t part_offsets[MAX_HOST_NUM + 1];
  uint64_t part_degrees[MAX_HOST_NUM];
  uint64_t counts[MAX_HOST_NUM];
};

static void WaitForReady(const TcSharedState *st) {
  while (CxlLoad(&st->magic) != kTcMagic) {
    usleep(1000);
  }
  while (CxlLoad(&st->ready) == 0) {
    usleep(1000);
  }
}

// Heuristic to see if sufficiently dense power-law graph
static bool WorthRelabelling(const Graph &g) {
  int64_t average_degree = g.num_edges() / g.num_nodes();
  if (average_degree < 10)
    return false;
  SourcePicker<Graph> sp(g);
  int64_t num_samples = std::min<int64_t>(1000, g.num_nodes());
  int64_t sample_total = 0;
  std::vector<int64_t> samples(num_samples);
  for (int64_t trial = 0; trial < num_samples; trial++) {
    samples[trial] = g.out_degree(sp.PickNext());
    sample_total += samples[trial];
  }
  std::sort(samples.begin(), samples.end());
  double sample_average = static_cast<double>(sample_total) / num_samples;
  double sample_median = samples[num_samples / 2];
  return sample_average / 1.3 > sample_median;
}

static void ComputeDegreePartition(const Graph &g, uint32_t hosts,
                                   int64_t *offsets, uint64_t *deg_sums,
                                   uint64_t *total_deg_out) {
  const int64_t total_nodes = g.num_nodes();
  std::vector<int64_t> degrees(total_nodes);
  int64_t total_deg = 0;
#pragma omp parallel for reduction(+ : total_deg)
  for (NodeID n = 0; n < total_nodes; n++) {
    degrees[n] = g.out_degree(n);
    total_deg += degrees[n];
  }

  offsets[0] = 0;
  if (total_deg == 0) {
    for (uint32_t h = 0; h < hosts; ++h) {
      offsets[h + 1] =
          static_cast<int64_t>((static_cast<int64_t>(h + 1) * total_nodes) /
                               hosts);
      deg_sums[h] = 0;
    }
    *total_deg_out = 0;
    return;
  }

  int64_t running = 0;
  int64_t host_start_sum = 0;
  uint32_t host = 0;
  for (NodeID n = 0; n < total_nodes; n++) {
    running += degrees[n];
    while (host + 1 < hosts &&
           running >= static_cast<int64_t>(
                          (static_cast<int64_t>(host + 1) * total_deg) /
                          hosts)) {
      offsets[host + 1] = static_cast<int64_t>(n + 1);
      deg_sums[host] = static_cast<uint64_t>(running - host_start_sum);
      host_start_sum = running;
      host++;
    }
  }
  offsets[hosts] = total_nodes;
  if (host < hosts) {
    deg_sums[host] = static_cast<uint64_t>(running - host_start_sum);
    for (uint32_t h = host + 1; h < hosts; ++h)
      deg_sums[h] = 0;
  }
  *total_deg_out = static_cast<uint64_t>(total_deg);
}

// Count triangles in vertex range [start, end)
static size_t OrderedCountRange(const ShmGraph &g, NodeID start, NodeID end) {
  size_t total = 0;
#pragma omp parallel for reduction(+ : total) schedule(dynamic, 64)
  for (NodeID u = start; u < end; u++) {
    for (NodeID v : g.out_neigh(u)) {
      if (v > u)
        break;
      auto it = g.out_neigh(v).begin();
      for (NodeID w : g.out_neigh(u)) {
        if (w > v)
          break;
        while (*it < w)
          it++;
        if (w == *it)
          total++;
      }
    }
  }
  return total;
}

static void RunTcOnce(const ShmGraph &g, TcSharedState *st, uint32_t host_id,
                      uint32_t hosts) {
  const NodeID start = static_cast<NodeID>(st->part_offsets[host_id]);
  const NodeID end = static_cast<NodeID>(st->part_offsets[host_id + 1]);

  size_t local_count = OrderedCountRange(g, start, end);
  st->counts[host_id] = local_count;
  flush_region(&st->counts[host_id], sizeof(uint64_t));
  store_fence();

  ShmBarrierWait(&st->barrier, hosts, host_id);
}

static void PrintTriangleStats(size_t total_triangles) {
  std::cout << total_triangles << " triangles" << std::endl;
}

// Simple serial verifier using set_intersection
static bool TCVerifier(const ShmGraph &g, size_t test_total) {
  size_t total = 0;
  std::vector<NodeID> intersection;
  intersection.reserve(g.num_nodes());
  for (NodeID u = 0; u < g.num_nodes(); u++) {
    for (NodeID v : g.out_neigh(u)) {
      auto new_end = std::set_intersection(
          g.out_neigh(u).begin(), g.out_neigh(u).end(), g.out_neigh(v).begin(),
          g.out_neigh(v).end(), intersection.begin());
      intersection.resize(new_end - intersection.begin());
      total += intersection.size();
    }
  }
  total = total / 6; // each triangle counted 6 times
  if (total != test_total)
    std::cout << total << " != " << test_total << std::endl;
  return total == test_total;
}

int main(int argc, char *argv[]) {
  ArgvPack pack = gapbs_mh::ParseMhOptions(argc, argv);
  int mh_argc = static_cast<int>(pack.args.size());
  char **mh_argv = pack.args.data();

  CLApp cli(mh_argc, mh_argv, "triangle-count (multi-host)");
  if (!cli.ParseArgs())
    return -1;

  uint32_t hosts = pack.opts.hosts;
  uint32_t host_id = pack.opts.host_id;

  struct shm_arg arg;
  memset(&arg, 0, sizeof(arg));
  arg.host_id = host_id;
  arg.host_num = hosts;
  arg.map_size = pack.opts.shm_mb * 1024ULL * 1024ULL;
  snprintf(arg.dax_dev, DEV_NAME_LENGTH, "%s", pack.opts.dev_path);

  TcSharedState *state = nullptr;

  if (host_id == 0) {
    arg.flags = SHM_ENABLE_RESET;
    int rc = shm_enable(arg);
    if (rc != 0) {
      fprintf(stderr, "shm_enable failed: %d\n", rc);
      return 1;
    }

    state = static_cast<TcSharedState *>(
        shm_malloc_id(kIdTcState, sizeof(TcSharedState)));
    if (!state) {
      perror("shm_malloc_id(state)");
      return 1;
    }
    memset(state, 0, sizeof(*state));
    CxlStore(&state->magic, 0u);
    CxlStore(&state->ready, 0u);
    state->hosts = hosts;

    int user_threads = omp_get_max_threads();
    int max_cores = omp_get_num_procs();
    if (max_cores > 0)
      omp_set_num_threads(max_cores);
    std::cout << "Using " << max_cores << " threads for graph generation..."
              << std::endl;

    std::cout << "[Host 0] Building graph..." << std::endl;
    Builder b(cli);
    Graph g_build = b.MakeGraph();
    if (g_build.directed()) {
      std::cerr << "Input graph is directed but tc requires undirected"
                << std::endl;
      return -2;
    }
    if (WorthRelabelling(g_build))
      g_build = Builder::RelabelByDegree(g_build);
    ComputeDegreePartition(g_build, hosts, state->part_offsets,
                           state->part_degrees, &state->total_degree);
    if (cli.logging_en()) {
      for (uint32_t h = 0; h < hosts; ++h) {
        double pct = state->total_degree == 0
                         ? 0.0
                         : (100.0 * state->part_degrees[h]) /
                               static_cast<double>(state->total_degree);
        std::cout << "TC partition host " << h << " nodes ["
                  << state->part_offsets[h] << ", "
                  << state->part_offsets[h + 1]
                  << ") deg_sum=" << state->part_degrees[h] << " (" << pct
                  << "%)" << std::endl;
      }
    }
    if (gapbs_mh::BuildSharedGraph(g_build, &state->graph) != 0) {
      fprintf(stderr, "failed to build shared graph\n");
      return 1;
    }

    if (user_threads > 0)
      omp_set_num_threads(user_threads);
    std::cout << "Restored " << user_threads << " threads for TC execution."
              << std::endl;

    flush_region(state, sizeof(*state));
    store_fence();
    CxlStore(&state->magic, kTcMagic);
    CxlStore(&state->ready, 1u);
    std::cout << "[Host 0] Initialization complete." << std::endl;
  } else {
    arg.flags = 0;
    int rc = shm_enable(arg);
    if (rc != 0) {
      fprintf(stderr, "shm_enable failed: %d\n", rc);
      return 1;
    }
  }

  state = static_cast<TcSharedState *>(
      ShmAttachRetry(kIdTcState, sizeof(TcSharedState), "tc_state"));
  if (!state)
    return 1;

  // Invalidate any locally cached copy of the state block before reading the
  // segment ids below: a previous run's state at the same offset (identical
  // magic) otherwise supplies stale ids that no longer exist in the segment
  // directory. (bfs_mh.cc has carried this flush all along.)
  flush_region(state, sizeof(*state));
  full_fence();

  WaitForReady(state);

  if (CxlLoad(&state->magic) != kTcMagic) {
    fprintf(stderr, "tc_state magic mismatch\n");
    return 1;
  }

  ShmGraph g;
  flush_region(&state->graph, sizeof(state->graph));
  full_fence();
  if (gapbs_mh::AttachSharedGraph(&state->graph, &g) != 0)
    return 1;
  g.FlushArrays();

  ShmBarrierInit(&state->barrier, hosts, host_id);

  Timer trial_timer;
  double total_seconds = 0.0;

  std::cout << "[Host " << host_id << "] Ready, waiting at initial barrier..."
            << std::endl;
  ShmBarrierWait(&state->barrier, hosts, host_id);
  std::cout << "[Host " << host_id << "] All hosts synchronized, starting TC"
            << std::endl;

  if (host_id == 0)
    g.PrintStats();

  for (int iter = 0; iter < cli.num_trials(); iter++) {
    // Reset counts
    if (host_id == 0) {
      for (uint32_t i = 0; i < hosts; ++i)
        state->counts[i] = 0;
      flush_region(state->counts, sizeof(state->counts));
      store_fence();
    }
    ShmBarrierWait(&state->barrier, hosts, host_id);

    if (host_id == 0)
      trial_timer.Start();
    ShmBarrierWait(&state->barrier, hosts, host_id);

    RunTcOnce(g, state, host_id, hosts);

    ShmBarrierWait(&state->barrier, hosts, host_id);
    if (host_id == 0) {
      trial_timer.Stop();
      size_t total_triangles = 0;
      for (uint32_t i = 0; i < hosts; ++i) {
        flush_region(&state->counts[i], sizeof(uint64_t));
        full_fence();
        total_triangles += state->counts[i];
      }

      PrintTime("Trial Time", trial_timer.Seconds());
      total_seconds += trial_timer.Seconds();

      if (cli.do_analysis() && (iter == (cli.num_trials() - 1)))
        PrintTriangleStats(total_triangles);
      if (cli.do_verify() && (iter == (cli.num_trials() - 1))) {
        bool ok = TCVerifier(g, total_triangles);
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

  sync_stats_dump("tc");
  return 0;
}
