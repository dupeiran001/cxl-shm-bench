#include <numa.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/absl_check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "lmbench/lmbench_mem.h"
#include "util/init.h"
#include "util/string_extension.h"

ABSL_FLAG(std::string, test, "",
          "the tests we need to run, available options are: rd wr ntwr rdwr "
          "cp fwr frd fcp bzero bcopy");
ABSL_FLAG(std::string, size, "", "the memory size for each worker");
ABSL_FLAG(int, parallel, 0, "how many workers to run");
ABSL_FLAG(int, warmup, 0, "whether to run warmup");
ABSL_FLAG(bool, aligned, false,
          "this is an option in lmbench, don't know what it is though");
ABSL_FLAG(int, mem, 0,
          "the numa memory node to run on, if memory node is -1, then we turn "
          "off numa node");
ABSL_FLAG(int, cpu, 0, "the numa cpu node to run on");
ABSL_FLAG(int, repetition, 0, "how many repitition");
ABSL_FLAG(std::string, dax_dev, "",
          "path to a dax device to mmap and use as main memory");
ABSL_FLAG(std::string, dax_offset, "0",
          "byte offset into dax device (supports K/M/G/T suffix)");
namespace lmbench {

namespace {
constexpr uint32_t kMinMemOpSize = 512;
constexpr double kMb = 1000000.0;
constexpr double kMicroSec = 1000000.0;
}  // namespace

absl::Status Run(absl::string_view arg_test) {
  auto wss = util::StringUtilities::ToSize(absl::GetFlag(FLAGS_size));
  enum lmbench_mem_t test;
  if (!wss.ok()) {
    return absl::InvalidArgumentError("per thread working set size");
  }

  if (wss.value() < kMinMemOpSize) {
    return absl::InvalidArgumentError("mem size too small");
  }

  const int parallel = absl::GetFlag(FLAGS_parallel);
  if (parallel <= 0) {
    return absl::InvalidArgumentError("parallel size");
  }

  if (arg_test == "rd") {
    test = READ;
  } else if (arg_test == "wr") {
    test = WRITE;
  } else if (arg_test == "rdwr") {
    test = RDWR;
  } else if (arg_test == "cp") {
    test = CP;
  } else if (arg_test == "fwr") {
    test = FWR;
  } else if (arg_test == "frd") {
    test = FRD;
  } else if (arg_test == "fcp") {
    test = FCP;
  } else if (arg_test == "bzero") {
    test = BZERO;
  } else if (arg_test == "bcopy") {
    test = BCOPY;
  } else if (arg_test == "ntwr") {
    test = NTWRITE;
  } else {
    return absl::InvalidArgumentError("unknown test");
  }
  const int cpu = absl::GetFlag(FLAGS_cpu);
  const int warmup = absl::GetFlag(FLAGS_warmup);
  const int repetition = absl::GetFlag(FLAGS_repetition);
  const int mem = absl::GetFlag(FLAGS_mem);
  const bool aligned = absl::GetFlag(FLAGS_aligned);
  const std::string dax_dev = absl::GetFlag(FLAGS_dax_dev);
  uint64_t dax_offset = 0;
  if (!dax_dev.empty()) {
    auto offset = util::StringUtilities::ToSize(absl::GetFlag(FLAGS_dax_offset));
    if (!offset.ok()) {
      return offset.status();
    }
    dax_offset = offset.value();
  }
  numa_run_on_node(cpu);
  bw_mem(parallel, warmup, repetition, wss.value(), aligned ? 1 : 0, test, mem,
         dax_dev.empty() ? nullptr : dax_dev.c_str(), dax_offset);
  return absl::OkStatus();
}

}  // namespace lmbench

void show_bandwidth(uint64_t time, uint64_t bytes, uint64_t iter,
                    double overhd) {
  double secs = (((double)time / (double)iter) - overhd) / lmbench::kMicroSec;
  LOG(INFO) << "per thread working set size " << bytes / lmbench::kMb
            << " MB, speed is " << bytes / lmbench::kMb / (secs) << " MB/s";
}

int main(int argc, char *argv[]) {
  absl::SetProgramUsageMessage(
      "bw_mem bandwidth benchmark\n"
      "Example: ./bw_mem --test=rd --size=1G --parallel=4");
  util::Init absl_log_congfig;
  ABSL_CHECK_OK(absl_log_congfig.InitializeCode(argc, argv, false));
  ABSL_CHECK_OK(lmbench::Run(absl::GetFlag(FLAGS_test)));
  return 0;
}
