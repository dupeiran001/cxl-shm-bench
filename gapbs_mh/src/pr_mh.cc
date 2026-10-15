// Copyright (c) 2015, The Regents of the University of California (Regents)
// See LICENSE.txt for license details
// Multi-Host CXL Version - PageRank

#include <cmath>
#include <cstdio>
#include <iostream>
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

static const uint64_t kIdPrState = 0x400;
static const uint64_t kIdPrScores = 0x401;
static const uint64_t kIdPrOutgoing = 0x402;
static const uint32_t kPrMagic = 0x4D485052; // "MHPR"

typedef float ScoreT;
const float kDamp = 0.85f;

struct PrSharedState {
  uint32_t magic;
  uint32_t hosts;
  uint32_t ready;
  uint32_t done;
  ShmBarrier barrier;
  ShmGraphInfo graph;
  uint64_t scores_id;
  uint64_t outgoing_id;
  gapbs_mh::HostSlot<double> errors[MAX_HOST_NUM]; // one line per host
};

static void WaitForReady(const PrSharedState *st) {
  while (CxlLoad(&st->magic) != kPrMagic) {
    usleep(1000);
  }
  while (CxlLoad(&st->ready) == 0) {
    usleep(1000);
  }
}

static void InitPrArrays(const ShmGraph &g, PrSharedState *st, ScoreT *scores,
                         ScoreT *outgoing, ScoreT init_score) {
#pragma omp parallel for schedule(static)
  for (NodeID n = 0; n < g.num_nodes(); n++) {
    scores[n] = init_score;
    outgoing[n] = init_score / g.out_degree(n);
  }
  CxlStore(&st->done, 0u);

  // Flush arrays for visibility
  flush_region(scores, g.num_nodes() * sizeof(ScoreT));
  flush_region(outgoing, g.num_nodes() * sizeof(ScoreT));
  flush_region(st, sizeof(*st));
  store_fence();
}

static void RunPrOnce(const ShmGraph &g, PrSharedState *st, ScoreT *scores,
                      ScoreT *outgoing, uint32_t host_id, uint32_t hosts,
                      int max_iters, double epsilon, uint32_t sync_k,
                      bool logging_enabled) {
  const int64_t total_nodes = g.num_nodes();
  const NodeID start = static_cast<NodeID>(
      (static_cast<int64_t>(host_id) * total_nodes) / hosts);
  const NodeID end = static_cast<NodeID>(
      (static_cast<int64_t>(host_id + 1) * total_nodes) / hosts);

  const ScoreT base_score = (1.0f - kDamp) / g.num_nodes();
  const size_t total_bytes = static_cast<size_t>(total_nodes) * sizeof(ScoreT);

  uint32_t k = sync_k == 0 ? 1 : sync_k;
  for (int iter = 0; iter < max_iters; iter++) {
    Timer iter_timer;
    iter_timer.Start();
    // Invalidate cached outgoing values so we see remote updates.
    flush_region(outgoing, total_bytes);
    full_fence();

    double local_error = 0.0;
#pragma omp parallel for reduction(+ : local_error) schedule(dynamic, 16384)
    for (NodeID u = start; u < end; u++) {
      ScoreT incoming_total = 0;
      for (NodeID v : g.in_neigh(u))
        incoming_total += outgoing[v];
      ScoreT old_score = scores[u];
      ScoreT new_score = base_score + kDamp * incoming_total;
      scores[u] = new_score;
      local_error += fabs(new_score - old_score);
      outgoing[u] = new_score / g.out_degree(u);
    }

    // Publish this host's updated scores/outgoing.
    flush_region(scores + start, (end - start) * sizeof(ScoreT));
    flush_region(outgoing + start, (end - start) * sizeof(ScoreT));
    store_fence();

    bool do_sync = (k == 1) || ((iter + 1) % k == 0) || (iter == max_iters - 1);
    double total_error = -1.0;
    if (do_sync) {
      // Store local error and sync
      st->errors[host_id].v = local_error;
      flush_region(&st->errors[host_id], sizeof(st->errors[host_id]));
      store_fence();
      ShmBarrierWait(&st->barrier, hosts, host_id);

      // Host 0 aggregates
      if (host_id == 0) {
        total_error = 0.0;
        for (uint32_t i = 0; i < hosts; ++i) {
          flush_region(&st->errors[i], sizeof(st->errors[i]));
          full_fence();
          total_error += st->errors[i].v;
        }
        CxlStore(&st->done, total_error < epsilon ? 1u : 0u);
      }

      ShmBarrierWait(&st->barrier, hosts, host_id);
    }

    iter_timer.Stop();
    if (logging_enabled && host_id == 0) {
      if (do_sync) {
        printf("iter %4d  local=%10.5lf  total=%10.5lf  time=%10.5lf  sync=1\n",
               iter, local_error, total_error, iter_timer.Seconds());
      } else {
        printf("iter %4d  local=%10.5lf  total=NA        time=%10.5lf  sync=0\n",
               iter, local_error, iter_timer.Seconds());
      }
    }

    if (do_sync && CxlLoad(&st->done))
      break;
  }
}

static void PrintTopScores(const ShmGraph &g, const ScoreT *scores) {
  std::vector<std::pair<NodeID, ScoreT>> score_pairs(g.num_nodes());
  for (NodeID n = 0; n < g.num_nodes(); n++) {
    score_pairs[n] = std::make_pair(n, scores[n]);
  }
  int k = 5;
  std::vector<std::pair<ScoreT, NodeID>> top_k = TopK(score_pairs, k);
  for (auto kvp : top_k)
    std::cout << kvp.second << ":" << kvp.first << std::endl;
}

static bool PRVerifier(const ShmGraph &g, const ScoreT *scores,
                       double target_error) {
  const ScoreT base_score = (1.0f - kDamp) / g.num_nodes();
  pvector<ScoreT> incoming_sums(g.num_nodes(), 0);
  double error = 0;
  for (NodeID u : g.vertices()) {
    ScoreT outgoing_contrib = scores[u] / g.out_degree(u);
    for (NodeID v : g.out_neigh(u))
      incoming_sums[v] += outgoing_contrib;
  }
  for (NodeID n : g.vertices()) {
    error += fabs(base_score + kDamp * incoming_sums[n] - scores[n]);
    incoming_sums[n] = 0;
  }
  PrintTime("Total Error", error);
  return error < target_error;
}

int main(int argc, char *argv[]) {
  ArgvPack pack = gapbs_mh::ParseMhOptions(argc, argv);
  int mh_argc = static_cast<int>(pack.args.size());
  char **mh_argv = pack.args.data();

  CLPageRank cli(mh_argc, mh_argv, "pagerank (multi-host)", 1e-4, 20);
  if (!cli.ParseArgs())
    return -1;

  uint32_t hosts = pack.opts.hosts;
  uint32_t host_id = pack.opts.host_id;
  int user_threads = omp_get_max_threads();
  int max_cores = omp_get_num_procs();

  // Setup SHM arg
  struct shm_arg arg;
  memset(&arg, 0, sizeof(arg));
  arg.host_id = host_id;
  arg.host_num = hosts;
  arg.map_size = pack.opts.shm_mb * 1024ULL * 1024ULL;
  snprintf(arg.dax_dev, DEV_NAME_LENGTH, "%s", pack.opts.dev_path);

  // Pointers for shared state
  PrSharedState *state = nullptr;
  ScoreT *scores = nullptr;
  ScoreT *outgoing = nullptr;

  if (host_id == 0) {
    // Host 0: Enable & Init
    arg.flags = SHM_ENABLE_RESET;
    int rc = shm_enable(arg);
    if (rc != 0) {
      fprintf(stderr, "shm_enable failed: %d\n", rc);
      return 1;
    }

    state = static_cast<PrSharedState *>(
        shm_malloc_id(kIdPrState, sizeof(PrSharedState)));
    if (!state) {
      perror("shm_malloc_id(state)");
      return 1;
    }
    memset(state, 0, sizeof(*state));
    CxlStore(&state->magic, 0u);
    CxlStore(&state->ready, 0u);
    state->hosts = hosts;
    state->scores_id = kIdPrScores;
    state->outgoing_id = kIdPrOutgoing;

    if (max_cores > 0)
      omp_set_num_threads(max_cores);
    std::cout << "[Host 0] Building graph..." << std::endl;
    Builder b(cli);
    Graph g_build = b.MakeGraph();
    if (gapbs_mh::BuildSharedGraph(g_build, &state->graph) != 0) {
      fprintf(stderr, "failed to build shared graph\n");
      return 1;
    }

    size_t nodes = static_cast<size_t>(g_build.num_nodes());
    scores = static_cast<ScoreT *>(
        shm_malloc_id(kIdPrScores, nodes * sizeof(ScoreT)));
    outgoing = static_cast<ScoreT *>(
        shm_malloc_id(kIdPrOutgoing, nodes * sizeof(ScoreT)));
    if (!scores || !outgoing) {
      perror("shm_malloc_id(pr arrays)");
      return 1;
    }

    ShmGraph shm_graph;
    if (gapbs_mh::AttachSharedGraph(&state->graph, &shm_graph) != 0)
      return 1;

    ScoreT init_score = 1.0f / shm_graph.num_nodes();
    std::cout << "[Host 0] Initializing PR arrays..." << std::endl;
    InitPrArrays(shm_graph, state, scores, outgoing, init_score);
    if (user_threads > 0)
      omp_set_num_threads(user_threads);

    CxlStore(&state->magic, kPrMagic);
    CxlStore(&state->ready, 1u);
    std::cout << "[Host 0] Initialization complete." << std::endl;
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
  state = static_cast<PrSharedState *>(
      ShmAttachRetry(kIdPrState, sizeof(PrSharedState), "pr_state"));
  if (!state)
    return 1;

  // Invalidate any locally cached copy of the state block before reading the
  // segment ids below: a previous run's state at the same offset (identical
  // magic) otherwise supplies stale ids that no longer exist in the segment
  // directory. (bfs_mh.cc has carried this flush all along.)
  flush_region(state, sizeof(*state));
  full_fence();

  WaitForReady(state);

  if (CxlLoad(&state->magic) != kPrMagic) {
    fprintf(stderr, "pr_state magic mismatch\n");
    return 1;
  }

  ShmGraph g;
  flush_region(&state->graph, sizeof(state->graph));
  full_fence();
  if (gapbs_mh::AttachSharedGraph(&state->graph, &g) != 0)
    return 1;
  g.FlushArrays();

  if (ShmBarrierInit(&state->barrier, hosts, host_id) != 0)
    return 1;

  scores =
      static_cast<ScoreT *>(ShmAttachRetry(state->scores_id, 0, "pr_scores"));
  outgoing = static_cast<ScoreT *>(
      ShmAttachRetry(state->outgoing_id, 0, "pr_outgoing"));
  if (!scores || !outgoing)
    return 1;

  Timer trial_timer;
  double total_seconds = 0.0;

  std::cout << "[Host " << host_id << "] Ready, waiting at initial barrier..."
            << std::endl;
  ShmBarrierWait(&state->barrier, hosts, host_id);
  std::cout << "[Host " << host_id << "] All hosts synchronized, starting PR"
            << std::endl;

  if (host_id == 0)
    g.PrintStats();

  for (int iter = 0; iter < cli.num_trials(); iter++) {
    if (host_id == 0) {
      ScoreT init_score = 1.0f / g.num_nodes();
      if (max_cores > 0)
        omp_set_num_threads(max_cores);
      InitPrArrays(g, state, scores, outgoing, init_score);
      if (user_threads > 0)
        omp_set_num_threads(user_threads);
    }
    ShmBarrierWait(&state->barrier, hosts, host_id);

    if (host_id == 0)
      trial_timer.Start();
    ShmBarrierWait(&state->barrier, hosts, host_id);

    RunPrOnce(g, state, scores, outgoing, host_id, hosts, cli.max_iters(),
              cli.tolerance(), pack.opts.sync_k,
              cli.logging_en() && host_id == 0);

    ShmBarrierWait(&state->barrier, hosts, host_id);
    if (host_id == 0) {
      trial_timer.Stop();
      PrintTime("Trial Time", trial_timer.Seconds());
      total_seconds += trial_timer.Seconds();
      if (cli.do_analysis() && (iter == (cli.num_trials() - 1)))
        PrintTopScores(g, scores);
      if (cli.do_verify() && (iter == (cli.num_trials() - 1))) {
        flush_region(scores, g.num_nodes() * sizeof(ScoreT));
        full_fence();
        bool ok = PRVerifier(g, scores, cli.tolerance());
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

  sync_stats_dump("pr");
  return 0;
}
