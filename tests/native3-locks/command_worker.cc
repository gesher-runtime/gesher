// The SAME portable command worker calls actual Linux fcntl and the Windows
// common POSIX implementation. No lock state or expected results live here.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cerrno>
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace {
constexpr size_t kSlots = 32, kPage = 4096;
int descriptors[kSlots];
FILE* streams[kSlots]{};
unsigned char* memory = nullptr;
int Descriptor(int slot) { return slot >= 0 && slot < static_cast<int>(kSlots) ? descriptors[slot] : -1; }
bool Slot(int slot) { return slot >= 0 && slot < static_cast<int>(kSlots); }
void Forget(int fd) {
  for (int& descriptor : descriptors) if (descriptor == fd) descriptor = -1;
}
void Result(int result, int error, bool minimum = true) {
  std::printf("{\"result\":%d,\"errno\":%d,\"minimum\":%s}\n", result < 0 ? -1 : 0, error, minimum ? "true" : "false");
  std::fflush(stdout);
}
int Failure(const char* message) {
  std::fprintf(stderr, "COMMAND_WORKER_FATAL %s errno=%d\n", message, errno);
  return 91;
}
void LockStream(FILE* stream) {
#ifdef _WIN32
  _lock_file(stream);
#else
  flockfile(stream);
#endif
}
void UnlockStream(FILE* stream) {
#ifdef _WIN32
  _unlock_file(stream);
#else
  funlockfile(stream);
#endif
}
}  // namespace

int main(int argc, char** argv) {
  if (!std::getenv("GITHUB_ACTIONS") || std::strcmp(std::getenv("GITHUB_ACTIONS"), "true") != 0) return 90;
  if (argc != 7) return 92;  // main, separate, hardlink, notdir child, empty, absent device
  static_assert(sizeof(struct flock) == 32 && offsetof(struct flock, l_start) == 8);
  for (int& descriptor : descriptors) descriptor = -1;
  memory = static_cast<unsigned char*>(mmap(reinterpret_cast<void*>(UINT64_C(0x1100000000)), kPage * 2,
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (memory == MAP_FAILED || reinterpret_cast<uintptr_t>(memory) <= UINT32_MAX) return Failure("high64bit mapping");
  std::printf("{\"ready\":true,\"pid\":%d,\"pointer_high\":true}\n", static_cast<int>(getpid()));
  std::fflush(stdout);
  char line[512], command[32];
  while (std::fgets(line, sizeof(line), stdin)) {
    if (std::sscanf(line, "%31s", command) != 1) return Failure("empty command");
    if (std::strcmp(command, "exit") == 0) break;
    int slot = -1, other = -1, flags = 0;
    if (std::strcmp(command, "open") == 0 || std::strcmp(command, "stream") == 0) {
      if (std::sscanf(line, "%*s %d %d %d", &slot, &other, &flags) != 3 || !Slot(slot) || other < 0 || other > 5 || Descriptor(slot) >= 0) return Failure("open command shape");
      errno = 123;
      int result;
      if (command[0] == 's') {
        streams[slot] = std::fopen(argv[other + 1], flags == 0 ? "r" : flags == 1 ? "w" : "r+");
        result = streams[slot] ? fileno(streams[slot]) : -1;
      } else result = open(argv[other + 1], flags);
      int error = errno;
      if (result >= 0) descriptors[slot] = result;
      Result(result, error);
    } else if (std::strcmp(command, "dup") == 0 || std::strcmp(command, "dupfd") == 0) {
      if (std::sscanf(line, "%*s %d %d %d", &slot, &other, &flags) != 3 || !Slot(other) || Descriptor(other) >= 0) return Failure("dup command shape");
      errno = 123;
      int result = std::strcmp(command, "dupfd") == 0 ? fcntl(Descriptor(slot), F_DUPFD, flags) : dup(Descriptor(slot));
      int error = errno;
      if (result >= 0) descriptors[other] = result;
      Result(result, error, result < 0 || std::strcmp(command, "dupfd") != 0 || result >= flags);
    } else if (std::strcmp(command, "dup2") == 0) {
      if (std::sscanf(line, "%*s %d %d", &slot, &other) != 2 || !Slot(other) || Descriptor(other) < 0) return Failure("dup2 command shape");
      errno = 123;
      int result = dup2(Descriptor(slot), Descriptor(other));
      int error = errno;
      Result(result, error);
    } else if (std::strcmp(command, "close") == 0 || std::strcmp(command, "fclose") == 0) {
      if (std::sscanf(line, "%*s %d", &slot) != 1 || !Slot(slot)) return Failure("close command shape");
      int fd = Descriptor(slot);
      errno = 123;
      int result;
      if (command[0] == 'f') {
        if (!streams[slot]) return Failure("missing actual stream");
        result = std::fclose(streams[slot]); streams[slot] = nullptr;
      } else result = close(fd);
      int error = errno;
      if (result == 0) Forget(fd);
      Result(result, error);
    } else if (std::strcmp(command, "freopen") == 0) {
      if (std::sscanf(line, "%*s %d %d", &slot, &other) != 2 || !Slot(slot) || !streams[slot] || other < 0 || other > 5) return Failure("freopen command shape");
      int old = Descriptor(slot);
      errno = 123;
      FILE* stream = std::freopen(argv[other + 1], "r+", streams[slot]);
      int error = errno;
      Forget(old); streams[slot] = stream;
      if (stream) descriptors[slot] = fileno(stream);
      Result(stream ? 0 : -1, error);
    } else if (std::strcmp(command, "contended_stdio") == 0) {
      if (std::sscanf(line, "%*s %d %d", &slot, &other) != 2 || !Slot(slot) || !streams[slot] ||
          !Slot(other) || slot == other || Descriptor(other) < 0) return Failure("contended stdio command shape");
      // The controller bounds this entire command externally. A historical
      // registry-before-stream implementation can block in close(otherfd)
      // while fclose waits for this real CRT/glibc stream lock. No fake lock
      // callback or implementation hook participates in the two operations.
      FILE* stream = streams[slot];
      int stream_fd = Descriptor(slot), close_fd = Descriptor(other);
      std::atomic<bool> entering{false}, completed{false};
      int stream_result = -1, stream_error = 0;
      LockStream(stream);
      std::thread closer([&] {
        entering.store(true, std::memory_order_release);
        errno = 123;
        stream_result = std::fclose(stream);
        stream_error = errno;
        completed.store(true, std::memory_order_release);
      });
      auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!entering.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= limit) std::_Exit(93);
        std::this_thread::yield();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      bool contended = !completed.load(std::memory_order_acquire);
      errno = 123;
      int close_result = close(close_fd), close_error = errno;
      UnlockStream(stream);
      while (!completed.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= limit) std::_Exit(94);
        std::this_thread::yield();
      }
      closer.join();  // Completion was observed; no unbounded callback remains.
      streams[slot] = nullptr;
      if (stream_result == 0) Forget(stream_fd);
      if (close_result == 0) Forget(close_fd);
      std::printf("{\"result\":%d,\"errno\":%d,\"stream_result\":%d,\"stream_errno\":%d,\"contended\":%s}\n",
          close_result, close_error, stream_result, stream_error, contended ? "true" : "false");
      std::fflush(stdout);
    } else if (std::strcmp(command, "io") == 0) {
      if (std::sscanf(line, "%*s %d", &slot) != 1) return Failure("io command shape");
      const char payload[] = "advisory-I/O";
      char observed[sizeof(payload)]{};
      errno = 123;
      off_t position = lseek(Descriptor(slot), 12, SEEK_SET);
      ssize_t written = position < 0 ? -1 : write(Descriptor(slot), payload, sizeof(payload));
      position = lseek(Descriptor(slot), 12, SEEK_SET);
      ssize_t read_bytes = position < 0 ? -1 : read(Descriptor(slot), observed, sizeof(observed));
      int error = errno;
      Result(written == static_cast<ssize_t>(sizeof(payload)) && read_bytes == written &&
             std::memcmp(payload, observed, sizeof(payload)) == 0 ? 0 : -1, error);
    } else if (std::strcmp(command, "lock") == 0) {
      int kind, whence, pid, pointer;
      long long start, length;
      if (std::sscanf(line, "%*s %d %d %d %d %lld %lld %d %d", &slot, &flags, &kind, &whence,
                      &start, &length, &pid, &pointer) != 8 || pointer < 0 || pointer > 5) return Failure("lock command shape");
      std::memset(memory, 0xa5, kPage * 2);
      size_t offset = pointer == 3 || pointer == 4 ? kPage - 16 : 64;
      auto* input = reinterpret_cast<struct flock*>(memory + offset);
      input->l_type = static_cast<short>(kind); input->l_whence = static_cast<short>(whence);
      input->l_start = start; input->l_len = length; input->l_pid = pid;
      void* protected_page = pointer == 3 || pointer == 4 ? memory + kPage : memory;
      if (pointer >= 2 && mprotect(protected_page, kPage, pointer == 4 || pointer == 5 ? PROT_NONE : PROT_READ)) return Failure("fixture page protection");
      errno = 123;
      int result = fcntl(Descriptor(slot), flags, pointer == 1 ? reinterpret_cast<struct flock*>(1) : input);
      int error = errno;
      if (pointer >= 2 && mprotect(protected_page, kPage, PROT_READ | PROT_WRITE)) return Failure("fixture page restore");
      bool canary = true;
      for (size_t index = 1; index <= 8; ++index) if (memory[offset - index] != 0xa5 || memory[offset + sizeof(*input) + index - 1] != 0xa5) canary = false;
      std::printf("{\"result\":%d,\"errno\":%d,\"type\":%d,\"whence\":%d,\"start\":%lld,\"length\":%lld,\"pid\":%d,\"canary\":%s}\n",
          result, error, input->l_type, input->l_whence, static_cast<long long>(input->l_start),
          static_cast<long long>(input->l_len), input->l_pid, canary ? "true" : "false");
      std::fflush(stdout);
    } else return Failure("unknown command");
  }
  for (size_t slot = 0; slot < kSlots; ++slot) if (streams[slot]) {
    int fd = fileno(streams[slot]);
    if (std::fclose(streams[slot])) return Failure("final stream cleanup");
    streams[slot] = nullptr; Forget(fd);
  }
  for (int& descriptor : descriptors) if (descriptor >= 0) {
    int fd = descriptor;
    if (close(fd)) return Failure("final descriptor cleanup");
    Forget(fd);
  }
  if (munmap(memory, kPage * 2)) return Failure("final memory cleanup");
  std::puts("{\"bye\":true}");
  std::fflush(stdout);
  return 0;
}
