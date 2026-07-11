#include "init.h"

#include <signal.h>
#include <unistd.h>

#include <vector>

#include "absl/flags/parse.h"
#include "absl/log/absl_check.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
namespace util {
namespace {
void flushLogOnSignal(int signal) {
  LOG(INFO) << absl::StrFormat("received signal %d", signal);
  if (signal == SIGSEGV) {
    LOG(FATAL) << "Segmentation fault!";
  } else if (signal == SIGFPE) {
    LOG(FATAL) << "Floating point exception!";
  } else if (signal == SIGHUP) {
    LOG(INFO) << "sighup, flushing log";
  }
  if (signal != SIGHUP) {
    exit(0);
  }
}
}  // namespace

absl::Status Init::InitializeCode(int argc, char** argv, bool remove_flags) {
  (void)remove_flags;
  absl::ParseCommandLine(argc, argv);

  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverity::kInfo);

  std::vector<int> signals = {
      SIGINT, SIGTERM, SIGSEGV, SIGFPE, SIGHUP,
  };
  for (auto sig : signals) {
    signal(sig, flushLogOnSignal);
  }
  // signal(SIGSEGV, sigsegv_handler);
  return absl::OkStatus();
};

Init::~Init() {}
}  // namespace util
