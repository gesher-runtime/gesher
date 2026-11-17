#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace gesher {

enum class PipeDirection { kRead, kWrite };

struct PipeState {
  mutable std::mutex mutex;
  std::condition_variable readable_cv;
  std::condition_variable writable_cv;

  HANDLE read_handle = INVALID_HANDLE_VALUE;
  HANDLE write_handle = INVALID_HANDLE_VALUE;

  size_t capacity = 64 * 1024;
  int readers = 1;
  int writers = 1;

  ~PipeState() {
    if (read_handle != INVALID_HANDLE_VALUE) CloseHandle(read_handle);
    if (write_handle != INVALID_HANDLE_VALUE) CloseHandle(write_handle);
  }
};

struct PipeEndpoint {
  PipeDirection direction;
  std::atomic<bool> nonblocking{false};
  std::shared_ptr<PipeState> state;
};

struct PipeFd {
  std::shared_ptr<PipeEndpoint> endpoint;
};

void RegisterPipe(int read_fd, int write_fd, bool nonblocking);
bool LookupPipe(int fd, PipeFd* out);
void ForgetPipe(int fd);
void DupPipe(int old_fd, int new_fd);

bool ReadPipe(int fd, void* buf, unsigned int count, int* result);
bool WritePipe(int fd, const void* buf, unsigned int count, int* result);
bool PollPipe(int fd, short events, short* revents);

}  // namespace gesher
