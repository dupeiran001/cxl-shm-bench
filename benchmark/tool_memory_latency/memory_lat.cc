#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <numa.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/usage.h"
#include "absl/log/absl_check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "util/auto_release.h"
#include "util/init.h"
#include "util/perf.h"
#include "util/status_macros.h"
#include "util/string_extension.h"
#include "util/x86_64/timestamp.h"
#include "util/xxhash.h"

ABSL_FLAG(uint32_t, m, 0, "memory node");
ABSL_FLAG(uint32_t, n, 0, "cpu node");
ABSL_FLAG(uint32_t, lat_low, 0, "low bound of memory latency");
ABSL_FLAG(uint32_t, lat_high, 0, "high bound of memory latency");
ABSL_FLAG(uint32_t, lat_step, UINT32_MAX, "steps when we measure tail latency");
ABSL_FLAG(double, tail_lat_pct, 0, "total memory access size");
ABSL_FLAG(std::string, stride, "1T", "pointer shuffling stride");
ABSL_FLAG(std::string, sz, "1M", "working set size");
ABSL_FLAG(std::string, total_sz, "512M", "total memory access size");
ABSL_FLAG(bool, sequential, false, "Should we access the memory?");
ABSL_FLAG(bool, numa_affinity, true, "should we pin cpu and memory?");
ABSL_FLAG(bool, dependent, true,
          "use dependent pointer-chase for per-access operations");
ABSL_FLAG(std::string, test, "",
          "available options include rd, store_flush, flush_clean, ring_bw");
ABSL_FLAG(std::string, flush, "clwb", "flush instruction: clwb or clflushopt");
ABSL_FLAG(std::string, dax_dev, "",
          "path to a dax device to mmap and use as main memory");
ABSL_FLAG(std::string, dax_offset, "0",
          "byte offset into dax device (supports K/M/G/T suffix)");
ABSL_FLAG(uint32_t, parallel, 1,
          "number of concurrent worker threads (rd, ring_bw)");
ABSL_FLAG(std::string, payload_sz, "0",
          "payload size per node in ring_bw (0 disables payload access)");
ABSL_FLAG(double, seconds, 0,
          "run duration in seconds (ring_bw only; overrides total_sz)");

namespace {
constexpr uint32_t kPageStride = 128;
constexpr uint32_t kPageShift = 12;
constexpr uint32_t kPageSize = 1ULL << kPageShift;
constexpr uint32_t kCacheLineSize = 64;
constexpr float kBytePerMsToMBPerSec = 1000.0 / 1024 / 1024;

constexpr uint32_t kPadNum = 7;

volatile uint64_t dummy_sum = 0;

enum class FlushMethod { kClwb, kClflushopt };

struct TraverseElem {
  struct TraverseElem *next_;
  uint64_t pad_[kPadNum];
};
// static_assert(sizeof(TraverseElem) == kCacheLineSize);

struct RingNode {
  RingNode *next_;
  void *payload_;
  uint64_t pad_[6];
};
static_assert(sizeof(RingNode) == kCacheLineSize);

inline uint64_t AlignUp(uint64_t x, uint64_t a) {
  return (x + a - 1) & ~(a - 1);
}

absl::Status GeneratePattern(std::vector<uint64_t> &order, uint64_t stride,
                             bool sequential) {
  uint64_t num = order.size();
  for (uint64_t i = 0; i < num; i++) {
    order.at(i) = i;
  }
  if (!sequential) {
    for (uint64_t i = 0; i < num; i += (stride / 2)) {
      std::shuffle(order.begin() + i,
                   order.begin() + i +
                       ((num - i) > stride ? stride : (num - i)),
                   std::default_random_engine(0));
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<FlushMethod> ParseFlushMethod(absl::string_view name) {
  if (name == "clwb") {
    return FlushMethod::kClwb;
  }
  if (name == "clflushopt") {
    return FlushMethod::kClflushopt;
  }
  return absl::InvalidArgumentError("flush must be clwb or clflushopt");
}

inline void FlushLine(void *addr, FlushMethod method) {
  if (method == FlushMethod::kClwb) {
    asm volatile("clwb (%0)" : : "r"(addr) : "memory");
  } else {
    asm volatile("clflushopt (%0)" : : "r"(addr) : "memory");
  }
}

inline uint64_t ReadTsc() {
  unsigned int lo, hi;
  asm volatile("lfence\nrdtsc" : "=a"(lo), "=d"(hi)::"memory");
  return (static_cast<uint64_t>(hi) << 32) | lo;
}

inline void DoNotOptimizeU64(uint64_t value) {
  asm volatile("" : : "r"(value) : "memory");
}

absl::Status BuildOrder(uint64_t elem_num, uint64_t shuffle_stride,
                        bool sequential, std::vector<uint64_t> *order) {
  order->assign(elem_num, 0);
  for (uint64_t i = 0; i < elem_num; i++) {
    order->at(i) = i;
  }
  if (sequential || shuffle_stride < 2) {
    return absl::OkStatus();
  }
  return GeneratePattern(*order, shuffle_stride, sequential);
}

void BuildRing(TraverseElem *array, const std::vector<uint64_t> &order) {
  for (uint64_t i = 1; i < order.size(); i++) {
    array[order.at(i - 1)].next_ = &array[order.at(i)];
  }
  array[order.at(order.size() - 1)].next_ = &array[order.at(0)];
}

void BuildRingSlice(TraverseElem *array, const std::vector<uint64_t> &order,
                    uint64_t begin, uint64_t end) {
  if (begin >= end || end > order.size()) {
    return;
  }
  for (uint64_t i = begin + 1; i < end; ++i) {
    array[order.at(i - 1)].next_ = &array[order.at(i)];
  }
  array[order.at(end - 1)].next_ = &array[order.at(begin)];
}

void BuildRing(RingNode *array, const std::vector<uint64_t> &order) {
  for (uint64_t i = 1; i < order.size(); i++) {
    array[order.at(i - 1)].next_ = &array[order.at(i)];
  }
  array[order.at(order.size() - 1)].next_ = &array[order.at(0)];
}

absl::StatusOr<void *> ThrashMemoryBegin(int32_t node, int64_t memory_size) {
  auto buffer = numa_alloc_onnode(memory_size, node);

  if (!buffer) {
    return absl::InternalError("alloc thrashing memory");
  }

  memset(buffer, 0, memory_size);
  auto xx64_val = util::XXHash64::hash(buffer, memory_size, 0);
  LOG(INFO) << absl::StrFormat("calcualted xxhash64 for 0x%lx bytes, got %lx",
                               memory_size, xx64_val);
  return buffer;
}

void ThrashMemoryEnd(void *addr, int64_t memory_size) {
  numa_free(addr, memory_size);
}

} // namespace

namespace tools {

class MemoryLatTest {
public:
  MemoryLatTest(bool numa_affinity, int memory_node, int cpu_node, uint64_t wss,
                uint64_t total_sz, uint64_t shuffle_stride,
                absl::string_view dax_dev, uint64_t dax_offset)
      : numa_affinity_(numa_affinity), memory_node_(memory_node),
        cpu_node_(cpu_node), wss_(wss), total_sz_(total_sz),
        iter_(total_sz_ / wss_), shuffle_stride_(shuffle_stride),
        dax_dev_(dax_dev), dax_offset_(dax_offset) {};
  absl::Status RunPointerChase(uint32_t parallel, uint32_t lat_low,
                               uint32_t lat_high,
                               uint32_t lat_step, bool sequential,
                               bool force_random);
  absl::Status RunRingBandwidth(uint32_t parallel, uint64_t payload_sz,
                                bool sequential, double seconds);
  absl::Status RunStoreFlush(bool sequential, FlushMethod flush_method,
                             uint32_t lat_low, uint32_t lat_high,
                             uint32_t lat_step, bool dependent);
  absl::Status RunFlushClean(bool sequential, FlushMethod flush_method,
                             uint32_t lat_low, uint32_t lat_high,
                             uint32_t lat_step, bool dependent);

private:
  bool numa_affinity_;
  int memory_node_;
  int cpu_node_;
  uint64_t wss_;
  uint64_t total_sz_;
  uint64_t iter_;
  uint64_t shuffle_stride_;
  std::string dax_dev_;
  uint64_t dax_offset_;
  absl::Status PinCore();
  absl::StatusOr<void *> AllocateBuffer(uint64_t len, int *dax_fd,
                                        void **map_base, size_t *map_len);
  void ReleaseBuffer(void *buffer, uint64_t len, int dax_fd, void *map_base,
                     size_t map_len);
  absl::Status ValidateSweep(uint32_t lat_low, uint32_t lat_high,
                             uint32_t lat_step);
};

absl::Status MemoryLatTest::RunPointerChase(uint32_t parallel, uint32_t lat_low,
                                            uint32_t lat_high, uint32_t lat_step,
                                            bool sequential, bool force_random) {
  if (parallel == 0) {
    return absl::InvalidArgumentError("--parallel must be >= 1");
  }
  if (numa_affinity_) {
    auto st = PinCore();
    if (!st.ok()) {
      return st;
    }
  }
  RETURN_IF_ERROR(ValidateSweep(lat_low, lat_high, lat_step));
  if (iter_ == 0) {
    return absl::InvalidArgumentError("total_sz must be >= sz");
  }

  uint64_t elem_num = wss_ / sizeof(struct TraverseElem);
  if (wss_ % sizeof(struct TraverseElem)) {
    return absl::InvalidArgumentError("odd wss");
  }

  uint64_t stride = shuffle_stride_;
  if (stride > elem_num) {
    stride = elem_num;
  }
  if (force_random) {
    sequential = false;
    stride = elem_num;
  }

  int dax_fd = -1;
  void *map_base = nullptr;
  size_t map_len = 0;
  ASSIGN_OR_RETURN(void *buffer,
                   AllocateBuffer(wss_, &dax_fd, &map_base, &map_len));
  util::ResourceRelease release_fun(
      [&]() { ReleaseBuffer(buffer, wss_, dax_fd, map_base, map_len); });

  memset(buffer, 0, wss_);
  LOG(INFO) << "memory set up";

  std::vector<uint64_t> order;
  RETURN_IF_ERROR(BuildOrder(elem_num, stride, sequential, &order));

  auto working_array = reinterpret_cast<struct TraverseElem *>(buffer);
  std::vector<uint64_t> slice_begins(parallel, 0);
  std::vector<uint64_t> slice_ends(parallel, 0);
  const uint64_t base_chunk = elem_num / parallel;
  const uint64_t remainder = elem_num % parallel;
  uint64_t cursor = 0;
  for (uint32_t tid = 0; tid < parallel; ++tid) {
    const uint64_t count = base_chunk + (tid < remainder ? 1 : 0);
    slice_begins[tid] = cursor;
    slice_ends[tid] = cursor + count;
    cursor += count;
    BuildRingSlice(working_array, order, slice_begins[tid], slice_ends[tid]);
  }
  LOG(INFO) << absl::StrFormat("pointer-chase elements=%lu", elem_num);
  LOG(INFO) << absl::StrFormat(
      "rd: parallel=%u workers, wss_bytes=%lu iters=%lu elems_per_worker~=%lu",
      parallel, wss_, iter_, base_chunk);
  util::debug_perf_switch();
  absl::SleepFor(absl::Seconds(5));

  auto iterate_latency = [&](uint64_t threshold) -> absl::StatusOr<double> {
    std::atomic<uint64_t> below{0};
    std::atomic<uint64_t> rounds{0};
    std::atomic<uint64_t> total_cycles{0};
    std::atomic<uint32_t> worker_failures{0};
    std::vector<std::thread> workers;
    workers.reserve(parallel);

    for (uint32_t tid = 0; tid < parallel; ++tid) {
      workers.emplace_back([&, tid]() {
        if (numa_affinity_) {
          auto st = PinCore();
          if (!st.ok()) {
            worker_failures.fetch_add(1, std::memory_order_relaxed);
            return;
          }
        }
        auto iteration = iter_;
        const uint64_t begin = slice_begins[tid];
        const uint64_t end = slice_ends[tid];
        if (begin >= end) {
          return;
        }
        auto head = &working_array[order.at(begin)];
        while (iteration--) {
          auto cur = head;
          do {
            uint64_t prev = ReadTsc();
            cur = cur->next_;
            dummy_sum += cur->pad_[0];
            DoNotOptimizeU64(dummy_sum);
            uint64_t curr = ReadTsc();
            uint64_t delta = curr - prev;
            total_cycles.fetch_add(delta, std::memory_order_relaxed);
            if (delta < threshold) {
              below.fetch_add(1, std::memory_order_relaxed);
            }
            rounds.fetch_add(1, std::memory_order_relaxed);
          } while (cur != head);
        }
      });
    }
    for (auto &t : workers) {
      t.join();
    }
    if (worker_failures.load(std::memory_order_relaxed) != 0) {
      return absl::InternalError("one or more workers failed to pin core");
    }
    const uint64_t r = rounds.load(std::memory_order_relaxed);
    if (r == 0) {
      return absl::InternalError("no rounds");
    }
    double pct =
        static_cast<double>(below.load(std::memory_order_relaxed)) * 100.0 / r;
    double avg =
        static_cast<double>(total_cycles.load(std::memory_order_relaxed)) / r;
    LOG(INFO) << absl::StrFormat(
        "threshold %lu cycles: %.3f%% <= threshold, avg %.2f cycles", threshold,
        pct, avg);
    return pct;
  };

  for (uint64_t lat = lat_low; lat <= lat_high; lat += lat_step) {
    ASSIGN_OR_RETURN(auto tmp, iterate_latency(lat));
    (void)tmp;
  }

  return absl::OkStatus();
}

absl::Status MemoryLatTest::RunRingBandwidth(uint32_t parallel,
                                             uint64_t payload_sz,
                                             bool sequential, double seconds) {
  if (parallel == 0) {
    return absl::InvalidArgumentError("--parallel must be >= 1");
  }
  if (seconds < 0) {
    return absl::InvalidArgumentError("--seconds must be >= 0");
  }
  if (numa_affinity_) {
    RETURN_IF_ERROR(PinCore());
  }

  const uint64_t payload_aligned =
      payload_sz == 0 ? 0 : AlignUp(payload_sz, kCacheLineSize);
  const uint64_t bytes_per_elem = kCacheLineSize + payload_aligned;
  if (bytes_per_elem == 0) {
    return absl::InvalidArgumentError("invalid element size");
  }
  const uint64_t elem_num = wss_ / bytes_per_elem;
  if (elem_num == 0) {
    return absl::InvalidArgumentError(
        "sz too small for one ring element (need >= 64 + payload)");
  }
  const uint64_t ring_bytes = elem_num * bytes_per_elem;
  if (ring_bytes == 0) {
    return absl::InvalidArgumentError("ring_bytes overflow");
  }
  const bool time_bounded = seconds > 0;
  const uint64_t iteration = time_bounded ? 0 : (total_sz_ / ring_bytes);
  if (!time_bounded && iteration == 0) {
    return absl::InvalidArgumentError(
        "total_sz must be >= sz (ring footprint)");
  }

  const uint64_t nodes_bytes = elem_num * kCacheLineSize;
  const uint64_t payload_total_bytes = elem_num * payload_aligned;

  int dax_fd = -1;
  void *map_base = nullptr;
  size_t map_len = 0;
  ASSIGN_OR_RETURN(void *buffer,
                   AllocateBuffer(wss_, &dax_fd, &map_base, &map_len));
  util::ResourceRelease release_fun(
      [&]() { ReleaseBuffer(buffer, wss_, dax_fd, map_base, map_len); });

  memset(buffer, 0, wss_);
  LOG(INFO) << absl::StrFormat(
      "ring_bw: threads=%u elem_num=%lu payload=%lu (aligned=%lu) "
      "footprint=%lu bytes (unused=%lu) iterations=%lu seconds=%.3f",
      parallel, elem_num, payload_sz, payload_aligned, ring_bytes,
      wss_ - ring_bytes, iteration, seconds);

  std::vector<uint64_t> order;
  uint64_t stride = shuffle_stride_;
  if (stride > elem_num) {
    stride = elem_num;
  }
  RETURN_IF_ERROR(BuildOrder(elem_num, stride, sequential, &order));

  auto *nodes = reinterpret_cast<RingNode *>(buffer);
  char *payload_base = reinterpret_cast<char *>(buffer) + nodes_bytes;
  for (uint64_t i = 0; i < elem_num; ++i) {
    nodes[i].payload_ =
        payload_aligned ? (payload_base + i * payload_aligned) : nullptr;
  }
  BuildRing(nodes, order);

  if (payload_aligned) {
    for (uint64_t i = 0; i < elem_num; ++i) {
      char *p = static_cast<char *>(nodes[i].payload_);
      for (uint64_t off = 0; off < payload_aligned; off += kCacheLineSize) {
        *reinterpret_cast<uint64_t *>(p + off) = (i ^ off);
      }
    }
  }

  util::debug_perf_switch();
  absl::SleepFor(absl::Seconds(2));

  std::atomic<int64_t> start_ns{0};
  std::atomic<int64_t> end_ns{0};
  std::atomic<bool> stop{false};

  std::barrier start_barrier(parallel + 1, [&]() {
    start_ns.store(absl::ToUnixNanos(absl::Now()), std::memory_order_relaxed);
  });
  std::barrier end_barrier(parallel + 1, [&]() {
    end_ns.store(absl::ToUnixNanos(absl::Now()), std::memory_order_relaxed);
  });

  std::atomic<uint32_t> worker_failures{0};
  std::vector<uint64_t> nodes_visited(parallel, 0);
  std::vector<std::thread> workers;
  workers.reserve(parallel);

  for (uint32_t tid = 0; tid < parallel; ++tid) {
    workers.emplace_back([&, tid]() {
      if (numa_affinity_) {
        auto st = PinCore();
        if (!st.ok()) {
          worker_failures.fetch_add(1, std::memory_order_relaxed);
          start_barrier.arrive_and_drop();
          end_barrier.arrive_and_drop();
          return;
        }
      }

      uint64_t start_idx = 0;
      if (parallel > 1) {
        uint64_t step = elem_num / parallel;
        if (step == 0) {
          step = 1;
        }
        start_idx = (static_cast<uint64_t>(tid) * step) % elem_num;
      }
      RingNode *head = &nodes[order.at(start_idx)];
      RingNode *cur = head;

      uint64_t local_sum = 0;
      uint64_t local_nodes = 0;
      start_barrier.arrive_and_wait();
      if (time_bounded) {
        while (!stop.load(std::memory_order_relaxed)) {
          cur = cur->next_;
          local_nodes++;
          local_sum += reinterpret_cast<uint64_t>(cur);
          if (payload_aligned) {
            char *p = static_cast<char *>(cur->payload_);
            for (uint64_t off = 0; off < payload_aligned;
                 off += kCacheLineSize) {
              local_sum += *reinterpret_cast<uint64_t *>(p + off);
            }
          }
          if (cur == head) {
            // keep chasing
          }
        }
      } else {
        for (uint64_t it = 0; it < iteration; ++it) {
          do {
            cur = cur->next_;
            local_nodes++;
            local_sum += reinterpret_cast<uint64_t>(cur);
            if (payload_aligned) {
              char *p = static_cast<char *>(cur->payload_);
              for (uint64_t off = 0; off < payload_aligned;
                   off += kCacheLineSize) {
                local_sum += *reinterpret_cast<uint64_t *>(p + off);
              }
            }
          } while (cur != head);
        }
      }
      __atomic_fetch_add(&dummy_sum, local_sum, __ATOMIC_RELAXED);
      DoNotOptimizeU64(local_sum);
      nodes_visited[tid] = local_nodes;
      end_barrier.arrive_and_wait();
    });
  }

  start_barrier.arrive_and_wait();
  if (time_bounded) {
    absl::SleepFor(absl::Seconds(seconds));
    stop.store(true, std::memory_order_relaxed);
  }
  end_barrier.arrive_and_wait();

  for (auto &t : workers) {
    t.join();
  }
  if (worker_failures.load(std::memory_order_relaxed) != 0) {
    return absl::InternalError("one or more workers failed to pin core");
  }

  const int64_t s = start_ns.load(std::memory_order_relaxed);
  const int64_t e = end_ns.load(std::memory_order_relaxed);
  if (e <= s) {
    return absl::InternalError("invalid timing window");
  }
  const double seconds = static_cast<double>(e - s) / 1e9;

  uint64_t total_nodes = 0;
  for (uint32_t tid = 0; tid < parallel; ++tid) {
    total_nodes += nodes_visited[tid];
  }
  const uint64_t total_node_bytes = total_nodes * kCacheLineSize;
  const uint64_t total_payload_bytes = total_nodes * payload_aligned;
  const uint64_t total_bytes = total_node_bytes + total_payload_bytes;

  const double bw_gib_s =
      (static_cast<double>(total_bytes) / seconds) / (1024.0 * 1024.0 * 1024.0);
  LOG(INFO) << absl::StrFormat(
      "ring_bw: seconds=%.6f total_bytes=%lu (node=%lu payload=%lu) "
      "nodes=%lu bandwidth=%.3f GiB/s",
      seconds, total_bytes, total_node_bytes, total_payload_bytes, total_nodes,
      bw_gib_s);
  return absl::OkStatus();
}

absl::Status MemoryLatTest::RunStoreFlush(bool sequential,
                                          FlushMethod flush_method,
                                          uint32_t lat_low, uint32_t lat_high,
                                          uint32_t lat_step, bool dependent) {
  if (numa_affinity_) {
    auto st = PinCore();
    if (!st.ok()) {
      return st;
    }
  }
  RETURN_IF_ERROR(ValidateSweep(lat_low, lat_high, lat_step));
  if (iter_ == 0) {
    return absl::InvalidArgumentError("total_sz must be >= sz");
  }

  uint64_t elem_num = wss_ / sizeof(struct TraverseElem);
  if (wss_ % sizeof(struct TraverseElem)) {
    return absl::InvalidArgumentError("odd wss");
  }

  uint64_t stride = shuffle_stride_;
  if (stride > elem_num) {
    stride = elem_num;
  }

  int dax_fd = -1;
  void *map_base = nullptr;
  size_t map_len = 0;
  ASSIGN_OR_RETURN(void *buffer,
                   AllocateBuffer(wss_, &dax_fd, &map_base, &map_len));
  util::ResourceRelease release_fun(
      [&]() { ReleaseBuffer(buffer, wss_, dax_fd, map_base, map_len); });

  memset(buffer, 0, wss_);
  LOG(INFO) << "memory set up";

  std::vector<uint64_t> order;
  RETURN_IF_ERROR(BuildOrder(elem_num, stride, sequential, &order));

  auto working_array = reinterpret_cast<struct TraverseElem *>(buffer);
  if (dependent) {
    BuildRing(working_array, order);
  }
  util::debug_perf_switch();
  absl::SleepFor(absl::Seconds(5));

  auto iterate_latency = [&](uint64_t threshold) -> absl::StatusOr<double> {
    uint64_t below = 0;
    uint64_t rounds = 0;
    uint64_t total_cycles = 0;
    auto iteration = iter_;
    while (iteration--) {
      if (dependent) {
        auto head = &working_array[order.at(0)];
        auto cur = head;
        while (true) {
          DoNotOptimizeU64(dummy_sum);
          uint64_t prev = ReadTsc();
          auto next = cur->next_;
          dummy_sum += cur->pad_[0];
          uint64_t val = reinterpret_cast<uint64_t>(next) ^ rounds;
          cur->pad_[1] = val;
          FlushLine(&cur->pad_[1], flush_method);
          _mm_mfence();
          uint64_t curr = ReadTsc();
          uint64_t delta = curr - prev;
          total_cycles += delta;
          if (delta < threshold) {
            below++;
          }
          rounds++;
          cur = next;
          if (cur == head) {
            break;
          }
        }
      } else {
        for (auto &i : order) {
          dummy_sum += working_array[i].pad_[0];
          DoNotOptimizeU64(dummy_sum);
          uint64_t val =
              (reinterpret_cast<uint64_t>(&working_array[i]) ^ rounds);
          uint64_t prev = ReadTsc();
          working_array[i].pad_[1] = val;
          FlushLine(&working_array[i].pad_[1], flush_method);
          _mm_sfence();
          uint64_t curr = ReadTsc();
          uint64_t delta = curr - prev;
          total_cycles += delta;
          if (delta < threshold) {
            below++;
          }
          rounds++;
        }
      }
    }
    if (rounds == 0) {
      return absl::InternalError("no rounds");
    }
    double pct = static_cast<double>(below) * 100.0 / rounds;
    double avg = static_cast<double>(total_cycles) / rounds;
    LOG(INFO) << absl::StrFormat(
        "threshold %lu cycles: %.3f%% <= threshold, avg %.2f cycles", threshold,
        pct, avg);
    return pct;
  };

  for (uint64_t lat = lat_low; lat <= lat_high; lat += lat_step) {
    ASSIGN_OR_RETURN(auto tmp, iterate_latency(lat));
    (void)tmp;
  }

  return absl::OkStatus();
}

absl::Status MemoryLatTest::RunFlushClean(bool sequential,
                                          FlushMethod flush_method,
                                          uint32_t lat_low, uint32_t lat_high,
                                          uint32_t lat_step, bool dependent) {
  if (numa_affinity_) {
    auto st = PinCore();
    if (!st.ok()) {
      return st;
    }
  }
  RETURN_IF_ERROR(ValidateSweep(lat_low, lat_high, lat_step));
  if (iter_ == 0) {
    return absl::InvalidArgumentError("total_sz must be >= sz");
  }

  uint64_t elem_num = wss_ / sizeof(struct TraverseElem);
  if (wss_ % sizeof(struct TraverseElem)) {
    return absl::InvalidArgumentError("odd wss");
  }

  uint64_t stride = shuffle_stride_;
  if (stride > elem_num) {
    stride = elem_num;
  }

  int dax_fd = -1;
  void *map_base = nullptr;
  size_t map_len = 0;
  ASSIGN_OR_RETURN(void *buffer,
                   AllocateBuffer(wss_, &dax_fd, &map_base, &map_len));
  util::ResourceRelease release_fun(
      [&]() { ReleaseBuffer(buffer, wss_, dax_fd, map_base, map_len); });

  memset(buffer, 0, wss_);
  LOG(INFO) << "memory set up";

  std::vector<uint64_t> order;
  RETURN_IF_ERROR(BuildOrder(elem_num, stride, sequential, &order));

  auto working_array = reinterpret_cast<struct TraverseElem *>(buffer);
  if (dependent) {
    BuildRing(working_array, order);
  }
  util::debug_perf_switch();
  absl::SleepFor(absl::Seconds(5));

  auto warm_cache = [&]() {
    uint64_t tmp = 0;
    for (auto &i : order) {
      tmp += working_array[i].pad_[0];
    }
    dummy_sum += tmp;
    _mm_mfence();
  };

  auto iterate_latency = [&](uint64_t threshold) -> absl::StatusOr<double> {
    uint64_t below = 0;
    uint64_t rounds = 0;
    uint64_t total_cycles = 0;
    auto iteration = iter_;
    while (iteration--) {
      warm_cache();
      if (dependent) {
        auto head = &working_array[order.at(0)];
        auto cur = head;
        while (true) {
          auto next = cur->next_;
          uint64_t prev = ReadTsc();
          FlushLine(cur, flush_method);
          _mm_sfence();
          uint64_t curr = ReadTsc();
          uint64_t delta = curr - prev;
          total_cycles += delta;
          if (delta < threshold) {
            below++;
          }
          rounds++;
          cur = next;
          if (cur == head) {
            break;
          }
        }
      } else {
        for (auto &i : order) {
          uint64_t prev = ReadTsc();
          FlushLine(&working_array[i], flush_method);
          _mm_sfence();
          uint64_t curr = ReadTsc();
          uint64_t delta = curr - prev;
          total_cycles += delta;
          if (delta < threshold) {
            below++;
          }
          rounds++;
        }
      }
    }
    if (rounds == 0) {
      return absl::InternalError("no rounds");
    }
    double pct = static_cast<double>(below) * 100.0 / rounds;
    double avg = static_cast<double>(total_cycles) / rounds;
    LOG(INFO) << absl::StrFormat(
        "threshold %lu cycles: %.3f%% <= threshold, avg %.2f cycles", threshold,
        pct, avg);
    return pct;
  };

  for (uint64_t lat = lat_low; lat <= lat_high; lat += lat_step) {
    ASSIGN_OR_RETURN(auto tmp, iterate_latency(lat));
    (void)tmp;
  }

  return absl::OkStatus();
}

absl::StatusOr<void *> MemoryLatTest::AllocateBuffer(uint64_t len, int *dax_fd,
                                                     void **map_base,
                                                     size_t *map_len) {
  void *buffer = nullptr;
  *dax_fd = -1;
  *map_base = nullptr;
  *map_len = 0;

  if (len == 0) {
    return absl::InvalidArgumentError("working set size must be > 0");
  }

  if (!dax_dev_.empty()) {
    if (dax_offset_ % kPageSize != 0) {
      return absl::InvalidArgumentError("dax_offset must be page-aligned");
    }
    *dax_fd = open(dax_dev_.c_str(), O_RDWR);
    if (*dax_fd < 0) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "open dax device %s failed: %s", dax_dev_.c_str(), strerror(errno)));
    }
    *map_len = (len + kPageSize - 1) & ~(kPageSize - 1);
    *map_base = mmap(NULL, *map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                     *dax_fd, (off_t)dax_offset_);
    if (*map_base == MAP_FAILED) {
      close(*dax_fd);
      return absl::InvalidArgumentError(
          absl::StrFormat("mmap dax buffer failed: %s", strerror(errno)));
    }
    buffer = *map_base;
  } else if (numa_affinity_) {
    buffer = numa_alloc_onnode(len, memory_node_);
  } else if (posix_memalign(&buffer, kPageSize, len)) {
    return absl::InvalidArgumentError("allocate buffer");
  }

  if (buffer == nullptr) {
    return absl::InvalidArgumentError("allocate buffer");
  }
  if (reinterpret_cast<uint64_t>(buffer) & (kPageSize - 1)) {
    return absl::InvalidArgumentError("allocate unaligned address");
  }
  return buffer;
}

void MemoryLatTest::ReleaseBuffer(void *buffer, uint64_t len, int dax_fd,
                                  void *map_base, size_t map_len) {
  if (!buffer) {
    return;
  }
  if (map_base) {
    munmap(map_base, map_len);
    close(dax_fd);
  } else if (numa_affinity_) {
    numa_free(buffer, len);
  } else {
    free(buffer);
  }
}

absl::Status MemoryLatTest::ValidateSweep(uint32_t lat_low, uint32_t lat_high,
                                          uint32_t lat_step) {
  if (lat_step == 0) {
    return absl::InvalidArgumentError("lat_step must be > 0");
  }
  if (lat_high < lat_low) {
    return absl::InvalidArgumentError("lat_high must be >= lat_low");
  }
  return absl::OkStatus();
}

absl::Status MemoryLatTest::PinCore() {
  if (numa_run_on_node(cpu_node_)) {
    return absl::InvalidArgumentError("bind running cpu core");
  };
  return absl::OkStatus();
}

absl::Status main(int argc, char *argv[]) {
  absl::SetProgramUsageMessage(
      "tool_memory_latency benchmark\n"
      "Example: ./tool_memory_latency --test=rd --sz=1G");
  uint64_t wss, total_size, shuffle_stride;
  util::Init util_log_config;
  ABSL_CHECK_OK(util_log_config.InitializeCode(argc, argv, false));

  auto st = util::StringUtilities::ToSize(absl::GetFlag(FLAGS_sz));
  ABSL_CHECK_OK(st);
  wss = st.value();
  st = util::StringUtilities::ToSize(absl::GetFlag(FLAGS_total_sz));
  ABSL_CHECK_OK(st);
  total_size = st.value();
  st = util::StringUtilities::ToSize(absl::GetFlag(FLAGS_stride));
  ABSL_CHECK_OK(st);
  shuffle_stride = st.value() > wss ? wss : st.value();
  LOG(INFO) << absl::StrFormat("Working set size %s, total siz %s",
                               absl::GetFlag(FLAGS_sz),
                               absl::GetFlag(FLAGS_total_sz));
  auto dax_offset_st =
      util::StringUtilities::ToSize(absl::GetFlag(FLAGS_dax_offset));
  ABSL_CHECK_OK(dax_offset_st);
  tools::MemoryLatTest instance(
      absl::GetFlag(FLAGS_numa_affinity), absl::GetFlag(FLAGS_m),
      absl::GetFlag(FLAGS_n), wss, total_size, shuffle_stride,
      absl::GetFlag(FLAGS_dax_dev), dax_offset_st.value());
  const std::string test = absl::GetFlag(FLAGS_test);
  const std::string flush_str =
      absl::AsciiStrToLower(absl::GetFlag(FLAGS_flush));
  ASSIGN_OR_RETURN(auto flush_method, ParseFlushMethod(flush_str));

  if (test == "rd") {
    RETURN_IF_ERROR(instance.RunPointerChase(
        absl::GetFlag(FLAGS_parallel), absl::GetFlag(FLAGS_lat_low),
        absl::GetFlag(FLAGS_lat_high),
        absl::GetFlag(FLAGS_lat_step), absl::GetFlag(FLAGS_sequential), false));
  } else if (test == "store_flush") {
    RETURN_IF_ERROR(instance.RunStoreFlush(
        absl::GetFlag(FLAGS_sequential), flush_method,
        absl::GetFlag(FLAGS_lat_low), absl::GetFlag(FLAGS_lat_high),
        absl::GetFlag(FLAGS_lat_step), absl::GetFlag(FLAGS_dependent)));
  } else if (test == "flush_clean") {
    RETURN_IF_ERROR(instance.RunFlushClean(
        absl::GetFlag(FLAGS_sequential), flush_method,
        absl::GetFlag(FLAGS_lat_low), absl::GetFlag(FLAGS_lat_high),
        absl::GetFlag(FLAGS_lat_step), absl::GetFlag(FLAGS_dependent)));
  } else if (test == "ring_bw") {
    auto payload_st =
        util::StringUtilities::ToSize(absl::GetFlag(FLAGS_payload_sz));
    ABSL_CHECK_OK(payload_st);
    RETURN_IF_ERROR(instance.RunRingBandwidth(
        absl::GetFlag(FLAGS_parallel), payload_st.value(),
        absl::GetFlag(FLAGS_sequential), absl::GetFlag(FLAGS_seconds)));
  } else {
    return absl::InvalidArgumentError("unsupported test");
  }
  return absl::OkStatus();
}

}; // namespace tools

int main(int argc, char *argv[]) {
  ABSL_CHECK_OK(tools::main(argc, argv));
  return 0;
}
