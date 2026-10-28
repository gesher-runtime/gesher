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
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <wct.h>
#include <io.h>
#include "stack_probe.h"
#include "crt_lock_probe.h"
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

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
unsigned ThreadId() {
#ifdef _WIN32
  return GetCurrentThreadId();
#else
  return static_cast<unsigned>(syscall(SYS_gettid));
#endif
}
#ifdef _WIN32
constexpr DWORD kWaitChainNodeCapacity = 16;
struct WaitTrace {
  unsigned samples = 0;
  DWORD error = 0, count = 0;
  BOOL complete = FALSE, cycle = FALSE;
  bool contradictory = false;
  WAITCHAIN_NODE_INFO nodes[kWaitChainNodeCapacity]{};
};
WaitTrace wait_trace;
bool InvalidWaitTrace(const WaitTrace& trace, DWORD waiting, DWORD owner) {
  if (!trace.complete || trace.error != ERROR_SUCCESS || trace.cycle || trace.count == 0 ||
      trace.count > kWaitChainNodeCapacity) return true;
  const auto& first = trace.nodes[0];
  if (first.ObjectType != WctThreadType || first.ThreadObject.ProcessId != GetCurrentProcessId() ||
      first.ThreadObject.ThreadId != waiting) return true;
  if (trace.count == 1) return first.ObjectStatus != WctStatusRunning && first.ObjectStatus != WctStatusBlocked;
  return trace.count < 3 || first.ObjectStatus != WctStatusBlocked ||
      trace.nodes[1].ObjectType != WctCriticalSectionType || trace.nodes[2].ObjectType != WctThreadType ||
      trace.nodes[2].ThreadObject.ProcessId != GetCurrentProcessId() || trace.nodes[2].ThreadObject.ThreadId != owner;
}
bool SingleBlockedCloser(const WaitTrace& trace, DWORD waiting, DWORD owner) {
  return !trace.contradictory && !InvalidWaitTrace(trace, waiting, owner) && trace.count == 1 &&
      trace.nodes[0].ObjectStatus == WctStatusBlocked;
}
void DumpWaitTrace(unsigned waiting, unsigned owner, FILE* stream, int descriptor, bool completed) {
  std::fprintf(stderr, "ACTUAL_WCT samples=%u success=%d error=%lu count=%lu cycle=%d contradictory=%d closer=%u owner=%u FILE=%p fd=%d HANDLE=%p completed-before-unlock=%d\n",
      wait_trace.samples, wait_trace.complete, wait_trace.error, wait_trace.count, wait_trace.cycle,
      wait_trace.contradictory, waiting, owner, static_cast<void*>(stream), descriptor,
      reinterpret_cast<void*>(_get_osfhandle(descriptor)), completed);
  for (DWORD index = 0; index < wait_trace.count && index < kWaitChainNodeCapacity; ++index) {
    const auto& node = wait_trace.nodes[index];
    std::fprintf(stderr, "ACTUAL_WCT_NODE index=%lu type=%d status=%d pid=%lu tid=%lu\n", index,
        static_cast<int>(node.ObjectType), static_cast<int>(node.ObjectStatus),
        node.ObjectType == WctThreadType ? node.ThreadObject.ProcessId : 0UL,
        node.ObjectType == WctThreadType ? node.ThreadObject.ThreadId : 0UL);
  }
}
#endif
bool StreamWaitObserved(unsigned waiting, unsigned owner) {
#ifdef _WIN32
  unsigned samples = wait_trace.samples + 1;
  bool contradictory = wait_trace.contradictory;
  wait_trace = {}; wait_trace.samples = samples; wait_trace.contradictory = contradictory;
  HWCT session = OpenThreadWaitChainSession(0, nullptr);
  if (!session) { wait_trace.error = GetLastError(); wait_trace.contradictory = true; return false; }
  // The pinned mingw wct.h omits this SDK macro. Microsoft's canonical
  // WinSDK header defines WCT_MAX_NODE_COUNT as16:
  // https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/wct.h
#ifdef WCT_MAX_NODE_COUNT
  static_assert(WCT_MAX_NODE_COUNT == kWaitChainNodeCapacity);
#endif
  auto& nodes = wait_trace.nodes;
  DWORD& count = wait_trace.count;
  count = kWaitChainNodeCapacity;
  const bool complete = GetThreadWaitChain(session, 0, 0, waiting, &count, nodes, &wait_trace.cycle);
  const DWORD error = complete ? ERROR_SUCCESS : GetLastError();
  wait_trace.complete = complete; wait_trace.error = error;
  wait_trace.contradictory |= InvalidWaitTrace(wait_trace, waiting, owner);
  if (count > kWaitChainNodeCapacity || error == ERROR_MORE_DATA || error == ERROR_TOO_MANY_THREADS)
    std::fprintf(stderr, "COMMAND_WCT_INCOMPLETE count=%lu error=%lu\n", count, error);
  bool observed = complete && count >= 3 && count <= kWaitChainNodeCapacity &&
      nodes[0].ObjectType == WctThreadType && nodes[0].ThreadObject.ThreadId == waiting &&
      nodes[1].ObjectType == WctCriticalSectionType && nodes[2].ObjectType == WctThreadType &&
      nodes[2].ThreadObject.ThreadId == owner;
  CloseThreadWaitChainSession(session);
  return observed;
#else
  (void)owner;
  char path[96], state[128]{};
  std::snprintf(path, sizeof(path), "/proc/self/task/%u/wchan", waiting);
  // A stdio observer can itself wait for libc's global FILE-list lock while
  // fclose is waiting for our held target stream. Observe through raw syscalls
  // without opening or closing another FILE object during this contention.
  int file = open(path, O_RDONLY | O_CLOEXEC);
  if (file < 0) return false;
  ssize_t bytes = read(file, state, sizeof(state) - 1);
  int closed = close(file);
  bool observed = bytes > 0 && closed == 0 && std::strstr(state, "futex");
  if (observed) std::fprintf(stderr, "ACTUAL_STDIO_WCHAN closer=%u state=%s observer=raw-syscalls\n", waiting, state);
  // After publishing its TID the closer calls only fclose, so this actual
  // kernel futex wait is its stdio lock, not a scheduler-readiness inference.
  return observed;
#endif
}
#ifdef _WIN32
bool EnableOwnWaitChainProbe() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token)) return false;
  TOKEN_PRIVILEGES privileges{};
  privileges.PrivilegeCount = 1;
  bool result = LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &privileges.Privileges[0].Luid);
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  SetLastError(ERROR_SUCCESS);
  if (result) result = AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
  CloseHandle(token);
  return result;
}
#endif
}  // namespace

int main(int argc, char** argv) {
  if (!std::getenv("GITHUB_ACTIONS") || std::strcmp(std::getenv("GITHUB_ACTIONS"), "true") != 0) return 90;
  if (argc != 8) return 92;  // main, separate, hardlink, notdir child, empty, absent device/disk
#ifdef _WIN32
  // main's narrow argv uses the runner's ANSI code page. The common POSIX
  // path API takes UTF-8; obtain actual Unicode test paths losslessly from
  // the original Windows command line instead of testing replaced '?' bytes.
  std::string utf8_paths[7];
  int wide_count = 0;
  wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &wide_count);
  if (!wide) return Failure("actual wide command line");
  bool converted = wide_count == argc;
  for (int index = 1; converted && index < argc; ++index) {
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[index], -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) { converted = false; break; }
    utf8_paths[index - 1].resize(static_cast<size_t>(bytes));
    converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[index], -1,
        utf8_paths[index - 1].data(), bytes, nullptr, nullptr) == bytes;
    argv[index] = utf8_paths[index - 1].data();
  }
  LocalFree(wide);
  if (!converted) return Failure("actual UTF-8 fixture paths");
#endif
#ifndef _WIN32
  struct rlimit limit{8192, 8192};
  if (setrlimit(RLIMIT_NOFILE, &limit)) return Failure("actual Linux fd limit");
#else
  if (!EnableOwnWaitChainProbe()) return Failure("actual own-thread WCT privilege");
  native3_crt_lock_probe::Layout crt_layout;
  if (!crt_layout.Initialize()) return Failure("actual named CRT lock implementation decode");
#endif
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
    if (std::strcmp(command, "crash") == 0) std::_Exit(23);  // No fd/stdio/registry cleanup.
    int slot = -1, other = -1, flags = 0;
    if (std::strcmp(command, "open") == 0 || std::strcmp(command, "stream") == 0) {
      if (std::sscanf(line, "%*s %d %d %d", &slot, &other, &flags) != 3 || !Slot(slot) || other < 0 || other > 6 || Descriptor(slot) >= 0) return Failure("open command shape");
      errno = 123;
      int result;
      if (command[0] == 's') {
        streams[slot] = std::fopen(argv[other + 1], flags == 0 ? "r" : flags == 1 ? "w" : "r+");
        result = streams[slot] ? fileno(streams[slot]) : -1;
      } else result = open(argv[other + 1], flags);
      int error = errno;
      if (result >= 0) descriptors[slot] = result;
      Result(result, error);
    } else if (std::strcmp(command, "dup") == 0 || std::strcmp(command, "dupfd") == 0 ||
               std::strcmp(command, "dupfdce") == 0) {
      if (std::sscanf(line, "%*s %d %d %d", &slot, &other, &flags) != 3 || !Slot(other) || Descriptor(other) >= 0) return Failure("dup command shape");
      errno = 123;
      const bool minimum_command = std::strcmp(command, "dup") != 0;
      int result = minimum_command ? fcntl(Descriptor(slot),
          std::strcmp(command, "dupfdce") == 0 ? F_DUPFD_CLOEXEC : F_DUPFD, flags) : dup(Descriptor(slot));
      int error = errno;
      if (result >= 0) descriptors[other] = result;
      std::printf("{\"result\":%d,\"errno\":%d,\"minimum\":%s,\"minimum_offset\":%d}\n", result < 0 ? -1 : 0,
                  error, result < 0 || !minimum_command || result >= flags ? "true" : "false",
                  result >= 0 && minimum_command ? result - flags : -1);
      std::fflush(stdout);
    } else if (std::strcmp(command, "getfd") == 0 || std::strcmp(command, "setfd") == 0) {
      if (std::sscanf(line, "%*s %d %d", &slot, &flags) != 2 || !Slot(slot)) return Failure("fd flags command shape");
      errno = 123;
      int result = fcntl(Descriptor(slot), command[0] == 'g' ? F_GETFD : F_SETFD, flags);
      int error = errno;
      bool inherited = false;
      if (result >= 0 && command[0] == 'g') {
#ifdef _WIN32
        DWORD native_flags = 0;
        if (!GetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(Descriptor(slot))), &native_flags)) return Failure("actual native inherit flag");
        inherited = (native_flags & HANDLE_FLAG_INHERIT) != 0;
#else
        inherited = (result & FD_CLOEXEC) == 0;
#endif
      }
      std::printf("{\"result\":%d,\"errno\":%d,\"flags\":%d,\"inherited\":%s}\n", result < 0 ? -1 : 0, error,
                  command[0] == 'g' ? result : flags, inherited ? "true" : "false");
      std::fflush(stdout);
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
      if (std::sscanf(line, "%*s %d %d", &slot, &other) != 2 || !Slot(slot) || !streams[slot] || other < 0 || other > 6) return Failure("freopen command shape");
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
      std::atomic<unsigned> entering{0};
#ifdef _WIN32
      std::atomic<uint64_t> entering_birth{0};
      bool stack_sampled = false;
#endif
      std::atomic<bool> completed{false};
      int stream_result = -1, stream_error = 0;
      const auto started = std::chrono::steady_clock::now();
      const auto limit = started + std::chrono::seconds(5);
#ifdef _WIN32
      const ULONGLONG start_tick = GetTickCount64(), deadline_tick = start_tick + 5000;
      static unsigned cycle_count = 0;
      const unsigned cycle = ++cycle_count;
      auto phase = [&](const char* name) {
        std::fprintf(stderr, "ACTUAL_STDIO_CLOCK cycle=%u phase=%s tick=%llu deadline=%llu\n", cycle, name,
            static_cast<unsigned long long>(GetTickCount64()), static_cast<unsigned long long>(deadline_tick));
      };
      auto exited_before_limit = [&](HANDLE thread) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(limit - std::chrono::steady_clock::now()).count();
        return remaining > 0 && WaitForSingleObject(thread, static_cast<DWORD>(remaining)) == WAIT_OBJECT_0 &&
            std::chrono::steady_clock::now() < limit;
      };
      phase("start");
      wait_trace = {};
      FILETIME owner_created{}, owner_exited{}, owner_kernel{}, owner_user{};
      if (!GetThreadTimes(GetCurrentThread(), &owner_created, &owner_exited, &owner_kernel, &owner_user))
        return Failure("actual stream owner birth");
      const DWORD stream_owner = GetCurrentThreadId();
      const uint64_t stream_owner_birth = native3_stack_probe::Birth(owner_created);
      if (stream_owner_birth == 0) return Failure("actual stream owner birth is empty");
      native3_crt_lock_probe::Snapshot before{}, held{}, waiting{};
      if (!crt_layout.Read(stream, &before) || before.owner != 0 || before.recursion != 0)
        return Failure("actual stream lock before owner acquisition");
      FILE* other_stream = std::tmpfile();
      FILE* completed_stream = std::tmpfile();
      if (!other_stream || !completed_stream) {
        if (other_stream) std::fclose(other_stream);
        if (completed_stream) std::fclose(completed_stream);
        return Failure("actual negative-control streams");
      }
      std::atomic<DWORD> control_tid{0};
      std::atomic<uint64_t> control_birth{0};
      std::atomic<bool> control_release{false}, control_completed{false};
      int control_close = -1;
      std::thread completed_closer([&] {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
          control_birth.store(native3_stack_probe::Birth(created), std::memory_order_release);
        control_tid.store(GetCurrentThreadId(), std::memory_order_release);
        while (!control_release.load(std::memory_order_acquire)) std::this_thread::yield();
        control_close = std::fclose(completed_stream);
        control_completed.store(true, std::memory_order_release);
      });
      while (!control_tid.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= limit) std::_Exit(97);
        std::this_thread::yield();
      }
      HANDLE completed_thread = native3_stack_probe::OpenOwnedForExit(control_tid.load(std::memory_order_acquire),
          control_birth.load(std::memory_order_acquire));
      if (!completed_thread) std::_Exit(97);
      control_release.store(true, std::memory_order_release);
      if (!exited_before_limit(completed_thread) || !CloseHandle(completed_thread)) std::_Exit(97);
      completed_closer.join();
      if (!control_completed.load(std::memory_order_acquire) || control_close != 0) std::_Exit(97);
      // The distinct live sibling has the SAME owner/recursion as the target,
      // isolating FILE/CS identity from a simple owner-only predicate.
      LockStream(other_stream);
      native3_crt_lock_probe::Snapshot other_held{};
      if (!crt_layout.Read(other_stream, &other_held) ||
          !crt_layout.Matches(other_stream, other_held, stream_owner, false)) std::_Exit(97);
#endif
      LockStream(stream);
#ifdef _WIN32
      if (!crt_layout.Read(stream, &held) || held.address != before.address ||
          !crt_layout.Matches(stream, held, stream_owner, false)) {
        Failure("actual stream lock held by expected owner"); std::_Exit(96);
      }
#endif
      std::thread closer([&] {
#ifdef _WIN32
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
          entering_birth.store(native3_stack_probe::Birth(created), std::memory_order_release);
#endif
        entering.store(ThreadId(), std::memory_order_release);
        errno = 123;
        stream_result = std::fclose(stream);
        stream_error = errno;
        completed.store(true, std::memory_order_release);
      });
      while (!entering.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= limit) std::_Exit(93);
        std::this_thread::yield();
      }
#ifdef _WIN32
      HANDLE closer_exit = native3_stack_probe::OpenOwnedForExit(entering.load(std::memory_order_acquire),
          entering_birth.load(std::memory_order_acquire));
      if (!closer_exit) std::_Exit(97);
      phase("held-and-entered");
#endif
      std::fprintf(stderr, "ACTUAL_STDIO_PHASE stream-held closer-entered tid=%u\n", entering.load(std::memory_order_acquire));
      bool contended = false;
      while (std::chrono::steady_clock::now() < limit) {
        contended |= StreamWaitObserved(entering.load(std::memory_order_acquire), ThreadId());
#ifdef _WIN32
        if (wait_trace.contradictory) { DumpWaitTrace(entering.load(), stream_owner, stream, stream_fd, completed.load()); std::_Exit(98); }
        if (!stack_sampled && wait_trace.samples >= 3 && wait_trace.nodes[0].ObjectStatus == WctStatusBlocked) {
          stack_sampled = true;
          if (!native3_stack_probe::Capture(entering.load(std::memory_order_acquire),
                  entering_birth.load(std::memory_order_acquire), stream, stream_fd, other_stream)) {
            Failure("actual owned closer stack capture");
            std::_Exit(95);
          }
          break;  // Both evidence routes still require every strong witness below.
        }
#else
        if (contended) break;
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
#ifdef _WIN32
      // Read only while this real owner still holds the stream lock. Never
      // inspect the FILE/embedded lock after unlock permits fclose to free it.
      if (!stack_sampled || completed.load(std::memory_order_acquire) || !crt_layout.Read(stream, &waiting) ||
          waiting.address != held.address || !crt_layout.Matches(stream, waiting, stream_owner, false)) {
        Failure("actual live FILE lock owner during closer wait"); std::_Exit(96);
      }
      bool wrong_birth = true;
      if (entering_birth.load() == stream_owner_birth ||
          !native3_stack_probe::MatchesBirth(entering.load(), stream_owner_birth, &wrong_birth) || wrong_birth ||
          crt_layout.Matches(stream, other_held, stream_owner, false) ||
          crt_layout.Matches(stream, held, entering.load(), false) ||
          crt_layout.Matches(stream, held, stream_owner, control_completed.load()) ||
          !crt_layout.RejectsUnsupportedCode()) std::_Exit(99);
      std::fprintf(stderr, "ACTUAL_CRT_NEGATIVE_CONTROLS wrong-live-FILE-CS=reject wrong-live-owner=reject actual-other-birth=reject completed-real-closer=reject unsupported-code-copy=reject call-other-FILE-argument=reject\n");
      native3_crt_lock_probe::Report("negative-sibling", other_stream, other_held, stream_owner, stream_owner_birth);
      std::fprintf(stderr, "ACTUAL_CRT_NEGATIVE_VALUES cycle=%u closer=%u closer-birth=%llu wrong-owner=%u wrong-birth=%llu completed-control-tid=%lu completed-control-birth=%llu completed-control-close=%d completed-control-signaled=true\n",
          cycle, entering.load(), static_cast<unsigned long long>(entering_birth.load()), entering.load(),
          static_cast<unsigned long long>(stream_owner_birth), control_tid.load(),
          static_cast<unsigned long long>(control_birth.load()), control_close);
      WaitTrace error_copy = wait_trace, cycle_copy = wait_trace, wrong_thread_copy = wait_trace;
      error_copy.complete = FALSE; error_copy.error = ERROR_INVALID_PARAMETER;
      cycle_copy.cycle = TRUE;
      wrong_thread_copy.nodes[0].ThreadObject.ThreadId = stream_owner;
      if (!InvalidWaitTrace(error_copy, entering.load(), stream_owner) ||
          !InvalidWaitTrace(cycle_copy, entering.load(), stream_owner) ||
          !InvalidWaitTrace(wrong_thread_copy, entering.load(), stream_owner)) std::_Exit(99);
      std::fprintf(stderr, "WCT_NEGATIVE_METADATA_COPIES error=reject cycle=reject wrong-live-thread=reject not-actual-OS-cycle=true\n");
      WaitTrace running_copy = wait_trace, sticky_copy = wait_trace;
      running_copy.count = sticky_copy.count = 1;
      running_copy.nodes[0].ObjectStatus = WctStatusRunning;
      sticky_copy.nodes[0].ObjectStatus = WctStatusBlocked;
      sticky_copy.contradictory = true;
      if (SingleBlockedCloser(running_copy, entering.load(), stream_owner) ||
          SingleBlockedCloser(sticky_copy, entering.load(), stream_owner)) std::_Exit(99);
      std::fprintf(stderr, "STDIO_ROUTE_METADATA_COPIES single-running=reject sticky-contradiction=reject not-actual-OS-waits=true\n");
      phase("sample-and-negatives");
      native3_crt_lock_probe::Report("before", stream, before, stream_owner, stream_owner_birth);
      native3_crt_lock_probe::Report("held", stream, held, stream_owner, stream_owner_birth);
      native3_crt_lock_probe::Report("waiting", stream, waiting, stream_owner, stream_owner_birth);
      DumpWaitTrace(entering.load(std::memory_order_acquire), ThreadId(), stream, stream_fd,
          completed.load(std::memory_order_acquire));
#endif
      errno = 123;
      int close_result = close(close_fd), close_error = errno;
      std::fprintf(stderr, "ACTUAL_STDIO_PHASE independent-fd-closed result=%d closer-completed-before-unlock=%d\n",
          close_result, completed.load(std::memory_order_acquire));
#ifdef _WIN32
      if (completed.load(std::memory_order_acquire)) std::_Exit(99);
      phase("independent-close");
#endif
      UnlockStream(stream);
#ifdef _WIN32
      phase("unlock");
#endif
      std::fprintf(stderr, "ACTUAL_STDIO_PHASE stream-unlocked\n");
      while (!completed.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= limit) std::_Exit(94);
        std::this_thread::yield();
      }
#ifdef _WIN32
      if (!exited_before_limit(closer_exit) || !CloseHandle(closer_exit)) std::_Exit(94);
#endif
      closer.join();  // Completion was observed; no unbounded callback remains.
#ifdef _WIN32
      phase("actual-thread-exit-and-join");
      UnlockStream(other_stream);
      if (std::fclose(other_stream) != 0 || std::chrono::steady_clock::now() >= limit) std::_Exit(97);
      phase("control-cleanup");
      // Decide only after the complete live-lock witness, all real rejection
      // controls, independent close, unlock, actual fclose and signaled exit/
      // join have finished under the SAME original deadline. A single WCT
      // node supplies no owner proof; the decoded CRT/FILE/call-frame conjunction
      // above supplies that proof only for this controlled stdio regression.
      const bool full_wct = contended && !wait_trace.contradictory &&
          !InvalidWaitTrace(wait_trace, entering.load(), stream_owner) && wait_trace.count >= 3;
      const bool single_blocked = SingleBlockedCloser(wait_trace, entering.load(), stream_owner);
      if ((!full_wct && !single_blocked) || close_result != 0 || stream_result != 0 ||
          !completed.load(std::memory_order_acquire) || std::chrono::steady_clock::now() >= limit) std::_Exit(99);
      contended = true;
      std::fprintf(stderr, "ACTUAL_STDIO_ROUTE cycle=%u route=%s full-wct=%d single-blocked=%d strong-witness=true independent-close=%d fclose=%d preunlock-completed=false signaled-exit=true joined=true\n",
          cycle, full_wct ? "full-wct" : "decoded-crt-lock", full_wct, single_blocked, close_result, stream_result);
#endif
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
