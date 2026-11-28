#pragma once
#include <cstdint>
namespace gesher::perf {
inline int64_t Now() { return 0; }
struct Site {
  explicit Site(const char*) {}
  void Add(int64_t) {}
};
}  // namespace gesher::perf
