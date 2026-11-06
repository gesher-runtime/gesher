// bionic's LP64 __pthread_cleanup_t and the per-thread LIFO it links into.
#pragma once

#include "abi_types.h"

namespace gesher::bionic {

using CleanupRoutine = void (BIONIC_SYSV *)(void*);
struct CleanupRecord {
  CleanupRecord* previous;
  CleanupRoutine routine;
  void* argument;
};
static_assert(sizeof(CleanupRecord) == 24);

class CleanupStack {
 public:
  void Push(CleanupRecord* record, CleanupRoutine routine, void* argument) {
    *record = {top_, routine, argument};
    top_ = record;
  }

  void Pop(CleanupRecord* record, bool execute) {
    // Remove before calling: a cleanup may itself push/pop another handler.
    top_ = record->previous;
    if (execute && record->routine != nullptr) record->routine(record->argument);
  }

  void Run() {
    while (top_ != nullptr) Pop(top_, true);
  }

 private:
  CleanupRecord* top_ = nullptr;
};

}  // namespace gesher::bionic
