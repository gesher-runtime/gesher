// Only the isolated CI fixture compiles these dependencies. No wire/env switch.
#pragma once
#ifdef GESHER_RECORD_WAIT_TEST
#include <windows.h>
#include <cstdint>
namespace gesher::record_locks {
struct WaitTestHooks {
  bool cleanup = false;
  bool fail_begin = false, fail_commit = false;
  unsigned begin_failures = 0, commit_failures = 0, event_failures = 0;
  uint64_t token_low = 0, token_high = 0;
  DWORD event_error = 0;
};
extern thread_local WaitTestHooks* wait_test_hooks;
}
#endif
