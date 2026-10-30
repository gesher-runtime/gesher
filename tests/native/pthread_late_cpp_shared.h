#pragma once
#include <pthread.h>
#include <atomic>
#include <cstdint>
struct LateCppCase {
  const char *mode;
  pthread_t expected{};
  std::uint64_t native{};
  std::atomic<int> constructed{0},a{0},b{0},mismatch{0},body_left{0},sequence{0};
};
void LateCppRecord(LateCppCase *,const char *);
void LateCppTouchA(LateCppCase *);
void LateCppTouchB(LateCppCase *);
