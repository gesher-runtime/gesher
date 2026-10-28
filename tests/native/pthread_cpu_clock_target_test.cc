// Native independent Win32 brackets for the bionic retained-clock core.
// This does not validate the still-open winpthreads public host API.
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <errno.h>

namespace gesher::bionic {
uint64_t PthreadSelfId();
int PthreadCpuClock(uint64_t, int*);
int ReadPthreadCpuClock(int, int64_t*);
}

namespace {
struct Actor {
  std::atomic<uint64_t> id{0};
  std::atomic<bool> busy{false};
  HANDLE stop = nullptr;
};
DWORD WINAPI RunActor(void* raw) {
  auto& actor = *static_cast<Actor*>(raw);
  actor.id.store(gesher::bionic::PthreadSelfId());
  while (actor.busy.load() && WaitForSingleObject(actor.stop, 0) == WAIT_TIMEOUT) {
    for (unsigned i = 0; i < 10000; ++i) MemoryBarrier();
  }
  WaitForSingleObject(actor.stop, INFINITE);
  return 0;
}
int64_t NativeTime(HANDLE thread) {
  FILETIME c, e, k, u;
  if (!GetThreadTimes(thread, &c, &e, &k, &u)) return -1;
  auto ticks = [](FILETIME t) { return (uint64_t{t.dwHighDateTime} << 32) | t.dwLowDateTime; };
  return static_cast<int64_t>((ticks(k) + ticks(u)) * 100);
}
int failures;
void Check(bool ok, const char* name) {
  std::printf("%s  pthread target core: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++failures;
}
}

int main() {
  using namespace gesher::bionic;
  uint64_t self = PthreadSelfId();
  DWORD baseline = 0;
  GetProcessHandleCount(GetCurrentProcess(), &baseline);
  int untouched = 77;
  int64_t untouched_time = 99;
  Check(PthreadCpuClock(UINT64_MAX, &untouched) == ESRCH && untouched == 77 &&
            PthreadCpuClock(self, nullptr) == EINVAL &&
            ReadPthreadCpuClock(-1, &untouched_time) == EINVAL && untouched_time == 99,
        "invalid identities and IDs preserve outputs");
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (stop == nullptr) return 1;
  std::array<Actor, 257> actors;
  std::array<HANDLE, 257> handles{};
  std::array<int, 257> clocks{};
  size_t count = 0;
  for (; count < actors.size(); ++count) {
    actors[count].stop = stop;
    actors[count].busy.store(count == 0);
    handles[count] = CreateThread(nullptr, 65536, RunActor, &actors[count],
                                  STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (handles[count] == nullptr) break;
  }
  bool ready = count == actors.size();
  ULONGLONG deadline = GetTickCount64() + 15000;
  for (size_t i = 0; i < count; ++i) {
    while (actors[i].id.load() == 0 && GetTickCount64() < deadline) Sleep(1);
    ready &= actors[i].id.load() != 0;
  }
  Check(ready, "257 real external self identities remain live");
  bool capacity = ready;
  if (ready) {
    for (size_t i = 0; i < 256; ++i) capacity &= PthreadCpuClock(actors[i].id.load(), &clocks[i]) == 0;
    int unchanged = 77;
    capacity &= PthreadCpuClock(actors[256].id.load(), &unchanged) == EAGAIN && unchanged == 77;
  }
  Check(capacity, "256 live slots and resource exhaustion preserve output");
  if (capacity) {
    bool bracket = true;
    for (int i = 0; i < 128; ++i) {
      int64_t before = NativeTime(handles[0]), sample = -1;
      int result = ReadPthreadCpuClock(clocks[0], &sample);
      int64_t after = NativeTime(handles[0]);
      bracket &= result == 0 && before >= 0 && before <= sample && sample <= after;
      Sleep(1);
    }
    Check(bracket, "128 busy target samples match independent owned Win32 handle brackets");
    DWORD before = 0, after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &before);
    bool repeat = true;
    for (int i = 0; i < 1024; ++i) {
      int clock = 0;
      repeat &= PthreadCpuClock(actors[0].id.load(), &clock) == 0 && clock == clocks[0];
    }
    GetProcessHandleCount(GetCurrentProcess(), &after);
    Check(repeat && before == after, "1024 acquisitions allocate no additional handles at capacity");
  }
  for (size_t i = 0; i < count; ++i) actors[i].busy.store(false);
  SetEvent(stop);
  bool exited = true;
  deadline = GetTickCount64() + 15000;
  for (size_t i = 0; i < count; ++i) {
    ULONGLONG now = GetTickCount64();
    DWORD remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
    exited &= WaitForSingleObject(handles[i], remaining) == WAIT_OBJECT_0;
    CloseHandle(handles[i]);
  }
  Check(exited, "all external targets actually exit within one deadline");
  CloseHandle(stop);
  int self_clock = 0;
  Check(PthreadCpuClock(self, &self_clock) == 0, "exit reaping restores capacity");
  int64_t sample = -1;
  Check(capacity && ReadPthreadCpuClock(clocks[0], &sample) == EINVAL,
        "retired ID cannot alias the newly acquired caller clock");
  DWORD remaining = 0;
  GetProcessHandleCount(GetCurrentProcess(), &remaining);
  Check(remaining <= baseline + 2, "terminated external and clock handles are reclaimed");
  std::printf("INFO  pthread target core: handles baseline=%lu remaining=%lu\n",
              static_cast<unsigned long>(baseline), static_cast<unsigned long>(remaining));
  std::printf("%d test(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}
