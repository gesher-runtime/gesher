// Executes the production buffering/state-transition core against real Linux
// files and pipes. Windows/ELF public-entry tests live in stdio_ofd_contract.inc.
#include "../../compat/posix/src/stdio_stream_buffer.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using gesher::stdio::StreamBuffer;
using Access = StreamBuffer::Access;
using Mode = StreamBuffer::Mode;
static int checks;
static int failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
  std::fprintf(stderr, "FAIL %d: %s (errno=%d)\n", __LINE__, #condition, errno); } } while (0)

struct Io {
  int fd;
  auto Reader() const { return [this](char* p, unsigned n) { return static_cast<int>(::read(fd, p, n)); }; }
  auto Writer() const { return [this](const char* p, unsigned n) { return static_cast<int>(::write(fd, p, n)); }; }
  auto Seeker() const { return [this](int64_t offset, int whence) { return static_cast<int64_t>(::lseek(fd, offset, whence)); }; }
};
struct File : Io {
  File() : Io{-1} {
    char path[] = "/tmp/gesher-stdio-core-XXXXXX";
    fd = mkstemp(path);
    if (fd < 0) std::abort();
    unlink(path);
  }
  ~File() { close(fd); }
  std::string Bytes() const {
    char data[128]{};
    ssize_t count = pread(fd, data, sizeof(data), 0);
    return count < 0 ? std::string() : std::string(data, static_cast<size_t>(count));
  }
};

static void AppendAndPosition() {
  File file;
  CHECK(write(file.fd, "base", 4) == 4);
  CHECK(lseek(file.fd, 0, SEEK_SET) == 0);
  int alias = dup(file.fd);
  CHECK(alias >= 0);
  StreamBuffer stream;
  char storage[8];
  CHECK(stream.Configure(Mode::kFull, storage, sizeof(storage), file.Writer()) == 0);
  CHECK(stream.Write("tail", 1, 4, file.Writer()) == 4);
  CHECK(stream.PendingOutput() == 4 && file.Bytes() == "base");
  CHECK(fcntl(alias, F_SETFL, O_APPEND) == 0);
  CHECK(stream.Flush(file.Writer()) == 0);
  CHECK(file.Bytes() == "basetail");
  CHECK(stream.Tell(file.Writer(), file.Seeker()) == 8);
  CHECK(fcntl(alias, F_SETFL, 0) == 0);
  CHECK(stream.Seek(1, SEEK_SET, file.Writer(), file.Seeker()) == 0);
  CHECK(stream.Write("XY", 1, 2, file.Writer()) == 2);
  CHECK(stream.Tell(file.Writer(), file.Seeker()) == 3);
  CHECK(file.Bytes() == "bXYetail");
  CHECK(stream.Seek(0, SEEK_SET, file.Writer(), file.Seeker()) == 0);
  char part[4]{};
  CHECK(stream.Read(part, 1, 2, file.Reader(), file.Writer()) == 2);
  CHECK(std::memcmp(part, "bX", 2) == 0);
  CHECK(lseek(file.fd, 0, SEEK_CUR) == 8);  // real read-ahead
  CHECK(stream.Tell(file.Writer(), file.Seeker()) == 2);
  CHECK(stream.Unget('!', file.Writer()) == '!');
  CHECK(stream.Tell(file.Writer(), file.Seeker()) == 1);
  CHECK(stream.Get(file.Reader(), file.Writer()) == '!');
  CHECK(stream.Get(file.Reader(), file.Writer()) == 'Y');
  CHECK(stream.Seek(1, SEEK_CUR, file.Writer(), file.Seeker()) == 0);
  CHECK(lseek(file.fd, 0, SEEK_CUR) == 4);
  CHECK(stream.Write("Z", 1, 1, file.Writer()) == 1);
  CHECK(stream.Flush(file.Writer()) == 0);
  CHECK(file.Bytes() == "bXYeZail");
  CHECK(stream.Seek(INT64_C(5) * 1024 * 1024 * 1024, SEEK_SET, file.Writer(), file.Seeker()) == 0);
  CHECK(stream.Tell(file.Writer(), file.Seeker()) == INT64_C(5) * 1024 * 1024 * 1024);
  close(alias);
}

static void PermissionsFlagsAndPushback() {
  File file;
  CHECK(write(file.fd, "abc", 3) == 3 && lseek(file.fd, 0, SEEK_SET) == 0);
  StreamBuffer input(Access::kRead);
  CHECK(input.Orient(0) == 0);
  CHECK(input.Read(nullptr, 0, SIZE_MAX, file.Reader(), file.Writer()) == 0);
  CHECK(input.Orient(0) == 0);
  CHECK(input.Flush(file.Writer()) == EOF && errno == EBADF && !input.Error());
  CHECK(input.Write("x", 1, 1, file.Writer()) == 0 && errno == EBADF && input.Error());
  input.Clear();
  CHECK(!input.Error());
  char data[8];
  CHECK(input.Read(data, 1, 8, file.Reader(), file.Writer()) == 3);
  CHECK(input.Eof() && !input.Error());
  CHECK(input.Orient(0) == -1 && input.Orient(1) == -1);
  CHECK(input.Unget('x', file.Writer()) == 'x' && !input.Eof());
  CHECK(input.Get(file.Reader(), file.Writer()) == 'x');
  CHECK(input.Get(file.Reader(), file.Writer()) == EOF && input.Eof());
  for (int i = 0; i < 80; ++i) CHECK(input.Unget(i, file.Writer()) == i);
  for (int i = 79; i >= 0; --i) CHECK(input.Get(file.Reader(), file.Writer()) == i);
  CHECK(input.Seek(0, SEEK_SET, file.Writer(), file.Seeker()) == 0 && !input.Eof());
  CHECK(input.Unget('z', file.Writer()) == 'z');
  CHECK(input.Seek(0, SEEK_SET, file.Writer(), file.Seeker()) == 0);
  CHECK(input.Get(file.Reader(), file.Writer()) == 'a');
  input.Clear();
  CHECK(input.Read(data, SIZE_MAX, 2, file.Reader(), file.Writer()) == 0 && errno == EOVERFLOW && input.Error());
  StreamBuffer output(Access::kWrite);
  CHECK(output.Read(data, 1, 1, file.Reader(), file.Writer()) == 0 && errno == EBADF && output.Error());
  output.Clear();
  CHECK(output.Write(data, SIZE_MAX, 2, file.Writer()) == 0 && errno == EOVERFLOW && output.Error());
  CHECK(output.Write(nullptr, 0, SIZE_MAX, file.Writer()) == 0);
  CHECK(file.Bytes() == "abc");
}

static void TransitionAndRebuffer() {
  File file;
  StreamBuffer stream;
  CHECK(stream.Write("12345", 1, 5, file.Writer()) == 5);
  CHECK(stream.Seek(0, SEEK_SET, file.Writer(), file.Seeker()) == 0);
  char text[16]{};
  CHECK(stream.Read(text, 1, sizeof(text), file.Reader(), file.Writer()) == 5 && stream.Eof());
  CHECK(stream.Write("6", 1, 1, file.Writer()) == 1 && !stream.Eof());
  CHECK(stream.Flush(file.Writer()) == 0 && file.Bytes() == "123456");
  CHECK(stream.Seek(0, SEEK_SET, file.Writer(), file.Seeker()) == 0);
  CHECK(stream.Get(file.Reader(), file.Writer()) == '1');
  size_t unread = stream.Unread();
  CHECK(unread == 5);
  CHECK(stream.Flush(file.Writer()) == 0 && stream.Unread() == unread);  // AOSP update-input fflush
  CHECK(stream.Get(file.Reader(), file.Writer()) == '2');
  CHECK(stream.Configure(Mode::kNone, nullptr, 0, file.Writer()) == 0);
  CHECK(stream.Get(file.Reader(), file.Writer()) == EOF && stream.Eof()); // discarded read-ahead
  char supplied[1]{};
  CHECK(stream.Configure(Mode::kFull, supplied, 0, file.Writer()) == 0 && !stream.Eof());
  // AOSP size=0 requests a normal allocated buffer, ignoring the supplied one.
  CHECK(stream.Write("789", 1, 3, file.Writer()) == 3);
  CHECK(supplied[0] == 0);
  CHECK(stream.Configure(Mode::kFull, nullptr, static_cast<size_t>(INT_MAX) + 1, file.Writer()) == -1);
  CHECK(stream.PendingOutput() == 3);
  CHECK(stream.Flush(file.Writer()) == 0 && file.Bytes() == "123456789");
}

static void ShortWritesAndFailures() {
  File file;
  StreamBuffer stream;
  char storage[8];
  CHECK(stream.Configure(Mode::kFull, storage, sizeof(storage), file.Writer()) == 0);
  CHECK(stream.Write("abcdef", 1, 6, file.Writer()) == 6);
  int calls = 0;
  auto short_writer = [&](const char* bytes, unsigned count) {
    if (++calls == 1) return errno = EINTR, -1;
    return static_cast<int>(write(file.fd, bytes, std::min(count, 2u)));
  };
  CHECK(stream.Flush(short_writer) == 0 && calls == 4 && file.Bytes() == "abcdef");
  CHECK(stream.Configure(Mode::kLine, storage, sizeof(storage), file.Writer()) == 0);
  CHECK(stream.Write("one\ntwo", 1, 7, file.Writer()) == 7);
  CHECK(file.Bytes() == "abcdefone\n" && stream.PendingOutput() == 3);
  auto failure = [](const char*, unsigned) { return errno = EIO, -1; };
  CHECK(stream.Flush(failure) == EOF && errno == EIO && stream.Error());
  CHECK(stream.PendingOutput() == 0);
  CHECK(stream.Flush(file.Writer()) == 0 && file.Bytes() == "abcdefone\n");
  stream.Clear();
  CHECK(stream.Write("line\n", 1, 5, failure) == 0 && stream.Error());
  CHECK(stream.PendingOutput() == 0);
  stream.Clear();
  CHECK(stream.Configure(Mode::kFull, storage, sizeof(storage), file.Writer()) == 0);
  CHECK(stream.Write("lost", 1, 4, file.Writer()) == 4);
  const auto offset = lseek(file.fd, 0, SEEK_CUR);
  CHECK(stream.Tell(failure, file.Seeker()) == offset && stream.Error()); // AOSP ftello flush error
  CHECK(stream.PendingOutput() == 0);
  stream.Clear();
  CHECK(stream.Write("drop", 1, 4, file.Writer()) == 4);
  CHECK(stream.Configure(Mode::kNone, nullptr, 0, failure) == 0 && stream.Error());
  CHECK(stream.PendingOutput() == 0);
}

static void AndroidEofRetry() {
  File file;
  StreamBuffer stream(Access::kRead);
  CHECK(stream.Get(file.Reader(), file.Writer()) == EOF && stream.Eof());
  CHECK(pwrite(file.fd, "x", 1, 0) == 1);
  CHECK(stream.Get(file.Reader(), file.Writer()) == 'x' && stream.Eof());
  stream.Clear();
  CHECK(!stream.Eof());
}

static void NonblockingPipes() {
  int pipes[2];
  CHECK(pipe2(pipes, O_NONBLOCK | O_CLOEXEC) == 0);
  Io input{pipes[0]}, output{pipes[1]};
  StreamBuffer reader(Access::kRead), writer(Access::kWrite);
  char bytes[16]{};
  CHECK(reader.Read(bytes, 1, 1, input.Reader(), input.Writer()) == 0 && errno == EAGAIN);
  CHECK(reader.Error() && !reader.Eof());
  CHECK(write(pipes[1], "xy", 2) == 2);
  CHECK(reader.Read(bytes, 1, 4, input.Reader(), input.Writer()) == 2 && errno == EAGAIN);
  CHECK(std::memcmp(bytes, "xy", 2) == 0 && reader.Error() && !reader.Eof());
  reader.Clear();
  CHECK(!reader.Error() && !reader.Eof());
  CHECK(writer.Configure(Mode::kNone, nullptr, 0, output.Writer()) == 0);
  std::string fill(4096, 'q');
  while (write(pipes[1], fill.data(), fill.size()) > 0) {}
  CHECK(errno == EAGAIN);
  CHECK(writer.Write("!", 1, 1, output.Writer()) == 0 && errno == EAGAIN && writer.Error());
  while (read(pipes[0], fill.data(), fill.size()) > 0) {}
  CHECK(errno == EAGAIN);
  writer.Clear();
  char buffer[8];
  CHECK(writer.Configure(Mode::kFull, buffer, sizeof(buffer), output.Writer()) == 0);
  CHECK(writer.Write("live", 1, 4, output.Writer()) == 4);
  CHECK(writer.Flush(output.Writer()) == 0);
  CHECK(reader.Read(bytes, 1, 4, input.Reader(), input.Writer()) == 4 && std::memcmp(bytes, "live", 4) == 0);
  close(pipes[1]);
  CHECK(reader.Read(bytes, 1, 1, input.Reader(), input.Writer()) == 0 && reader.Eof() && !reader.Error());
  close(pipes[0]);
}

static void DelimitedGrowthAndErrors() {
  auto writer = [](const char*, unsigned) { return errno = EBADF, -1; };
  auto reallocate = [](void* bytes, size_t size) { return std::realloc(bytes, size); };
  // The allocator is the production callback boundary, not a replacement of
  // the buffer algorithm. Exercise overflow before any allocation is allowed.
  int attempts = 0;
  auto fail = [&](void*, size_t) -> void* { ++attempts; return errno = ENOMEM, nullptr; };
  char* bytes = nullptr;
  size_t capacity = 0;
  constexpr size_t limit = static_cast<size_t>(PTRDIFF_MAX);
  CHECK(gesher::stdio::detail::GrowLine(&bytes, &capacity, limit, 1, fail) == -1 && errno == EOVERFLOW);
  CHECK(gesher::stdio::detail::GrowLine(&bytes, &capacity, limit - 1, 1, fail) == -1 && errno == EOVERFLOW);
  CHECK(gesher::stdio::detail::GrowLine(&bytes, &capacity, 0, SIZE_MAX, fail) == -1 && errno == EOVERFLOW);
  CHECK(attempts == 0 && bytes == nullptr && capacity == 0);
  CHECK(gesher::stdio::detail::GrowLine(&bytes, &capacity, limit - 2, 1, fail) == -1 && errno == ENOMEM);
  CHECK(attempts == 1 && bytes == nullptr && capacity == 0);

  for (size_t chunk : {size_t{1}, size_t{3}, size_t{128}, size_t{4096}}) {
    StreamBuffer stream(Access::kRead);
    std::vector<char> storage(chunk);
    CHECK(stream.Configure(Mode::kFull, storage.data(), storage.size(), writer) == 0);
    const std::string data = std::string(8192, 'x') + '\n' + "suffix";
    size_t cursor = 0, calls = 0;
    auto reader = [&](char* out, unsigned bound) {
      ++calls;
      if (calls == 1) return errno = EINTR, -1;
      size_t size = std::min(static_cast<size_t>(bound), data.size() - cursor);
      std::memcpy(out, data.data() + cursor, size);
      cursor += size;
      return static_cast<int>(size);
    };
    bytes = nullptr; capacity = 0;
    CHECK(stream.ReadDelimited(&bytes, &capacity, '\n', reader, writer, reallocate) == 8193);
    CHECK(capacity == 16384 && std::memcmp(bytes, data.data(), 8193) == 0 && bytes[8193] == 0);
    CHECK(std::all_of(bytes + 8194, bytes + capacity, [](char c) { return c == 0; }));
    CHECK(cursor - stream.Unread() == 8193 && !stream.Error() && !stream.Eof());
    CHECK(stream.ReadDelimited(&bytes, &capacity, '\n', reader, writer, reallocate) == 6 &&
          std::memcmp(bytes, "suffix", 7) == 0 && stream.Eof());
    std::free(bytes);
  }

  StreamBuffer pushed(Access::kRead);
  CHECK(pushed.Unget('\n', writer) == '\n' && pushed.Unget('!', writer) == '!');
  int reads = 0;
  auto no_input = [&](char*, unsigned) { ++reads; return 0; };
  bytes = nullptr; capacity = 0;
  CHECK(pushed.ReadDelimited(&bytes, &capacity, '\n', no_input, writer, fail) == -1 &&
        pushed.Unread() == 2 && reads == 0 && pushed.Error());
  pushed.Clear();
  CHECK(pushed.ReadDelimited(&bytes, &capacity, '\n', no_input, writer, reallocate) == 2 &&
        std::memcmp(bytes, "!\n", 3) == 0 && pushed.Unread() == 0 && reads == 0);
  std::free(bytes);

  // A failing growth after a completed span retains that prefix, but the
  // next span remains unread for retry. Input isn't lost to a failed realloc.
  StreamBuffer partial(Access::kRead);
  char storage[128];
  CHECK(partial.Configure(Mode::kFull, storage, sizeof(storage), writer) == 0);
  const std::string record(300, 'p');
  size_t position = 0;
  auto reader = [&](char* out, unsigned bound) {
    size_t size = std::min(static_cast<size_t>(bound), record.size() - position);
    std::memcpy(out, record.data() + position, size); position += size; return static_cast<int>(size);
  };
  bytes = static_cast<char*>(std::malloc(129)); capacity = 129;
  std::memset(bytes, 0x55, capacity);
  char* original = bytes;
  CHECK(partial.ReadDelimited(&bytes, &capacity, '\n', reader, writer, fail) == -1 &&
        bytes == original && capacity == 129 && position - partial.Unread() == 128);
  CHECK(std::memcmp(bytes, record.data(), 128) == 0 && bytes[128] == 0x55);
  partial.Clear();
  CHECK(partial.ReadDelimited(&bytes, &capacity, '\n', reader, writer, reallocate) == 172 &&
        std::memcmp(bytes, record.data() + 128, 172) == 0 && bytes[172] == 0);
  std::free(bytes);
}

int main() {
  AppendAndPosition();
  PermissionsFlagsAndPushback();
  TransitionAndRebuffer();
  ShortWritesAndFailures();
  AndroidEofRetry();
  NonblockingPipes();
  DelimitedGrowthAndErrors();
  std::printf("stdio production core: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
