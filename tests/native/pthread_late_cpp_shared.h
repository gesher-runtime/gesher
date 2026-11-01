#pragma once
#include <pthread.h>
#include <atomic>
#include <cstdint>
struct LateCppCase {
  const char *mode;
  pthread_t expected{};
  std::uint64_t native{};
  std::atomic<std::uintptr_t> native_handle{0};
  std::atomic<int> constructed{0},a{0},b{0},mismatch{0},body_left{0},sequence{0};
  std::atomic<int> a_done{0},b_done{0};
  std::atomic<int> late_sets{0},late_key_dtors{0},early_key_dtors{0},hold_claimed{0};
  bool held=false;
  std::uintptr_t hold_entered{},hold_release{};
};
void LateCppRecord(LateCppCase *,const char *);
void LateCppTouchA(LateCppCase *);
void LateCppTouchB(LateCppCase *);
