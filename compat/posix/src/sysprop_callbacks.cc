// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// libutils/misc.cpp's Windows build drops these callbacks. Supply the same
// priority ordering and unlocked snapshot dispatch as the Android build.
#include <algorithm>
#include <mutex>
#include <vector>
// The fork set (gesher/fork.h); absent where a host test compiles this file alone.
#if __has_include(<gesher/fork.h>)
#include <gesher/fork.h>
#endif

namespace {
struct Callback { void (*function)(); int priority; };
std::mutex& CallbackMutex() { static auto* value = new std::mutex; return *value; }
std::vector<Callback>& Callbacks() { static auto* value = new std::vector<Callback>; return *value; }
}
extern "C" void do_report_sysprop_change() {
  std::vector<Callback> snapshot;
  {
    std::lock_guard lock(CallbackMutex());
    snapshot = Callbacks();
  }
  for (const auto& callback : snapshot) callback.function();
}
namespace android {
void add_sysprop_change_callback(void (*callback)(), int priority) {
  if (!callback) return;
  std::lock_guard lock(CallbackMutex());
  auto& callbacks = Callbacks();
  auto at = std::find_if(callbacks.begin(), callbacks.end(), [&](const Callback& old) { return priority >= old.priority; });
  callbacks.insert(at, {callback, priority});
}
void report_sysprop_change() { do_report_sysprop_change(); }
}

#ifdef GESHER_FORK_LOCK
// fork (gesher/fork.h).
GESHER_FORK_LOCK(gesher::kForkOrderSubsystem, std::mutex, CallbackMutex());
#endif
