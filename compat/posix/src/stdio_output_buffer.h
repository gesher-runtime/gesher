// Descriptor-backed stdio output. Buffering and byte counts follow bionic's
// __sfvwrite/__sflush: the descriptor's current status is consulted at flush,
// and an unsuccessful flush discards that attempted buffer and sets ferror.
#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <climits>
#include <cstring>
#include <memory>
#include <new>

namespace gesher::stdio {
class OutputBuffer {
 public:
  enum class Mode { kFull, kLine, kNone };
  static constexpr size_t kDefaultCapacity = 4096;
  bool error = false;

  int Configure(Mode mode, char* supplied, size_t size) {
    if (mode != Mode::kNone && size > INT_MAX) return -1;
    if (mode != Mode::kNone && size == 0) {
      supplied = nullptr;
      size = kDefaultCapacity;
    }
    std::unique_ptr<char[]> replacement;
    if (mode != Mode::kNone && !supplied) {
      replacement.reset(new (std::nothrow) char[size]);
      if (!replacement) return errno = ENOMEM, -1;
    }
    storage_ = std::move(replacement);
    data_ = supplied ? supplied : storage_.get();
    capacity_ = mode == Mode::kNone ? 0 : size;
    mode_ = mode;
    used_ = 0;
    return 0;
  }

  size_t Pending() const { return used_; }
  size_t Capacity() const { return capacity_; }
  Mode Buffering() const { return mode_; }
  char* Data() const { return data_; }
  int EnsureStorage() {
    if (mode_ == Mode::kNone || data_) return 0;
    return Configure(mode_, nullptr, capacity_);
  }
  void Purge() { used_ = 0; }

  template<class Writer>
  int Flush(Writer&& write) {
    size_t remaining = used_;
    used_ = 0;  // bionic resets the buffer before calling its write callback.
    const char* cursor = data_;
    while (remaining) {
      int result = Retry(write, cursor, remaining);
      if (result <= 0) { error = true; return -1; }
      cursor += result;
      remaining -= static_cast<size_t>(result);
    }
    return 0;
  }

  template<class Writer>
  size_t Write(const void* bytes, size_t count, Writer&& write) {
    const char* cursor = static_cast<const char*>(bytes);
    size_t remaining = count;
    if (!count) return 0;
    if (EnsureStorage() < 0) {
      error = true;
      return 0;
    }
    while (remaining) {
      size_t consumed = 0;
      if (mode_ == Mode::kNone) {
        int result = Retry(write, cursor, remaining);
        if (result <= 0) { error = true; break; }
        consumed = static_cast<size_t>(result);
      } else {
        size_t segment = remaining;
        bool newline = false;
        if (mode_ == Mode::kLine) {
          if (const void* end = std::memchr(cursor, '\n', remaining)) {
            segment = static_cast<const char*>(end) - cursor + 1;
            newline = true;
          }
        }
        size_t available = capacity_ - used_;
        if (used_ && segment > available) {
          std::memcpy(data_ + used_, cursor, available);
          used_ += available;
          if (Flush(write) < 0) break;
          consumed = available;
        } else if (used_ == 0 && segment >= capacity_) {
          size_t direct = mode_ == Mode::kLine ? capacity_ : (segment / capacity_) * capacity_;
          int result = Retry(write, cursor, direct);
          if (result <= 0) { error = true; break; }
          consumed = static_cast<size_t>(result);
        } else {
          consumed = segment;
          std::memcpy(data_ + used_, cursor, consumed);
          used_ += consumed;
        }
        // Do not count this chunk if the flush containing its newline failed.
        if (newline && consumed == segment && Flush(write) < 0) break;
      }
      cursor += consumed;
      remaining -= consumed;
    }
    return count - remaining;
  }

 private:
  template<class Writer>
  static int Retry(Writer& write, const char* bytes, size_t count) {
    unsigned chunk = static_cast<unsigned>(std::min(count, size_t{0x7ffff000}));
    int result;
    do { result = write(bytes, chunk); } while (result < 0 && errno == EINTR);
    return result;
  }
  Mode mode_ = Mode::kFull;
  std::unique_ptr<char[]> storage_;
  char* data_ = nullptr;
  size_t capacity_ = kDefaultCapacity;
  size_t used_ = 0;
};
}  // namespace gesher::stdio
