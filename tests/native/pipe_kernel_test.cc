// Shared public pipe contracts plus explicit Windows capability failures.
// Every potentially blocking phase has the test runner's process deadline.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <gesher/file_description.h>
#endif

static int checks, failures;
static void Check(const char* name, bool ok) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL pipe kernel: %s errno=%d\n", name, errno); }
}
static void Phase(const char* name) { std::printf("PIPE_KERNEL_PHASE %s\n", name); std::fflush(stdout); }
static void Pause() { timespec delay{0, 1000000}; nanosleep(&delay, nullptr); }
static void AwaitActive(int fd) {
#ifdef _WIN32
  bool active = false;
  for (int retry = 0; retry != 2000 && !active; ++retry) {
    pollfd descriptor{fd, POLLIN, 0};
    active = poll(&descriptor, 1, 0) == -1 && errno == EOPNOTSUPP;
    if (!active) Pause();
  }
  Check("owned operation reached pipe admission", active);
#else
  (void)fd;
  for (int retry = 0; retry != 10; ++retry) Pause();
#endif
}

struct RaceRead { int fd, result = -9, error = 0; std::atomic<bool>* start; };
static void* ReadOnce(void* raw) {
  auto* state = static_cast<RaceRead*>(raw);
  while (!state->start->load()) Pause();
  char byte;
  state->result = read(state->fd, &byte, 1);
  state->error = errno;
  return nullptr;
}
static void Admission() {
  Phase("admission-and-competing-readers");
  int ends[2] = {-1, -1};
  Check("pipe2 creates actual nonblocking endpoints", pipe2(ends, O_CLOEXEC | O_NONBLOCK) == 0);
  char bytes[4096]; std::memset(bytes, 'a', sizeof(bytes));
  unsigned accepted = 0;
  int result = 0;
  while (accepted < 16 * 1024 * 1024 && (result = write(ends[1], bytes, sizeof(bytes))) > 0) {
    Check("PIPE_BUF writes are complete", result == static_cast<int>(sizeof(bytes)));
    accepted += static_cast<unsigned>(result);
  }
  Check("full pipe reports EAGAIN", accepted > 0 && result == -1 && errno == EAGAIN);
  Check("full scalar write reports EAGAIN", write(ends[1], bytes, 1) == -1 && errno == EAGAIN);
  pollfd ready{ends[1], POLLOUT, 0};
  Check("full pipe is not writable", poll(&ready, 1, 0) == 0 && ready.revents == 0);
  Check("partial drain succeeds", read(ends[0], bytes, 1024) == 1024);
  iovec vector[2]{{bytes, 2048}, {bytes + 2048, 2048}};
  int before = 0, after = 0;
  Check("FIONREAD reports actual buffered bytes", ioctl(ends[0], FIONREAD, &before) == 0);
  Check("full vector cannot partially admit PIPE_BUF", writev(ends[1], vector, 2) == -1 && errno == EAGAIN);
  Check("failed atomic vector leaves bytes unchanged", ioctl(ends[0], FIONREAD, &after) == 0 && before == after);
  while (read(ends[0], bytes, sizeof(bytes)) > 0) {}
  Check("drained pipe reports EAGAIN", errno == EAGAIN);
  for (int iteration = 0; iteration != 16; ++iteration) {
    Check("one racing byte written", write(ends[1], "r", 1) == 1);
    std::atomic<bool> start{false};
    RaceRead a{ends[0], -9, 0, &start}, b{ends[0], -9, 0, &start};
    pthread_t first, second;
    Check("competing read threads start", pthread_create(&first, nullptr, ReadOnce, &a) == 0 &&
          pthread_create(&second, nullptr, ReadOnce, &b) == 0);
    start.store(true);
    Check("both nonblocking readers return", pthread_join(first, nullptr) == 0 && pthread_join(second, nullptr) == 0);
    Check("one reader wins and other sees EAGAIN", (a.result == 1 && b.result == -1 && b.error == EAGAIN) ||
          (b.result == 1 && a.result == -1 && a.error == EAGAIN));
  }
  close(ends[1]);
  ready = {ends[0], POLLIN, 0};
  Check("last writer closure reports HUP without invented bytes", poll(&ready, 1, 0) == 1 &&
        ready.revents == POLLHUP && read(ends[0], bytes, 1) == 0);
  close(ends[0]);
}

static void Reopen() {
  Phase("procfs-independent-open-and-access-direction");
  for (int source = 0; source != 2; ++source) {
    for (int access : {O_RDONLY, O_WRONLY, O_RDWR}) {
      int ends[2] = {-1, -1};
      Check("reopen fixture pipe", pipe2(ends, O_NONBLOCK) == 0);
      int alias = dup(ends[source]);
      Check("dup shares flags", alias >= 0 && fcntl(alias, F_SETFL, 0) == 0 &&
            (fcntl(ends[source], F_GETFL) & O_NONBLOCK) == 0);
      Check("restore original nonblocking status", fcntl(ends[source], F_SETFL, O_NONBLOCK) == 0);
      char path[80]; std::snprintf(path, sizeof(path), "/proc/self/fd/%d", ends[source]);
      int opened = open(path, access | O_NONBLOCK | O_CLOEXEC);
#ifdef _WIN32
      Check("unsupported fresh pipe open reports an operation error", opened == -1 && errno == EOPNOTSUPP);
      if (opened >= 0) close(opened);
      Check("failed reopen preserves original and alias status", (fcntl(ends[source], F_GETFL) & O_NONBLOCK) != 0 &&
            (fcntl(alias, F_GETFL) & O_NONBLOCK) != 0);
      Check("failed reopen does not change descriptor flags", fcntl(ends[source], F_GETFD) == 0 && fcntl(alias, F_GETFD) == 0);
      char byte = 0;
      Check("failed reopen leaves the real byte queue usable", write(ends[1], "i", 1) == 1 && read(ends[0], &byte, 1) == 1 && byte == 'i');
      Check("unsupported reopen without CLOEXEC fails too", open(path, access | O_NONBLOCK) == -1 && errno == EOPNOTSUPP);
#else
      Check("procfs reopens pipe with requested access", opened >= 0 && (fcntl(opened, F_GETFL) & O_ACCMODE) == access);
      if (opened >= 0) {
        Check("procfs honors CLOEXEC", fcntl(opened, F_GETFD) == FD_CLOEXEC);
        Check("procfs creates independent status description", fcntl(opened, F_SETFL, 0) == 0 &&
              (fcntl(ends[source], F_GETFL) & O_NONBLOCK) != 0 && (fcntl(alias, F_GETFL) & O_NONBLOCK) != 0);
        Check("reopened endpoint can return to nonblocking", fcntl(opened, F_SETFL, O_NONBLOCK) == 0);
        char byte = 0;
        if (access != O_WRONLY) {
          Check("write to original pipe", write(ends[1], "i", 1) == 1);
          Check("reopened reader consumes the original buffer", read(opened, &byte, 1) == 1 && byte == 'i');
          while (read(ends[0], &byte, 1) > 0) {}
        }
        if (access != O_RDONLY) {
          Check("reopened writer reaches original buffer", write(opened, "o", 1) == 1 &&
                read(ends[0], &byte, 1) == 1 && byte == 'o');
        }
        close(opened);
      }
      int inherited = open(path, access | O_NONBLOCK);
      Check("procfs does not invent CLOEXEC", inherited >= 0 && fcntl(inherited, F_GETFD) == 0);
      if (inherited >= 0) close(inherited);
#endif
      close(alias); close(ends[0]); close(ends[1]);
    }
  }
}

static void Cancellation() {
  Phase("blocking-cancellation-and-alias-lifetime");
  int ends[2] = {-1, -1}; Check("blocking cancellation fixture", pipe(ends) == 0);
  std::atomic<bool> start{true};
  Phase("status-query-and-F_SETFL-during-blocked-read");
  RaceRead alias_wait{ends[0], -9, 0, &start};
  pthread_t waiting;
  Check("alias-status reader starts", pthread_create(&waiting, nullptr, ReadOnce, &alias_wait) == 0);
  AwaitActive(ends[0]);
  Check("F_GETFL does not wait for blocked I/O", fcntl(ends[0], F_GETFL) >= 0);
#ifdef _WIN32
  Check("active mode transition fails without changing flags", fcntl(ends[0], F_SETFL, O_NONBLOCK) == -1 &&
        errno == EOPNOTSUPP && (fcntl(ends[0], F_GETFL) & O_NONBLOCK) == 0);
  pollfd busy{ends[0], POLLIN, 0};
  Check("active zero-timeout poll returns an error without false readiness", poll(&busy, 1, 0) == -1 &&
        errno == EOPNOTSUPP && busy.revents == 0);
  int available = 919;
  Check("active FIONREAD fails without manufacturing a count", ioctl(ends[0], FIONREAD, &available) == -1 &&
        errno == EOPNOTSUPP && available == 919);
#else
  Check("F_SETFL does not wait for blocked I/O", fcntl(ends[0], F_SETFL, O_NONBLOCK) == 0);
#endif
  Check("peer can complete existing read", write(ends[1], "s", 1) == 1);
  Check("alias-status reader joins", pthread_join(waiting, nullptr) == 0);
  Check("existing read delivered the real byte", alias_wait.result == 1);
  Check("idle endpoint permits mode changes", fcntl(ends[0], F_SETFL, O_NONBLOCK) == 0);
  char spare;
  while (read(ends[0], &spare, 1) > 0) {}
  Check("blocking mode restored", fcntl(ends[0], F_SETFL, 0) == 0);
  Phase("repeated-cancellation-retirement");
#ifdef _WIN32
  DWORD handles_before = 0;
#endif
  for (int iteration = 0; iteration != 17; ++iteration) {
    RaceRead state{ends[0], -9, 0, &start};
    pthread_t caller;
    Check("blocking read caller starts", pthread_create(&caller, nullptr, ReadOnce, &state) == 0);
    AwaitActive(ends[0]);
    Check("blocking caller can be canceled", pthread_cancel(caller) == 0);
    void* result = nullptr;
    Check("canceled reader joins", pthread_join(caller, &result) == 0 && result == PTHREAD_CANCELED);
#ifdef _WIN32
    // Warm the shared Windows thread-pool infrastructure once; compare actual
    // per-operation resources only after its first cancellation callback.
    if (iteration == 0) GetProcessHandleCount(GetCurrentProcess(), &handles_before);
#endif
  }
#ifdef _WIN32
  DWORD handles_after = 0; GetProcessHandleCount(GetCurrentProcess(), &handles_after);
  Check("cancellation retains no kernel handles", handles_after == handles_before);
  auto snapshot = gesher::CaptureDescriptor(ends[0]);
  const int old_slot = ends[0];
  Check("snapshot retains old pipe during descriptor reuse", static_cast<bool>(snapshot) && close(ends[0]) == 0);
  int replacement = open("/dev/null", O_RDONLY);
  Check("descriptor replacement", replacement >= 0 && dup2(replacement, old_slot) == old_slot);
  char byte;
  Check("retained pipe reads from exact old endpoint", write(ends[1], "p", 1) == 1 &&
        gesher::ReadFileDescription(snapshot, &byte, 1) == 1 && byte == 'p');
  Check("replacement remains NUL", read(old_slot, &byte, 1) == 0);
  if (replacement != old_slot) close(replacement);
  snapshot.reset();
#endif
  close(ends[0]); close(ends[1]);
}

struct FullWrite { int fd; const char* data; size_t size; ssize_t result = -1; };
static void* WriteAll(void* raw) {
  auto* state = static_cast<FullWrite*>(raw);
  state->result = write(state->fd, state->data, state->size);
  return nullptr;
}
static void BlockingCompletion() {
  Phase("blocking-full-write-with-real-reader");
  int ends[2]; Check("blocking writer fixture", pipe(ends) == 0);
  std::vector<char> payload(65537, 'b');
  FullWrite state{ends[1], payload.data(), payload.size()};
  pthread_t writer;
  Check("blocking writer starts", pthread_create(&writer, nullptr, WriteAll, &state) == 0);
  char buffer[8192];
  size_t total = 0;
  bool contents = true;
  while (total < payload.size()) {
    ssize_t count = read(ends[0], buffer, std::min(sizeof(buffer), payload.size() - total));
    if (count <= 0) break;
    for (ssize_t i = 0; i < count; ++i) contents = contents && buffer[i] == 'b';
    total += static_cast<size_t>(count);
  }
  Check("blocking write completes the whole real request", pthread_join(writer, nullptr) == 0 &&
        state.result == static_cast<ssize_t>(payload.size()) && total == payload.size() && contents);
#ifdef _WIN32
  int available = 919;
  Check("writer FIONREAD is unsupported instead of quota subtraction", ioctl(ends[1], FIONREAD, &available) == -1 &&
        errno == EOPNOTSUPP && available == 919);
#endif
  close(ends[0]); close(ends[1]);
}

struct DisabledRead {
  int fd;
  std::atomic<bool> entered{false}, completed{false};
  ssize_t result = -1;
  char byte = 0;
};
static void* ReadWithCancellationDisabled(void* raw) {
  auto* state = static_cast<DisabledRead*>(raw);
  int previous;
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous);
  state->entered.store(true);
  state->result = read(state->fd, &state->byte, 1);
  state->completed.store(true);
  pthread_setcancelstate(previous, nullptr);
  pthread_testcancel();
  return nullptr;
}
static void DisabledCancellation() {
  Phase("disabled-cancellation-preserves-I/O-and-pending-request");
  int ends[2]; Check("disabled cancellation fixture", pipe(ends) == 0);
  DisabledRead state{ends[0]};
  pthread_t reader;
  Check("disabled caller starts", pthread_create(&reader, nullptr, ReadWithCancellationDisabled, &state) == 0);
  while (!state.entered.load()) Pause();
  Check("disabled cancellation request is accepted", pthread_cancel(reader) == 0);
  for (int retry = 0; retry != 10; ++retry) Pause();
  Check("disabled cancellation did not end the read", !state.completed.load());
  Check("real writer completes disabled read", write(ends[1], "d", 1) == 1);
  void* result = nullptr;
  Check("pending cancellation survives reenable", pthread_join(reader, &result) == 0 && result == PTHREAD_CANCELED &&
        state.completed.load() && state.result == 1 && state.byte == 'd');
  close(ends[0]); close(ends[1]);
}

#ifdef _WIN32
static void NativeReopenEvidence() {
  Phase("native-ReOpenFile-observations");
  for (int endpoint = 0; endpoint != 2; ++endpoint) {
    for (DWORD access : {FILE_READ_DATA, FILE_WRITE_DATA, FILE_READ_DATA | FILE_WRITE_DATA}) {
      int ends[2] = {-1, -1}; if (pipe2(ends, O_NONBLOCK) < 0) { Check("native reopen probe pipe", false); return; }
      HANDLE source = reinterpret_cast<HANDLE>(_get_osfhandle(ends[endpoint]));
      HANDLE reopened = ReOpenFile(source, access | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
          FILE_SHARE_READ | FILE_SHARE_WRITE, 0);
      DWORD error = reopened == INVALID_HANDLE_VALUE ? GetLastError() : 0;
      DWORD state = PIPE_WAIT, original = 0;
      const bool changed = reopened != INVALID_HANDLE_VALUE && SetNamedPipeHandleState(reopened, &state, nullptr, nullptr);
      const bool queried = GetNamedPipeHandleStateW(source, &original, nullptr, nullptr, nullptr, nullptr, 0);
      bool read_queue = false, write_queue = false;
      state = PIPE_NOWAIT;
      const bool nowait = reopened != INVALID_HANDLE_VALUE && SetNamedPipeHandleState(reopened, &state, nullptr, nullptr);
      if (nowait) {
        HANDLE reader = reinterpret_cast<HANDLE>(_get_osfhandle(ends[0]));
        HANDLE writer = reinterpret_cast<HANDLE>(_get_osfhandle(ends[1]));
        DWORD count = 0;
        char byte = 0;
        if ((access & FILE_READ_DATA) != 0) {
          read_queue = WriteFile(writer, "r", 1, &count, nullptr) && count == 1 &&
              ReadFile(reopened, &byte, 1, &count, nullptr) && count == 1 && byte == 'r';
          if (!read_queue) ReadFile(reader, &byte, 1, &count, nullptr);
        }
        if ((access & FILE_WRITE_DATA) != 0) {
          write_queue = WriteFile(reopened, "w", 1, &count, nullptr) && count == 1 &&
              ReadFile(reader, &byte, 1, &count, nullptr) && count == 1 && byte == 'w';
        }
      }
      std::printf("PIPE_REOPEN_NATIVE endpoint=%d access=%lu opened=%d error=%lu set_wait=%d original_query=%d original_nowait=%d set_nowait=%d reads_original_queue=%d writes_original_queue=%d\n",
          endpoint, access, reopened != INVALID_HANDLE_VALUE, error, changed, queried,
          (original & PIPE_NOWAIT) != 0, nowait, read_queue, write_queue);
      if (reopened != INVALID_HANDLE_VALUE) CloseHandle(reopened);
      close(ends[0]); close(ends[1]);
    }
  }
}
#endif
int main() {
#ifdef _WIN32
  // Capture independent native observations before any blocking regression
  // can time out. These observations never waive the public assertions.
  NativeReopenEvidence();
#endif
  Admission();
  BlockingCompletion();
  Cancellation();
  DisabledCancellation();
  Reopen();
  std::printf("pipe kernel contracts: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
