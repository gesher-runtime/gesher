#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include <winternl.h>
#endif

static int checks;
static int failures;

static void Check(const char* name, bool success) {
  ++checks;
  if (!success) {
    ++failures;
    std::printf("FAIL %s errno=%d\n", name, errno);
  }
}

static void Observe(const char* label, int fd) {
#ifdef _WIN32
  using Query = LONG(WINAPI*)(HANDLE, PIO_STATUS_BLOCK, void*, ULONG, FILE_INFORMATION_CLASS);
  const auto query = reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile")));
  IO_STATUS_BLOCK status{};
  FILE_PIPE_LOCAL_INFORMATION info{};
  LONG result = query(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), &status, &info, sizeof(info), FilePipeLocalInformation);
  std::printf("NPFS %s result=%ld state=%lu write_quota=%lu inbound_quota=%lu outbound_quota=%lu read_data=%lu\n", label,
      result, info.NamedPipeState, info.WriteQuotaAvailable, info.InboundQuota, info.OutboundQuota, info.ReadDataAvailable);
#else
  (void)label;
  (void)fd;
#endif
}

static void Readiness(const char* label, int fd, short asked, short expected) {
  pollfd descriptor{fd, asked, 0};
  const int ready = poll(&descriptor, 1, 0);
  std::printf("POLL %s actual=%#x expected=%#x result=%d\n", label,
      static_cast<unsigned>(descriptor.revents), static_cast<unsigned>(expected), ready);
  Check(label, ready == (expected != 0) && descriptor.revents == expected);
}

static void LastReader(bool full) {
  std::printf("FIXTURE %s\n", full ? "full" : "empty");
  int ends[2];
  const bool opened = pipe2(ends, O_NONBLOCK | O_CLOEXEC) == 0;
  Check("pipe2", opened);
  if (!opened) return;
  const int reader = dup(ends[0]);
  Check("duplicate reader", reader >= 0);
  if (reader < 0) {
    close(ends[0]);
    close(ends[1]);
    return;
  }
  char bytes[4096]{};
  if (full) {
    int count = 0;
    ssize_t result;
    do { result = write(ends[1], bytes, sizeof(bytes)); }
    while (result == sizeof(bytes) && ++count < 4096);
    Check("fill the actual pipe", count > 0 && result == -1 && errno == EAGAIN);
  }
  const short writable = full ? 0 : POLLOUT | POLLWRNORM;
  Observe("connected", ends[1]);
  Readiness("before either reader closes", ends[1], POLLOUT | POLLWRNORM, writable);
  Check("close original reader", close(ends[0]) == 0);
  Readiness("dup preserves reader lifetime", ends[1], POLLOUT | POLLWRNORM, writable);
  Check("close final reader", close(reader) == 0);
  Observe("disconnected", ends[1]);
  Readiness("last reader closure reports error independently of room", ends[1], POLLOUT | POLLWRNORM, writable | POLLERR);
  Readiness("last reader error ignores request mask", ends[1], 0, POLLERR);
  Check("write after final close reports EPIPE", write(ends[1], bytes, 1) == -1 && errno == EPIPE);
  Observe("after_failed_write", ends[1]);
  Readiness("EPIPE does not change write-side readiness", ends[1], POLLOUT | POLLWRNORM, writable | POLLERR);
  close(ends[1]);
}

int main() {
  signal(SIGPIPE, SIG_IGN);
  LastReader(false);
  LastReader(true);
  std::printf("anonymous pipe peer readiness: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
