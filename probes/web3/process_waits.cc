// CI-only out-of-process observer; never loaded into the observed ART process.
#include <windows.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "thread_watch.h"

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  if (std::strcmp(argv[1], "--hold") == 0) {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) return 3;
    std::printf("WEB3_OBSERVER_HOLD pid=%lu tid=%lu\n", GetCurrentProcessId(), GetCurrentThreadId());
    std::fflush(stdout);
    const DWORD waited = WaitForSingleObject(event, 60000);
    CloseHandle(event);
    return waited == WAIT_TIMEOUT ? 0 : 4;
  }
  char* end = nullptr;
  errno = 0;
  const unsigned long parsed = std::strtoul(argv[1], &end, 10);
  if (errno != 0 || end == argv[1] || *end != 0 || parsed == 0 ||
      parsed == GetCurrentProcessId()) return 2;
  std::fprintf(stderr, "WEB3_OBSERVER_BEGIN pid=%lu\n", parsed);
  std::fflush(stderr);
  gesher::input::DumpProcessThreadWaits(static_cast<uint32_t>(parsed));
  std::fprintf(stderr, "WEB3_OBSERVER_END pid=%lu\n", parsed);
  return 0;
}
