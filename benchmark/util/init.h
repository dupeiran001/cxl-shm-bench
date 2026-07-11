#pragma once
#include "absl/log/absl_check.h"
#include "absl/status/status.h"

namespace util {
class Init {
 public:
 absl::Status InitializeCode(int argc, char** argv, bool remove_flags);
  ~Init();
};
}  // namespace util
