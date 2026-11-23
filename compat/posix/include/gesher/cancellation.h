// Scope cleanup for both C++ unwinding and winpthreads' explicit cancel chain.
#pragma once
#include <pthread.h>

namespace gesher {
class CancellationCleanup {
 public:
  using Routine = void (*)(void*);
  CancellationCleanup(Routine routine, void* argument) noexcept
      : routine_(routine), argument_(argument) {
#ifdef __WINPTHREADS_VERSION
    record_ = {Cancel, this, *pthread_getclean()};
    __pthread_MemoryBarrier();
    *pthread_getclean() = &record_;
    __pthread_MemoryBarrier();
#endif
  }
  ~CancellationCleanup() {
    if (!active_) return;
#ifdef __WINPTHREADS_VERSION
    *pthread_getclean() = record_.next;
#endif
    Run();
  }
  CancellationCleanup(const CancellationCleanup&) = delete;
  CancellationCleanup& operator=(const CancellationCleanup&) = delete;

 private:
  void Run() {
    active_ = false;
    routine_(argument_);
  }
#ifdef __WINPTHREADS_VERSION
  static void Cancel(void* raw) {
    auto& cleanup = *static_cast<CancellationCleanup*>(raw);
    if (*pthread_getclean() == &cleanup.record_) *pthread_getclean() = cleanup.record_.next;
    // A later C++ unwind, if provided by a runtime, must not execute twice.
    cleanup.Run();
  }
  _pthread_cleanup record_{};
#endif
  Routine routine_;
  void* argument_;
  bool active_ = true;
};
}
