#ifndef GAPBS_MH_SHM_GRAPH_H
#define GAPBS_MH_SHM_GRAPH_H

#include <stdint.h>
#include <string.h>

#include "../../gapbs/src/benchmark.h"
#include "../../gapbs/src/graph.h"

#include "shm_support.h"

namespace gapbs_mh {

static const uint64_t kIdGraphOutOffsets = 0x100;
static const uint64_t kIdGraphOutNeighbors = 0x101;
static const uint64_t kIdGraphInOffsets = 0x102;
static const uint64_t kIdGraphInNeighbors = 0x103;

struct ShmGraphInfo {
  int64_t num_nodes;
  int64_t num_edges;
  uint32_t directed;
  uint32_t _pad;
  uint64_t out_offsets_id;
  uint64_t out_neighbors_id;
  uint64_t in_offsets_id;
  uint64_t in_neighbors_id;
};

struct NeighborRange {
  NodeID *begin_;
  NodeID *end_;
  NodeID *begin() const { return begin_; }
  NodeID *end() const { return end_; }
};

class ShmGraph {
public:
  ShmGraph()
      : info_(nullptr), out_offsets_(nullptr), out_neighbors_(nullptr),
        in_offsets_(nullptr), in_neighbors_(nullptr) {}

  ShmGraph(const ShmGraphInfo *info, SGOffset *out_offsets,
           NodeID *out_neighbors, SGOffset *in_offsets, NodeID *in_neighbors)
      : info_(info), out_offsets_(out_offsets), out_neighbors_(out_neighbors),
        in_offsets_(in_offsets), in_neighbors_(in_neighbors) {}

  int64_t num_nodes() const { return info_->num_nodes; }
  int64_t num_edges() const { return info_->num_edges; }
  bool directed() const { return info_->directed != 0; }

  int64_t num_edges_directed() const {
    return directed() ? num_edges() : 2 * num_edges();
  }

  int64_t out_degree(NodeID n) const {
    return static_cast<int64_t>(out_offsets_[n + 1] - out_offsets_[n]);
  }

  int64_t in_degree(NodeID n) const {
    return static_cast<int64_t>(in_offsets_[n + 1] - in_offsets_[n]);
  }

  NeighborRange out_neigh(NodeID n) const {
    return NeighborRange{out_neighbors_ + out_offsets_[n],
                         out_neighbors_ + out_offsets_[n + 1]};
  }

  // Offset version for Afforest sparse sampling - skips first 'offset'
  // neighbors
  NeighborRange out_neigh(NodeID n, int64_t offset) const {
    NodeID *begin = out_neighbors_ + out_offsets_[n];
    NodeID *end = out_neighbors_ + out_offsets_[n + 1];
    begin += std::min(static_cast<int64_t>(end - begin), offset);
    return NeighborRange{begin, end};
  }

  NeighborRange in_neigh(NodeID n) const {
    return NeighborRange{in_neighbors_ + in_offsets_[n],
                         in_neighbors_ + in_offsets_[n + 1]};
  }

  // Offset version for Afforest sparse sampling
  NeighborRange in_neigh(NodeID n, int64_t offset) const {
    NodeID *begin = in_neighbors_ + in_offsets_[n];
    NodeID *end = in_neighbors_ + in_offsets_[n + 1];
    begin += std::min(static_cast<int64_t>(end - begin), offset);
    return NeighborRange{begin, end};
  }

  Range<NodeID> vertices() const {
    return Range<NodeID>(static_cast<NodeID>(num_nodes()));
  }

  void PrintStats() const {
    std::cout << "Graph has " << num_nodes() << " nodes and " << num_edges()
              << " ";
    if (!directed())
      std::cout << "un";
    std::cout << "directed edges for degree: " << num_edges() / num_nodes()
              << std::endl;
  }

  // Flush all graph arrays to invalidate stale CPU cache (for CXL)
  void FlushArrays() const {
    if (info_ && out_offsets_ && out_neighbors_) {
      size_t nodes = static_cast<size_t>(info_->num_nodes);
      size_t edges = static_cast<size_t>(info_->num_edges);
      if (!info_->directed)
        edges *= 2; // undirected = 2x edges in CSR

      flush_region(out_offsets_, (nodes + 1) * sizeof(SGOffset));
      flush_region(out_neighbors_, edges * sizeof(NodeID));

      if (info_->directed && in_offsets_ != out_offsets_) {
        flush_region(in_offsets_, (nodes + 1) * sizeof(SGOffset));
        flush_region(in_neighbors_, edges * sizeof(NodeID));
      }
      full_fence();
    }
  }

private:
  const ShmGraphInfo *info_;
  SGOffset *out_offsets_;
  NodeID *out_neighbors_;
  SGOffset *in_offsets_;
  NodeID *in_neighbors_;
};

inline int BuildSharedGraph(const Graph &g, ShmGraphInfo *info) {
  info->num_nodes = g.num_nodes();
  info->num_edges = g.num_edges();
  info->directed = g.directed() ? 1u : 0u;
  info->out_offsets_id = kIdGraphOutOffsets;
  info->out_neighbors_id = kIdGraphOutNeighbors;
  info->in_offsets_id = g.directed() ? kIdGraphInOffsets : kIdGraphOutOffsets;
  info->in_neighbors_id =
      g.directed() ? kIdGraphInNeighbors : kIdGraphOutNeighbors;

  size_t nodes = static_cast<size_t>(g.num_nodes());
  size_t edges = static_cast<size_t>(g.num_edges_directed());

  SGOffset *out_offsets = static_cast<SGOffset *>(
      shm_malloc_id(info->out_offsets_id, (nodes + 1) * sizeof(SGOffset)));
  if (!out_offsets)
    return -1;

  NodeID *out_neighbors = static_cast<NodeID *>(
      shm_malloc_id(info->out_neighbors_id, edges * sizeof(NodeID)));
  if (!out_neighbors)
    return -1;

  auto out_offs = g.VertexOffsets(false);
  memcpy(out_offsets, out_offs.data(), (nodes + 1) * sizeof(SGOffset));

#pragma omp parallel for schedule(dynamic, 1024)
  for (NodeID u = 0; u < g.num_nodes(); u++) {
    SGOffset off = out_offs[u];
    NodeID *dst = out_neighbors + off;
    for (NodeID v : g.out_neigh(u))
      *dst++ = v;
  }

  if (g.directed()) {
    SGOffset *in_offsets = static_cast<SGOffset *>(
        shm_malloc_id(info->in_offsets_id, (nodes + 1) * sizeof(SGOffset)));
    if (!in_offsets)
      return -1;

    NodeID *in_neighbors = static_cast<NodeID *>(
        shm_malloc_id(info->in_neighbors_id, edges * sizeof(NodeID)));
    if (!in_neighbors)
      return -1;

    auto in_offs = g.VertexOffsets(true);
    memcpy(in_offsets, in_offs.data(), (nodes + 1) * sizeof(SGOffset));

#pragma omp parallel for schedule(dynamic, 1024)
    for (NodeID u = 0; u < g.num_nodes(); u++) {
      SGOffset off = in_offs[u];
      NodeID *dst = in_neighbors + off;
      for (NodeID v : g.in_neigh(u))
        *dst++ = v;
    }
  }

  return 0;
}

inline int AttachSharedGraph(const ShmGraphInfo *info, ShmGraph *out_graph) {
  SGOffset *out_offsets = static_cast<SGOffset *>(
      ShmAttachRetry(info->out_offsets_id, 0, "graph_out_offsets"));
  if (!out_offsets)
    return -1;

  NodeID *out_neighbors = static_cast<NodeID *>(
      ShmAttachRetry(info->out_neighbors_id, 0, "graph_out_neighbors"));
  if (!out_neighbors)
    return -1;

  SGOffset *in_offsets = out_offsets;
  NodeID *in_neighbors = out_neighbors;
  if (info->directed) {
    in_offsets = static_cast<SGOffset *>(
        ShmAttachRetry(info->in_offsets_id, 0, "graph_in_offsets"));
    if (!in_offsets)
      return -1;

    in_neighbors = static_cast<NodeID *>(
        ShmAttachRetry(info->in_neighbors_id, 0, "graph_in_neighbors"));
    if (!in_neighbors)
      return -1;
  }

  *out_graph =
      ShmGraph(info, out_offsets, out_neighbors, in_offsets, in_neighbors);
  return 0;
}

// Host 0, after BuildSharedGraph() and before it advertises ready: write the
// CSR arrays back to the shared media, so a peer that sees ready and then
// invalidates its own cached copy reads this graph.
inline int PublishSharedGraph(const ShmGraphInfo *info) {
  ShmGraph g;
  if (AttachSharedGraph(info, &g) != 0)
    return -1;
  g.FlushArrays();
  return 0;
}

// ============================================================================
// Weighted Graph Support (for SSSP)
// ============================================================================

static const uint64_t kIdWGraphOutOffsets = 0x200;
static const uint64_t kIdWGraphOutNeighbors = 0x201;
static const uint64_t kIdWGraphInOffsets = 0x202;
static const uint64_t kIdWGraphInNeighbors = 0x203;

struct ShmWGraphInfo {
  int64_t num_nodes;
  int64_t num_edges;
  uint32_t directed;
  uint32_t _pad;
  uint64_t out_offsets_id;
  uint64_t out_neighbors_id;
  uint64_t in_offsets_id;
  uint64_t in_neighbors_id;
};

struct WNeighborRange {
  WNode *begin_;
  WNode *end_;
  WNode *begin() const { return begin_; }
  WNode *end() const { return end_; }
};

class ShmWGraph {
public:
  ShmWGraph()
      : info_(nullptr), out_offsets_(nullptr), out_neighbors_(nullptr),
        in_offsets_(nullptr), in_neighbors_(nullptr) {}

  ShmWGraph(const ShmWGraphInfo *info, SGOffset *out_offsets,
            WNode *out_neighbors, SGOffset *in_offsets, WNode *in_neighbors)
      : info_(info), out_offsets_(out_offsets), out_neighbors_(out_neighbors),
        in_offsets_(in_offsets), in_neighbors_(in_neighbors) {}

  int64_t num_nodes() const { return info_->num_nodes; }
  int64_t num_edges() const { return info_->num_edges; }
  bool directed() const { return info_->directed != 0; }

  int64_t num_edges_directed() const {
    return directed() ? num_edges() : 2 * num_edges();
  }

  int64_t out_degree(NodeID n) const {
    return static_cast<int64_t>(out_offsets_[n + 1] - out_offsets_[n]);
  }

  int64_t in_degree(NodeID n) const {
    return static_cast<int64_t>(in_offsets_[n + 1] - in_offsets_[n]);
  }

  WNeighborRange out_neigh(NodeID n) const {
    return WNeighborRange{out_neighbors_ + out_offsets_[n],
                          out_neighbors_ + out_offsets_[n + 1]};
  }

  WNeighborRange in_neigh(NodeID n) const {
    return WNeighborRange{in_neighbors_ + in_offsets_[n],
                          in_neighbors_ + in_offsets_[n + 1]};
  }

  Range<NodeID> vertices() const {
    return Range<NodeID>(static_cast<NodeID>(num_nodes()));
  }

  void PrintStats() const {
    std::cout << "Graph has " << num_nodes() << " nodes and " << num_edges()
              << " ";
    if (!directed())
      std::cout << "un";
    std::cout << "directed edges for degree: " << num_edges() / num_nodes()
              << std::endl;
  }

  // Flush all graph arrays to invalidate stale CPU cache (for CXL)
  void FlushArrays() const {
    if (info_ && out_offsets_ && out_neighbors_) {
      size_t nodes = static_cast<size_t>(info_->num_nodes);
      size_t edges = static_cast<size_t>(info_->num_edges);
      if (!info_->directed)
        edges *= 2;

      flush_region(out_offsets_, (nodes + 1) * sizeof(SGOffset));
      flush_region(out_neighbors_, edges * sizeof(WNode));

      if (info_->directed && in_offsets_ != out_offsets_) {
        flush_region(in_offsets_, (nodes + 1) * sizeof(SGOffset));
        flush_region(in_neighbors_, edges * sizeof(WNode));
      }
      full_fence();
    }
  }

private:
  const ShmWGraphInfo *info_;
  SGOffset *out_offsets_;
  WNode *out_neighbors_;
  SGOffset *in_offsets_;
  WNode *in_neighbors_;
};

inline int BuildSharedWGraph(const WGraph &g, ShmWGraphInfo *info) {
  info->num_nodes = g.num_nodes();
  info->num_edges = g.num_edges();
  info->directed = g.directed() ? 1u : 0u;
  info->out_offsets_id = kIdWGraphOutOffsets;
  info->out_neighbors_id = kIdWGraphOutNeighbors;
  info->in_offsets_id = g.directed() ? kIdWGraphInOffsets : kIdWGraphOutOffsets;
  info->in_neighbors_id =
      g.directed() ? kIdWGraphInNeighbors : kIdWGraphOutNeighbors;

  size_t nodes = static_cast<size_t>(g.num_nodes());
  size_t edges = static_cast<size_t>(g.num_edges_directed());

  SGOffset *out_offsets = static_cast<SGOffset *>(
      shm_malloc_id(info->out_offsets_id, (nodes + 1) * sizeof(SGOffset)));
  if (!out_offsets)
    return -1;

  WNode *out_neighbors = static_cast<WNode *>(
      shm_malloc_id(info->out_neighbors_id, edges * sizeof(WNode)));
  if (!out_neighbors)
    return -1;

  auto out_offs = g.VertexOffsets(false);
  memcpy(out_offsets, out_offs.data(), (nodes + 1) * sizeof(SGOffset));

#pragma omp parallel for schedule(dynamic, 1024)
  for (NodeID u = 0; u < g.num_nodes(); u++) {
    SGOffset off = out_offs[u];
    WNode *dst = out_neighbors + off;
    for (WNode wn : g.out_neigh(u))
      *dst++ = wn;
  }

  if (g.directed()) {
    SGOffset *in_offsets = static_cast<SGOffset *>(
        shm_malloc_id(info->in_offsets_id, (nodes + 1) * sizeof(SGOffset)));
    if (!in_offsets)
      return -1;

    WNode *in_neighbors = static_cast<WNode *>(
        shm_malloc_id(info->in_neighbors_id, edges * sizeof(WNode)));
    if (!in_neighbors)
      return -1;

    auto in_offs = g.VertexOffsets(true);
    memcpy(in_offsets, in_offs.data(), (nodes + 1) * sizeof(SGOffset));

#pragma omp parallel for schedule(dynamic, 1024)
    for (NodeID u = 0; u < g.num_nodes(); u++) {
      SGOffset off = in_offs[u];
      WNode *dst = in_neighbors + off;
      for (WNode wn : g.in_neigh(u))
        *dst++ = wn;
    }
  }

  return 0;
}

inline int AttachSharedWGraph(const ShmWGraphInfo *info, ShmWGraph *out_graph) {
  SGOffset *out_offsets = static_cast<SGOffset *>(
      ShmAttachRetry(info->out_offsets_id, 0, "wgraph_out_offsets"));
  if (!out_offsets)
    return -1;

  WNode *out_neighbors = static_cast<WNode *>(
      ShmAttachRetry(info->out_neighbors_id, 0, "wgraph_out_neighbors"));
  if (!out_neighbors)
    return -1;

  SGOffset *in_offsets = out_offsets;
  WNode *in_neighbors = out_neighbors;
  if (info->directed) {
    in_offsets = static_cast<SGOffset *>(
        ShmAttachRetry(info->in_offsets_id, 0, "wgraph_in_offsets"));
    if (!in_offsets)
      return -1;

    in_neighbors = static_cast<WNode *>(
        ShmAttachRetry(info->in_neighbors_id, 0, "wgraph_in_neighbors"));
    if (!in_neighbors)
      return -1;
  }

  *out_graph =
      ShmWGraph(info, out_offsets, out_neighbors, in_offsets, in_neighbors);
  return 0;
}

} // namespace gapbs_mh

#endif
