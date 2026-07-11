#ifndef _UTIL_AUTO_RELEASE_H_
#define _UTIL_AUTO_RELEASE_H_
#include <functional>

namespace util {
class ResourceRelease {
 public:
  ResourceRelease(std::function<void(void)> release_func)
      : release_func_(release_func) {}
  ~ResourceRelease() { release_func_(); }

 private:
  std::function<void(void)> release_func_;
};
};  // namespace util
#endif