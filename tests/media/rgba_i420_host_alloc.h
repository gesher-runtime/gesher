// The software encoder's Windows aligned allocator, adapted only for host tests.
#pragma once
#ifndef _WIN32
#include <cstdlib>
inline void* _aligned_malloc(size_t bytes, size_t alignment) {
  void* p = nullptr;
  return posix_memalign(&p, alignment, bytes) == 0 ? p : nullptr;
}
inline void _aligned_free(void* p) { std::free(p); }
#endif
