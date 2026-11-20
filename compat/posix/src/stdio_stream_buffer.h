// The descriptor-backed part of C stdio, independent of FILE's host ABI.
// The state transitions follow Android's stdio.cpp, refill.c and wsetup.c:
// a read buffer belongs to the stream, status flags belong to its descriptor's
// open file description, and a failed output flush discards that buffer.
// Keep OS calls in the supplied callbacks so this production core is exercised
// against real descriptors by the Linux-host and Windows contract tests.
#pragma once

#include "stdio_output_buffer.h"

#include <cstdint>
#include <cstdio>
#include <limits>

namespace gesher::stdio {
class StreamBuffer {
 public:
  enum class Access { kRead, kWrite, kUpdate };
  using Mode = OutputBuffer::Mode;

  explicit StreamBuffer(Access access = Access::kUpdate) : access_(access) {}
  bool Error() const { return output_.error; }
  bool Eof() const { return eof_; }
  void SetError(int error) { output_.error = true; errno = error; ++failures_; }
  void Clear() { output_.error = false; eof_ = false; }
  uint64_t Failures() const { return failures_; }
  size_t PendingOutput() const { return output_.Pending(); }
  Mode Buffering() const { return output_.Buffering(); }
  size_t Unread() const { return available_ - next_ + pushed_; }
  bool Writing() const { return direction_ == Direction::kWrite; }
  bool Reading() const { return direction_ == Direction::kRead; }
  bool CanWrite() const { return access_ != Access::kRead; }
  bool CanRead() const { return access_ != Access::kWrite; }
  int Orient(int mode) {
    if (orientation_ == 0) orientation_ = mode > 0 ? 1 : mode < 0 ? -1 : 0;
    return orientation_;
  }

  template<class Writer>
  int Configure(Mode mode, char* supplied, size_t size, Writer&& writer) {
    if (mode != Mode::kNone && size > INT_MAX) return -1;
    // Android setvbuf attempts the flush but still changes buffering after a
    // write failure. An allocation failure returns EOF and falls back to the
    // normal buffer size, or to unbuffered I/O if that also fails.
    FlushOutput(writer);
    const bool error = output_.error;
    // Release the old allocation before replacing it, as setvbuf does in
    // bionic; changing a large buffer must not require two such allocations.
    output_ = OutputBuffer{};
    int result = output_.Configure(mode, supplied, size);
    if (result < 0 && output_.Configure(mode, nullptr, OutputBuffer::kDefaultCapacity) < 0) {
      output_.Configure(Mode::kNone, nullptr, 0);
    }
    output_.error = error;
    DiscardInput();
    eof_ = false;
    return result;
  }

  template<class Writer>
  int FlushOutput(Writer&& writer) {
    if (output_.Flush(writer) < 0) { ++failures_; return -1; }
    return 0;
  }

  template<class Writer>
  int Flush(Writer&& writer) {
    // Android fflush rejects a read-only stream. For an update stream that is
    // currently reading, __sflush is a no-op: it does not discard read-ahead.
    if (!CanWrite()) return errno = EBADF, EOF;
    return FlushOutput(writer) < 0 ? EOF : 0;
  }

  template<class Writer>
  size_t Write(const void* bytes, size_t size, size_t count, Writer&& writer) {
    if (size && count > SIZE_MAX / size) { SetError(EOVERFLOW); return 0; }
    const size_t total = size * count;
    if (!total) return 0;
    if (!CanWrite()) { SetError(EBADF); return 0; }
    Orient(-1);
    if (direction_ == Direction::kRead) {
      // ISO C requires an intervening positioning operation unless reading
      // reached EOF. Do not silently seek or invent a different transition.
      DiscardInput();
      eof_ = false;
    }
    direction_ = Direction::kWrite;
    const size_t written = output_.Write(bytes, total, writer);
    if (written != total) ++failures_;
    return written / size;
  }

  template<class Reader, class Writer>
  size_t Read(void* bytes, size_t size, size_t count, Reader&& reader, Writer&& writer) {
    if (size && count > SIZE_MAX / size) { SetError(EOVERFLOW); return 0; }
    const size_t total = size * count;
    if (!total) return 0;
    Orient(-1);
    if (!PrepareRead(writer)) return 0;
    char* cursor = static_cast<char*>(bytes);
    size_t remaining = total;
    while (remaining) {
      if (pushed_) {
        *cursor++ = static_cast<char>(Pushback()[--pushed_]);
        --remaining;
      } else if (next_ != available_) {
        const size_t chunk = std::min(remaining, available_ - next_);
        std::memcpy(cursor, InputData() + next_, chunk);
        next_ += chunk;
        cursor += chunk;
        remaining -= chunk;
      } else {
        // Bionic __srefill deliberately retries the descriptor even after an
        // earlier EOF (SysV compatibility). Successful input does not itself
        // clear the indicator; clearerr, seek, ungetc or an output transition do.
        if (output_.EnsureStorage() < 0) { SetError(errno); break; }
        const size_t capacity = InputCapacity();
        if (remaining > capacity) {
          // Bionic avoids an extra copy for requests larger than the buffer.
          int result = ReadChunk(reader, cursor, remaining);
          if (result <= 0) break;
          cursor += result;
          remaining -= static_cast<size_t>(result);
        } else {
          int result = ReadChunk(reader, InputData(), capacity);
          if (result <= 0) break;
          next_ = 0;
          available_ = static_cast<size_t>(result);
        }
      }
    }
    return (total - remaining) / size;
  }

  template<class Reader, class Writer>
  int Get(Reader&& reader, Writer&& writer) {
    unsigned char value;
    return Read(&value, 1, 1, reader, writer) == 1 ? value : EOF;
  }

  template<class Writer>
  int Unget(int value, Writer&& writer) {
    if (value == EOF) return EOF;
    if (!PrepareRead(writer)) return EOF;
    Orient(-1);
    if (pushed_ == push_capacity_) {
      if (push_capacity_ > SIZE_MAX / 2) return errno = ENOMEM, EOF;
      const size_t capacity = push_capacity_ * 2;
      std::unique_ptr<unsigned char[]> replacement(new (std::nothrow) unsigned char[capacity]);
      if (!replacement) return errno = ENOMEM, EOF;
      std::memcpy(replacement.get(), Pushback(), pushed_);
      push_storage_ = std::move(replacement);
      push_capacity_ = capacity;
    }
    Pushback()[pushed_++] = static_cast<unsigned char>(value);
    eof_ = false;
    return static_cast<unsigned char>(value);
  }

  template<class Writer, class Seeker>
  int64_t Tell(Writer&& writer, Seeker&& seeker) {
    // Android ftello attempts to flush, then reports the descriptor offset
    // even if that flush failed. The stream's error indicator retains failure.
    FlushOutput(writer);
    int64_t position = seeker(0, SEEK_CUR);
    if (position < 0) return -1;
    const size_t unread = Unread();
    if (unread > static_cast<uint64_t>(INT64_MAX)) return errno = EOVERFLOW, -1;
    return position - static_cast<int64_t>(unread);
  }

  template<class Writer, class Seeker>
  int Seek(int64_t offset, int whence, Writer&& writer, Seeker&& seeker) {
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return errno = EINVAL, -1;
    if (whence == SEEK_CUR) {
      int64_t current = Tell(writer, seeker);
      if (current == -1) return -1;
      if ((offset > 0 && current > INT64_MAX - offset) ||
          (offset < 0 && current < INT64_MIN - offset)) return errno = EOVERFLOW, -1;
      offset += current;
      whence = SEEK_SET;
    }
    if (FlushOutput(writer) < 0 || seeker(offset, whence) < 0) return -1;
    DiscardInput();
    eof_ = false;
    return 0;
  }

 private:
  enum class Direction { kNone, kRead, kWrite };
  template<class Writer>
  bool PrepareRead(Writer& writer) {
    if (!CanRead()) { SetError(EBADF); return false; }
    if (direction_ == Direction::kWrite && FlushOutput(writer) < 0) return false;
    direction_ = Direction::kRead;
    return true;
  }
  template<class Reader>
  int ReadChunk(Reader& reader, char* bytes, size_t count) {
    int result;
    const unsigned chunk = static_cast<unsigned>(std::min(count, size_t{0x7ffff000}));
    do { result = reader(bytes, chunk); } while (result < 0 && errno == EINTR);
    if (result < 0) SetError(errno);
    else if (result == 0) eof_ = true;
    return result;
  }
  char* InputData() { return output_.Buffering() == Mode::kNone ? &unbuffered_ : output_.Data(); }
  size_t InputCapacity() const { return output_.Buffering() == Mode::kNone ? 1 : output_.Capacity(); }
  unsigned char* Pushback() { return push_storage_ ? push_storage_.get() : push_small_; }
  void DiscardInput() { next_ = 0; available_ = 0; pushed_ = 0; }

  OutputBuffer output_;
  Access access_;
  Direction direction_ = Direction::kNone;
  bool eof_ = false;
  int orientation_ = 0;
  uint64_t failures_ = 0;
  size_t next_ = 0;
  size_t available_ = 0;
  char unbuffered_ = 0;
  unsigned char push_small_[16]{};
  std::unique_ptr<unsigned char[]> push_storage_;
  size_t pushed_ = 0;
  size_t push_capacity_ = sizeof(push_small_);
};
}  // namespace gesher::stdio
